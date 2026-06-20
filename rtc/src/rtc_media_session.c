/*
 * Per-peer RTP/RTCP media session. See rtc_media_session.h.
 */
#include "rtc/rtc_media_session.h"

#include "rtc/rtc_rtcp.h"

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
    if (rtcp.report_count > 0)
        rtc_transport_report_rtcp_loss(s->transport, rtcp.reports[0].fraction_lost);
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

void rtc_media_session_handle_rtcp(rtc_media_session_t *s, const uint8_t *buf, size_t len) {
    if (!s || !s->ready || !buf || len <= 8)
        return;
    uint8_t pt, fmt;
    if (!rtc_rtcp_get_pt_fmt(buf, len, &pt, &fmt))
        return;
    switch (pt) {
        case RTCP_PT_SR:
            session_handle_sr(s, buf, len);
            break;
        case RTCP_PT_RR:
            session_handle_rr(s, buf, len);
            break;
        case RTCP_PT_RTPFB:
            if (fmt == RTCP_FMT_NACK)
                session_handle_nack(s, buf, len);
            break;
        case RTCP_PT_PSFB:
            session_handle_psfb(s, fmt, buf, len);
            break;
        default:
            break;
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

    rtc_u32_map_iter_t it = {0};
    uint32_t ssrc;
    void *val;
    while (rtc_u32_map_next(&s->send_streams, &it, &ssrc, &val))
        rtc_rtp_send_stream_emit_sr((rtc_rtp_send_stream_t *)val, s->transport);

    size_t n = rtc_vec_len(&s->recv_streams);
    for (size_t i = 0; i < n; i++) {
        rtc_rtp_recv_stream_t **slot = (rtc_rtp_recv_stream_t **)rtc_vec_at(&s->recv_streams, i);
        if (slot && *slot)
            rtc_rtp_recv_stream_emit_rr(*slot, s->transport);
    }

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
    rtc_u32_map_free(&s->send_streams);
    rtc_vec_free(&s->recv_streams);
    s->ready = false;
}
