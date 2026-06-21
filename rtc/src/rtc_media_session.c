/*
 * Per-peer RTP/RTCP media session. See rtc_media_session.h.
 */
#include "rtc/rtc_media_session.h"

#include "rtc/rtc_rtcp.h"

#include <stdlib.h>

#define RTC_MEDIA_SESSION_REPORT_MS 5000

/* ---- Transport RTP router: deliver + resolve ---- */

static void session_rtp_sink(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_rtp_recv_stream_on_packet((rtc_rtp_recv_stream_t *)user, pkt);
}

/* First packet for an unbound SSRC: match a receive stream by payload type and
 * return it for the transport to bind. Handles peers that omit a=ssrc. */
static void *session_rtp_resolve(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_media_session_t *s = (rtc_media_session_t *)user;
    size_t n = rtc_vec_len(&s->recv_streams);
    for (size_t i = 0; i < n; i++) {
        rtc_rtp_recv_stream_t **slot = (rtc_rtp_recv_stream_t **)rtc_vec_at(&s->recv_streams, i);
        rtc_rtp_recv_stream_t *rs = slot ? *slot : NULL;
        if (rs && rtc_rtp_recv_stream_can_receive(rs, pkt->header.payload_type)) {
            rtc_rtp_recv_stream_set_ssrc(rs, pkt->header.ssrc);
            return rs;
        }
    }
    return NULL;
}

/* ---- Inbound RTCP routing ---- */

static rtc_rtp_send_stream_t *session_sender(rtc_media_session_t *s, uint32_t ssrc) {
    return (rtc_rtp_send_stream_t *)rtc_u32_map_get(&s->send_streams, ssrc);
}

static void session_handle_sr(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, len) != RTC_OK)
        return;
    rtc_rtp_recv_stream_t *rs =
        (rtc_rtp_recv_stream_t *)rtc_transport_rtp_bound(s->transport, rtcp.sender_ssrc);
    if (rs)
        rtc_rtp_recv_stream_on_sr(rs, &rtcp.sr);
}

static void session_handle_rr(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, len) != RTC_OK)
        return;
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
            (void)rtc_rtp_send_stream_on_rr(ss, rr, rtc_rtcp_rtt_from_rr(rr));
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
    if (pt == RTCP_PT_SR)
        session_handle_sr(s, buf, len);
    else if (pt == RTCP_PT_RR)
        session_handle_rr(s, buf, len);
}

static void report_intercept_tick(rtc_interceptor_t *it, uint64_t now_ms) {
    (void)now_ms;
    rtc_media_session_t *s = si_session(it);
    rtc_u32_map_iter_t mi = {0};
    uint32_t ssrc;
    void *val;
    while (rtc_u32_map_next(&s->send_streams, &mi, &ssrc, &val))
        rtc_rtp_send_stream_emit_sr((rtc_rtp_send_stream_t *)val, s->transport);
    size_t n = rtc_vec_len(&s->recv_streams);
    for (size_t i = 0; i < n; i++) {
        rtc_rtp_recv_stream_t **slot = (rtc_rtp_recv_stream_t **)rtc_vec_at(&s->recv_streams, i);
        if (slot && *slot)
            rtc_rtp_recv_stream_emit_rr(*slot, s->transport);
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

    rtc_interceptor_chain_tick(&s->chain, rtc_time_ms());

    s->report_timer = rtc_worker_add_timer(s->worker, rtc_time_ms() + RTC_MEDIA_SESSION_REPORT_MS,
                                           session_report_timer, s);
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

rtc_err_t rtc_media_session_add_sender(rtc_media_session_t *s, rtc_rtp_send_stream_t *stream) {
    if (!s || !s->ready || !stream)
        return RTC_ERR_INVALID;
    return rtc_u32_map_set(&s->send_streams, rtc_rtp_send_stream_ssrc(stream), stream);
}

rtc_err_t rtc_media_session_add_receiver(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream) {
    if (!s || !s->ready || !stream)
        return RTC_ERR_INVALID;
    return rtc_vec_push(&s->recv_streams, &stream);
}

void rtc_media_session_bind_receiver(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream,
                                     uint32_t ssrc) {
    if (!s || !s->ready || !stream)
        return;
    rtc_rtp_recv_stream_set_ssrc(stream, ssrc);
    if (s->transport)
        rtc_transport_bind_rtp(s->transport, ssrc, stream);
}

rtc_err_t rtc_media_session_add_interceptor(rtc_media_session_t *s, rtc_interceptor_t *it) {
    if (!s || !s->ready || !it)
        return RTC_ERR_INVALID;
    return rtc_interceptor_chain_add(&s->chain, it);
}

void rtc_media_session_start(rtc_media_session_t *s) {
    if (!s || !s->ready || !s->worker)
        return;
    if (atomic_exchange_explicit(&s->running, true, memory_order_acq_rel))
        return;
    if (s->report_timer == RTC_WORKER_TIMER_INVALID)
        s->report_timer = rtc_worker_add_timer(
            s->worker, rtc_time_ms() + RTC_MEDIA_SESSION_REPORT_MS, session_report_timer, s);
}

void rtc_media_session_stop(rtc_media_session_t *s) {
    if (!s || !s->ready)
        return;
    atomic_store_explicit(&s->running, false, memory_order_release);
    if (s->worker && s->report_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(s->worker, s->report_timer);
        s->report_timer = RTC_WORKER_TIMER_INVALID;
    }
}

void rtc_media_session_close(rtc_media_session_t *s) {
    if (!s || !s->ready)
        return;
    rtc_interceptor_chain_close(&s->chain);
    rtc_u32_map_free(&s->send_streams);
    rtc_vec_free(&s->recv_streams);
    s->ready = false;
}
