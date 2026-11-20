/*
 * Per-peer RTP/RTCP media session. See rtc_media_session.h.
 */
#include "rtc/rtc_media_session.h"

#include "rtc/rtc_rtcp.h"
#include "rtc/rtc_rtp_ext.h"
#include "rtc_worker_internal.h"
#include "rtc_transport_internal.h"

#include <stdlib.h>
#include <string.h>

#define RTC_MEDIA_SESSION_FEEDBACK_MS 20

/* ---- Transport RTP router: deliver + resolve ---- */

static void session_rtp_sink(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_rtp_recv_stream_on_packet((rtc_rtp_recv_stream_t *)user, pkt);
}

static rtc_rtp_recv_stream_t *session_recv_at(rtc_media_session_t *s, size_t i) {
    rtc_rtp_recv_stream_t **slot = (rtc_rtp_recv_stream_t **)rtc_vec_at(&s->recv_streams, i);
    return slot ? *slot : NULL;
}

static bool session_source_retired(rtc_media_session_t *s, uint32_t ssrc, uint64_t now) {
    if (s->retirement_overflow)
        return true;
    for (size_t i = 0; i < RTC_MEDIA_SESSION_RETIRED_MAX; i++)
        if (s->retired[i].ssrc == ssrc && s->retired[i].until_ms > now)
            return true;
    return false;
}

static void session_retire_source(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream,
                                  uint64_t now) {
    uint32_t ssrc = rtc_rtp_recv_stream_ssrc(stream);
    uint64_t grace = rtc_rtcp_source_timeout_ms(rtc_rtp_recv_stream_stats(stream));
    bool recorded = false;
    for (size_t i = 0; i < RTC_MEDIA_SESSION_RETIRED_MAX; i++) {
        if (!s->retired[i].ssrc || s->retired[i].ssrc == ssrc || s->retired[i].until_ms <= now) {
            s->retired[i].ssrc = ssrc;
            s->retired[i].until_ms = grace > UINT64_MAX - now ? UINT64_MAX : now + grace;
            recorded = true;
            break;
        }
    }
    if (!recorded) {
        RTC_LOG_ERR("Too many recently retired RTP sources");
        s->retirement_overflow = true;
        rtc_media_session_stop(s);
        if (s->transport)
            rtc_transport_close(s->transport);
    }
    if (s->transport)
        rtc_transport_unbind_rtp(s->transport, ssrc);
    rtc_rtp_recv_stream_set_ssrc(stream, 0);
}

static rtc_rtp_recv_stream_t *session_select_source(rtc_media_session_t *s,
                                                    rtc_rtp_recv_stream_t *stream, uint32_t ssrc) {
    uint32_t previous = rtc_rtp_recv_stream_ssrc(stream);
    if (previous && previous != ssrc)
        session_retire_source(s, stream, rtc_time_ms());
    if (s->retirement_overflow)
        return NULL;
    rtc_rtp_recv_stream_set_ssrc(stream, ssrc);
    return stream;
}

rtc_rtp_recv_stream_t *rtc_media_session_resolve(rtc_media_session_t *s,
                                                 const rtc_rtp_packet_t *pkt) {
    if (!s || !s->ready || !pkt)
        return NULL;
    if (!pkt->header.ssrc || session_source_retired(s, pkt->header.ssrc, rtc_time_ms()))
        return NULL;
    size_t n = rtc_vec_len(&s->recv_streams);

    /* 1. MID match: when a MID extension is negotiated and present, route to the
     * receive stream whose MID matches. Disambiguates bundled m-sections that
     * share a payload type (RFC 8843/8852). */
    if (s->mid_ext_id && pkt->header.extension && pkt->ext_data && pkt->ext_len) {
        char mid[RTC_RTP_MID_MAX];
        if (rtc_rtp_ext_get_string(pkt->ext_data, pkt->ext_len, s->mid_ext_id, mid, sizeof(mid)) >
            0) {
            bool negotiated = false;
            for (size_t i = 0; i < n; i++) {
                rtc_rtp_recv_stream_t *rs = session_recv_at(s, i);
                negotiated |= rtc_rtp_recv_stream_has_mid(rs) &&
                              rtc_rtp_recv_stream_can_receive(rs, pkt->header.payload_type);
                if (rs && rtc_rtp_recv_stream_mid_matches(rs, mid) &&
                    rtc_rtp_recv_stream_can_receive(rs, pkt->header.payload_type)) {
                    return session_select_source(s, rs, pkt->header.ssrc);
                }
            }
            if (negotiated)
                return NULL;
        }
    }

    /* 2. Payload-type match (peers that omit a=ssrc and MID). */
    rtc_rtp_recv_stream_t *match = NULL;
    for (size_t i = 0; i < n; i++) {
        rtc_rtp_recv_stream_t *rs = session_recv_at(s, i);
        if (rs && rtc_rtp_recv_stream_can_receive(rs, pkt->header.payload_type)) {
            if (match)
                return NULL;
            match = rs;
        }
    }
    return match ? session_select_source(s, match, pkt->header.ssrc) : NULL;
}

/* First packet for an unbound SSRC: hand the transport demux the resolved
 * receive stream to auto-bind. Handles peers that omit a=ssrc. */
static void *session_rtp_resolve(const rtc_rtp_packet_t *pkt, void *user) {
    return rtc_media_session_resolve((rtc_media_session_t *)user, pkt);
}

/* ---- Inbound RTCP routing ---- */

static rtc_rtp_send_stream_t *session_sender(rtc_media_session_t *s, uint32_t ssrc) {
    return (rtc_rtp_send_stream_t *)rtc_u32_map_get(&s->send_streams, ssrc);
}

static rtc_rtp_recv_stream_t *session_receiver(rtc_media_session_t *s, uint32_t ssrc) {
    if (!ssrc)
        return NULL;
    for (size_t i = 0; i < rtc_vec_len(&s->recv_streams); i++) {
        rtc_rtp_recv_stream_t *stream = session_recv_at(s, i);
        if (rtc_rtp_recv_stream_ssrc(stream) == ssrc)
            return stream;
    }
    return NULL;
}

static void session_handle_bye(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    rtc_rtcp_bye_t bye;
    if (rtc_rtcp_parse_bye(&bye, buf, len) != RTC_OK)
        return;
    for (unsigned i = 0; i < bye.count; i++) {
        rtc_rtp_recv_stream_t *stream = session_receiver(s, bye.ssrcs[i]);
        if (stream)
            session_retire_source(s, stream, rtc_time_ms());
    }
}

static void session_handle_sr(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, len) != RTC_OK)
        return;
    rtc_rtp_recv_stream_t *rs = session_receiver(s, rtcp.sender_ssrc);
    if (rs)
        rtc_rtp_recv_stream_on_sr(rs, &rtcp.sr);
}

static void session_handle_rr(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, len) != RTC_OK)
        return;
    rtc_rtp_recv_stream_on_rtcp_activity(session_receiver(s, rtcp.sender_ssrc), rtc_time_ms());
#ifdef MRTC_ENABLE_TWCC
    /* The loss-based estimator is transport-wide, but an RR can report several
     * SSRCs with different loss; feed the worst so congestion control reacts to
     * the most-affected stream rather than whichever happens to be first. */
    uint8_t worst_loss = 0;
    for (int i = 0; i < rtcp.report_count; i++) {
        if (rtcp.reports[i].fraction_lost > worst_loss)
            worst_loss = rtcp.reports[i].fraction_lost;
    }
    if (rtcp.report_count > 0)
        rtc_transport_report_rtcp_loss(s->transport, worst_loss);
#endif
    for (int i = 0; i < rtcp.report_count; i++) {
        const rtc_rtcp_rr_block_t *rr = &rtcp.reports[i];
        rtc_rtp_send_stream_t *ss = session_sender(s, rr->ssrc);
        if (ss)
            (void)rtc_rtp_send_stream_on_rr(
                ss, rr,
                rtc_rtcp_stats_rtt_from_rr(rtc_rtp_send_stream_stats(ss), rr, rtc_time_ms()));
    }
}

static void session_handle_nack(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    rtc_rtcp_nack_t nack;
    if (rtc_rtcp_parse_nack(&nack, buf, len) != RTC_OK)
        return;
    rtc_rtp_send_stream_t *ss = session_sender(s, nack.media_ssrc);
    if (ss)
        rtc_rtp_send_stream_handle_nack(ss, nack.lost_seqs, nack.lost_count);
}

/* PLI (RFC 4585) and FIR (RFC 5104) both request a keyframe; only media_ssrc
 * is consumed, so they share routing. */
static void session_handle_psfb(rtc_media_session_t *s, uint8_t fmt, const uint8_t *buf,
                                size_t len) {
    if (fmt == 15) {
        rtc_rtcp_remb_t feedback;
        if (rtc_rtcp_parse_remb(&feedback, buf, len) == RTC_OK) {
            rtc_rtp_send_stream_t *sender = session_sender(s, feedback.media_ssrc);
            if (sender)
                rtc_rtp_send_stream_on_remb(sender, feedback.bitrate_bps);
        }
        return;
    }
    uint32_t media_ssrc;
    if (fmt == RTCP_FMT_PLI) {
        rtc_rtcp_pli_t pli;
        if (rtc_rtcp_parse_pli(&pli, buf, len) != RTC_OK)
            return;
        media_ssrc = pli.media_ssrc;
    } else if (fmt == RTCP_FMT_FIR) {
        rtc_rtcp_fir_t fir;
        if (rtc_rtcp_parse_fir(&fir, buf, len) != RTC_OK)
            return;
        media_ssrc = fir.media_ssrc;
    } else {
        return;
    }
    rtc_rtp_send_stream_t *ss = session_sender(s, media_ssrc);
    if (ss)
        rtc_rtp_send_stream_handle_pli(ss);
}

/* ---- Built-in interceptors ----
 *
 * Each built-in is a thin handler over the session: the report interceptor
 * routes SR / RR and emits periodic SR / RR; the NACK and PLI interceptors route
 * the matching feedback to the named send stream. They share one struct and
 * differ only by ops, so the RTCP plane is an ordered, extensible chain rather
 * than a hardcoded switch. */

typedef struct {
    rtc_interceptor_t base;
    rtc_media_session_t *s;
} session_interceptor_t;

static rtc_media_session_t *si_session(rtc_interceptor_t *it) {
    return ((session_interceptor_t *)it)->s;
}

static void report_intercept_rtcp(rtc_interceptor_t *it, uint8_t pt, uint8_t fmt,
                                  const uint8_t *buf, size_t len) {
    (void)fmt;
    rtc_media_session_t *s = si_session(it);
    if (pt == RTCP_PT_SR) {
        session_handle_sr(s, buf, len);
        session_handle_rr(s, buf, len);
    } else if (pt == RTCP_PT_RR)
        session_handle_rr(s, buf, len);
    else if (pt == RTCP_PT_BYE)
        session_handle_bye(s, buf, len);
}

static void report_intercept_tick(rtc_interceptor_t *it, uint64_t now_ms) {
    (void)now_ms;
    rtc_media_session_t *s = si_session(it);
    rtc_u32_map_iter_t mi = {0};
    uint32_t ssrc;
    void *val;
    size_t n = rtc_vec_len(&s->recv_streams);
    while (rtc_u32_map_next(&s->send_streams, &mi, &ssrc, &val)) {
        rtc_rtp_recv_stream_t *receiver = NULL;
        for (size_t i = 0; i < n; i++) {
            rtc_rtp_recv_stream_t **slot = rtc_vec_at(&s->recv_streams, i);
            if (slot && *slot && rtc_rtp_recv_stream_stats(*slot)->ssrc == ssrc) {
                receiver = *slot;
                break;
            }
        }
        rtc_rtp_send_stream_emit_report(val, receiver, s->transport);
    }
    for (size_t i = 0; i < n; i++) {
        rtc_rtp_recv_stream_t **slot = (rtc_rtp_recv_stream_t **)rtc_vec_at(&s->recv_streams, i);
        if (slot && *slot) {
            rtc_rtp_send_stream_t *sender =
                rtc_u32_map_get(&s->send_streams, rtc_rtp_recv_stream_stats(*slot)->ssrc);
            if (rtc_rtp_send_stream_is_active(sender) &&
                rtc_rtp_send_stream_stats(sender)->packets_sent)
                continue;
            rtc_rtp_recv_stream_emit_rr(*slot, s->transport);
        }
    }
}

static void nack_intercept_rtcp(rtc_interceptor_t *it, uint8_t pt, uint8_t fmt, const uint8_t *buf,
                                size_t len) {
    if (pt == RTCP_PT_RTPFB && fmt == RTCP_FMT_NACK)
        session_handle_nack(si_session(it), buf, len);
}

static void pli_intercept_rtcp(rtc_interceptor_t *it, uint8_t pt, uint8_t fmt, const uint8_t *buf,
                               size_t len) {
    if (pt == RTCP_PT_PSFB)
        session_handle_psfb(si_session(it), fmt, buf, len);
}

static void session_interceptor_destroy(rtc_interceptor_t *it) {
    free(it);
}

static const rtc_interceptor_ops_t report_ops = {
    .name = "report",
    .on_rtcp = report_intercept_rtcp,
    .on_tick = report_intercept_tick,
    .destroy = session_interceptor_destroy,
};
static const rtc_interceptor_ops_t nack_ops = {
    .name = "nack",
    .on_rtcp = nack_intercept_rtcp,
    .destroy = session_interceptor_destroy,
};
static const rtc_interceptor_ops_t pli_ops = {
    .name = "pli",
    .on_rtcp = pli_intercept_rtcp,
    .destroy = session_interceptor_destroy,
};

static rtc_err_t session_add_builtin(rtc_media_session_t *s, const rtc_interceptor_ops_t *ops) {
    session_interceptor_t *si = (session_interceptor_t *)calloc(1, sizeof(*si));
    if (!si)
        return RTC_ERR_NOMEM;
    si->base.ops = ops;
    si->s = s;
    rtc_err_t rc = rtc_interceptor_chain_add(&s->chain, &si->base);
    if (rc != RTC_OK)
        free(si);
    return rc;
}

static rtc_err_t session_install_default_interceptors(rtc_media_session_t *s) {
    rtc_err_t rc = session_add_builtin(s, &report_ops);
    if (rc == RTC_OK)
        rc = session_add_builtin(s, &nack_ops);
    if (rc == RTC_OK)
        rc = session_add_builtin(s, &pli_ops);
    return rc;
}

/* Walk a compound RTCP packet and dispatch each sub-packet through the
 * interceptor chain. Real peers bundle SR/RR + SDES + feedback (NACK/PLI/TWCC)
 * into one datagram, so dispatching only the first sub-packet would drop the
 * rest. Each RTCP header carries a 16-bit length in 32-bit words minus one
 * (RFC 3550 §6.4.1), i.e. (length + 1) * 4 bytes. */
void rtc_media_session_handle_rtcp(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    if (!s || !s->ready || !buf)
        return;
    if (rtc_rtcp_validate_compound(buf, len) != RTC_OK) {
        RTC_LOG_WARN("Malformed compound RTCP packet");
        return;
    }
    size_t offset = 0;
    while (offset + 4 <= len) {
        if (((buf[offset] >> 6) & 0x03) != 2)
            break;
        size_t sub_len = (((size_t)buf[offset + 2] << 8 | buf[offset + 3]) + 1) * 4;
        if (offset + sub_len > len)
            break;
        uint8_t pt, fmt;
        if (rtc_rtcp_get_pt_fmt(buf + offset, sub_len, &pt, &fmt))
            rtc_interceptor_chain_on_rtcp(&s->chain, pt, fmt, buf + offset, sub_len);
        offset += sub_len;
    }
}

static void session_on_rtcp(const uint8_t *data, size_t len, void *user) {
    rtc_media_session_handle_rtcp((rtc_media_session_t *)user, data, len);
}

/* ---- Periodic SR / RR emission ---- */

static void session_report_timer(void *user) {
    rtc_media_session_t *s = (rtc_media_session_t *)user;
    s->report_timer = RTC_WORKER_TIMER_INVALID;
    if (!atomic_load_explicit(&s->running, memory_order_acquire))
        return;

    uint64_t now = rtc_time_ms();
    for (size_t i = 0; i < rtc_vec_len(&s->recv_streams); i++)
        rtc_rtp_recv_stream_tick(session_recv_at(s, i), now);
    rtc_interceptor_chain_tick(&s->chain, now);

    if (atomic_load_explicit(&s->running, memory_order_acquire)) {
        s->report_timer = rtc_worker_add_timer(
            s->worker, rtc_time_ms() + RTC_MEDIA_SESSION_FEEDBACK_MS, session_report_timer, s);
        if (s->report_timer == RTC_WORKER_TIMER_INVALID) {
            atomic_store(&s->running, false);
            RTC_LOG_ERR("Media session report timer allocation failed");
            rtc_transport_close(s->transport);
        }
    }
}

/* ---- Lifecycle ---- */

rtc_err_t rtc_media_session_init(rtc_media_session_t *s, rtc_transport_t *transport,
                                 rtc_worker_t *worker) {
    if (!s)
        return RTC_ERR_INVALID;
    rtc_err_t rc = rtc_u32_map_init(&s->send_streams);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_vec_init(&s->recv_streams, sizeof(rtc_rtp_recv_stream_t *));
    if (rc != RTC_OK) {
        rtc_u32_map_free(&s->send_streams);
        return rc;
    }
    s->transport = transport;
    s->worker = worker;
    s->report_timer = RTC_WORKER_TIMER_INVALID;
    s->mid_ext_id = 0;
    memset(s->retired, 0, sizeof(s->retired));
    s->retirement_overflow = false;
    atomic_store_explicit(&s->running, false, memory_order_relaxed);
    s->ready = true;

    rtc_interceptor_chain_init(&s->chain);
    rc = session_install_default_interceptors(s);
    if (rc != RTC_OK) {
        rtc_interceptor_chain_close(&s->chain);
        rtc_u32_map_free(&s->send_streams);
        rtc_vec_free(&s->recv_streams);
        s->ready = false;
        return rc;
    }

    if (transport) {
        rtc_transport_set_rtp_router(transport, session_rtp_sink, session_rtp_resolve, s);
        rtc_transport_on_rtcp(transport, session_on_rtcp, s);
    }
    return RTC_OK;
}

static void session_local_source_changed(uint32_t previous, uint32_t current, void *user) {
    rtc_media_session_t *s = user;
    rtc_rtp_send_stream_t *stream = session_sender(s, previous);
    if (!stream || rtc_u32_map_set(&s->send_streams, current, stream) != RTC_OK) {
        rtc_transport_close(s->transport);
        return;
    }
    rtc_u32_map_remove(&s->send_streams, previous);
    for (size_t i = 0; i < rtc_vec_len(&s->recv_streams); i++) {
        rtc_rtp_recv_stream_t *receiver = session_recv_at(s, i);
        if (rtc_rtp_recv_stream_stats(receiver)->ssrc == previous)
            rtc_rtp_recv_stream_set_local_ssrc(receiver, current);
    }
}

rtc_err_t rtc_media_session_add_sender(rtc_media_session_t *s, rtc_rtp_send_stream_t *stream) {
    if (!s || !s->ready || !stream)
        return RTC_ERR_INVALID;
    rtc_rtp_send_stream_t *existing = session_sender(s, rtc_rtp_send_stream_ssrc(stream));
    if (existing)
        return existing == stream ? RTC_OK : RTC_ERR_INVALID;
    rtc_err_t rc = rtc_u32_map_set(&s->send_streams, rtc_rtp_send_stream_ssrc(stream), stream);
    if (rc == RTC_OK) {
        rtc_rtp_send_stream_on_source(stream, session_local_source_changed, s);
        rtc_rtp_send_stream_attach_transport(stream, s->transport);
    }
    return rc;
}

static int session_local_reporter_changed(uint32_t previous, uint32_t current, void *user) {
    (void)previous;
    rtc_rtp_recv_stream_set_local_ssrc(user, current);
    return RTC_OK;
}

rtc_err_t rtc_media_session_add_receiver(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream) {
    if (!s || !s->ready || !stream)
        return RTC_ERR_INVALID;
    rtc_rtp_recv_stream_attach_transport(stream, s->transport);
    uint32_t local = rtc_rtp_recv_stream_stats(stream)->ssrc;
    if (s->transport && !session_sender(s, local)) {
        int rc = local ? RTC_OK : rtc_transport_new_ssrc(s->transport, &local);
        if (rc == RTC_OK)
            rc = rtc_transport_register_source(s->transport, local, session_local_reporter_changed,
                                               stream, NULL);
        if (rc != RTC_OK)
            return rc;
        rtc_rtp_recv_stream_set_local_ssrc(stream, local);
    }
    rtc_err_t rc = rtc_vec_push(&s->recv_streams, &stream);
    if (rc != RTC_OK)
        rtc_transport_unregister_source(s->transport, stream);
    return rc;
}

void rtc_media_session_bind_receiver(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream,
                                     uint32_t ssrc) {
    if (!s || !s->ready || !stream)
        return;
    if (ssrc) {
        rtc_rtp_recv_stream_t *other = session_receiver(s, ssrc);
        if (other && other != stream) {
            RTC_LOG_ERR("Duplicate RTP receive source 0x%08x", ssrc);
            rtc_media_session_stop(s);
            if (s->transport)
                rtc_transport_close(s->transport);
            return;
        }
    }
    uint32_t previous = rtc_rtp_recv_stream_ssrc(stream);
    if (previous && previous != ssrc) {
        session_retire_source(s, stream, rtc_time_ms());
        if (s->retirement_overflow)
            return;
    }
    for (size_t i = 0; i < RTC_MEDIA_SESSION_RETIRED_MAX; i++)
        if (s->retired[i].ssrc == ssrc)
            s->retired[i].ssrc = 0;
    rtc_rtp_recv_stream_set_ssrc(stream, ssrc);
    if (s->transport && ssrc)
        rtc_transport_bind_rtp(s->transport, ssrc, stream);
}

void rtc_media_session_set_mid_ext_id(rtc_media_session_t *s, uint8_t ext_id) {
    if (s && s->ready)
        s->mid_ext_id = ext_id;
}

rtc_err_t rtc_media_session_add_interceptor(rtc_media_session_t *s, rtc_interceptor_t *it) {
    if (!s || !s->ready || !it)
        return RTC_ERR_INVALID;
    return rtc_interceptor_chain_add(&s->chain, it);
}

static void session_start_on_worker(void *user) {
    rtc_media_session_t *s = user;
    if (atomic_exchange_explicit(&s->running, true, memory_order_acq_rel))
        return;
    if (s->report_timer == RTC_WORKER_TIMER_INVALID) {
        s->report_timer = rtc_worker_add_timer(
            s->worker, rtc_time_ms() + RTC_MEDIA_SESSION_FEEDBACK_MS, session_report_timer, s);
        if (s->report_timer == RTC_WORKER_TIMER_INVALID) {
            atomic_store(&s->running, false);
            RTC_LOG_ERR("Media session report timer allocation failed");
            rtc_transport_close(s->transport);
        }
    }
}

void rtc_media_session_start(rtc_media_session_t *s) {
    if (!s || !s->ready || !s->worker || !s->transport)
        return;
    rtc_worker_invoke(s->worker, session_start_on_worker, s);
}

static void session_stop_on_worker(void *user) {
    rtc_media_session_t *s = user;
    atomic_store_explicit(&s->running, false, memory_order_release);
    if (s->worker && s->report_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(s->worker, s->report_timer);
        s->report_timer = RTC_WORKER_TIMER_INVALID;
    }
}

void rtc_media_session_stop(rtc_media_session_t *s) {
    if (!s || !s->ready)
        return;
    if (s->worker)
        rtc_worker_invoke(s->worker, session_stop_on_worker, s);
    else
        session_stop_on_worker(s);
}

void rtc_media_session_close(rtc_media_session_t *s) {
    if (!s || !s->ready)
        return;
    rtc_u32_map_iter_t iterator = {0};
    uint32_t ssrc;
    void *value;
    while (rtc_u32_map_next(&s->send_streams, &iterator, &ssrc, &value))
        rtc_rtp_send_stream_on_source(value, NULL, NULL);
    rtc_interceptor_chain_close(&s->chain);
    rtc_u32_map_free(&s->send_streams);
    rtc_vec_free(&s->recv_streams);
    s->ready = false;
}

void rtc_media_session_send_bye(rtc_media_session_t *s) {
    if (!s || !s->ready || !s->transport)
        return;
    rtc_u32_map_iter_t iterator = {0};
    uint32_t ssrc;
    void *value;
    while (rtc_u32_map_next(&s->send_streams, &iterator, &ssrc, &value)) {
        const rtc_rtcp_stats_t *stats = rtc_rtp_send_stream_stats(value);
        if (stats && stats->packets_sent) {
            int rc = rtc_transport_send_bye(s->transport, ssrc);
            if (rc != RTC_OK)
                RTC_LOG_DBG("RTCP BYE unavailable on retiring source: %d", rc);
        }
    }
}
