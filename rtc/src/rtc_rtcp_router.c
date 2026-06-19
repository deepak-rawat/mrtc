/*
 * RTCP feedback router. See rtc_rtcp_router.h.
 */
#include "rtc/rtc_rtcp_router.h"

#include "rtc/rtc_rtcp.h"

rtc_err_t rtc_rtcp_router_init(rtc_rtcp_router_t *r, rtc_transport_t *transport) {
    if (!r)
        return RTC_ERR_INVALID;
    rtc_err_t rc = rtc_u32_map_init(&r->send_streams);
    if (rc != RTC_OK)
        return rc;
    r->transport = transport;
    r->ready = true;
    return RTC_OK;
}

void rtc_rtcp_router_close(rtc_rtcp_router_t *r) {
    if (!r || !r->ready)
        return;
    rtc_u32_map_free(&r->send_streams);
    r->ready = false;
}

rtc_err_t rtc_rtcp_router_add_sender(rtc_rtcp_router_t *r, uint32_t ssrc,
                                     rtc_rtp_send_stream_t *stream) {
    if (!r || !r->ready || !stream)
        return RTC_ERR_INVALID;
    return rtc_u32_map_set(&r->send_streams, ssrc, stream);
}

void rtc_rtcp_router_remove_sender(rtc_rtcp_router_t *r, uint32_t ssrc) {
    if (r && r->ready)
        rtc_u32_map_remove(&r->send_streams, ssrc);
}

static rtc_rtp_send_stream_t *router_sender(rtc_rtcp_router_t *r, uint32_t ssrc) {
    return (rtc_rtp_send_stream_t *)rtc_u32_map_get(&r->send_streams, ssrc);
}

static void router_handle_sr(rtc_rtcp_router_t *r, const uint8_t *buf, size_t len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, len) != RTC_OK)
        return;
    rtc_rtp_recv_stream_t *rs =
        (rtc_rtp_recv_stream_t *)rtc_transport_rtp_bound(r->transport, rtcp.sender_ssrc);
    if (rs)
        rtc_rtp_recv_stream_on_sr(rs, &rtcp.sr);
}

static void router_handle_rr(rtc_rtcp_router_t *r, const uint8_t *buf, size_t len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, len) != RTC_OK)
        return;

#ifdef MRTC_ENABLE_TWCC
    if (rtcp.report_count > 0)
        rtc_transport_report_rtcp_loss(r->transport, rtcp.reports[0].fraction_lost);
#endif

    for (int i = 0; i < rtcp.report_count; i++) {
        const rtc_rtcp_rr_block_t *rr = &rtcp.reports[i];
        rtc_rtp_send_stream_t *ss = router_sender(r, rr->ssrc);
        if (ss)
            (void)rtc_rtp_send_stream_on_rr(ss, rr, rtc_rtcp_rtt_from_rr(rr));
    }
}

static void router_handle_nack(rtc_rtcp_router_t *r, const uint8_t *buf, size_t len) {
    rtc_rtcp_nack_t nack;
    if (rtc_rtcp_parse_nack(&nack, buf, len) != RTC_OK)
        return;
    rtc_rtp_send_stream_t *ss = router_sender(r, nack.media_ssrc);
    if (ss)
        rtc_rtp_send_stream_handle_nack(ss, nack.lost_seqs, nack.lost_count);
}

/* PLI (RFC 4585) and FIR (RFC 5104) both request a keyframe; only media_ssrc
 * is consumed, so they share routing. */
static void router_handle_psfb(rtc_rtcp_router_t *r, uint8_t fmt, const uint8_t *buf, size_t len) {
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
    rtc_rtp_send_stream_t *ss = router_sender(r, media_ssrc);
    if (ss)
        rtc_rtp_send_stream_handle_pli(ss);
}

void rtc_rtcp_router_handle(rtc_rtcp_router_t *r, const uint8_t *buf, size_t len) {
    if (!r || !r->ready || !buf || len <= 8)
        return;

    uint8_t pt, fmt;
    if (!rtc_rtcp_get_pt_fmt(buf, len, &pt, &fmt))
        return;

    switch (pt) {
        case RTCP_PT_SR:
            router_handle_sr(r, buf, len);
            break;
        case RTCP_PT_RR:
            router_handle_rr(r, buf, len);
            break;
        case RTCP_PT_RTPFB:
            if (fmt == RTCP_FMT_NACK)
                router_handle_nack(r, buf, len);
            break;
        case RTCP_PT_PSFB:
            router_handle_psfb(r, fmt, buf, len);
            break;
        default:
            break;
    }
}
