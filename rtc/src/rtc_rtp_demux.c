/*
 * SSRC -> consumer demultiplexer for inbound RTP. See rtc_rtp_demux.h.
 */
#include "rtc_rtp_demux.h"

rtc_err_t rtc_rtp_demux_init(rtc_rtp_demux_t *d, rtc_rtp_sink_fn sink, rtc_rtp_resolve_fn resolve,
                             void *resolve_user) {
    if (!d || !sink)
        return RTC_ERR_INVALID;
    rtc_err_t rc = rtc_u32_map_init(&d->by_ssrc);
    if (rc != RTC_OK)
        return rc;
    d->sink = sink;
    d->resolve = resolve;
    d->resolve_user = resolve_user;
    d->ready = true;
    return RTC_OK;
}

void rtc_rtp_demux_close(rtc_rtp_demux_t *d) {
    if (!d || !d->ready)
        return;
    rtc_u32_map_free(&d->by_ssrc);
    d->ready = false;
}

rtc_err_t rtc_rtp_demux_bind(rtc_rtp_demux_t *d, uint32_t ssrc, void *user) {
    if (!d || !d->ready || !user)
        return RTC_ERR_INVALID;
    return rtc_u32_map_set(&d->by_ssrc, ssrc, user);
}

void rtc_rtp_demux_unbind(rtc_rtp_demux_t *d, uint32_t ssrc) {
    if (d && d->ready)
        rtc_u32_map_remove(&d->by_ssrc, ssrc);
}

void *rtc_rtp_demux_get(const rtc_rtp_demux_t *d, uint32_t ssrc) {
    return (d && d->ready) ? rtc_u32_map_get(&d->by_ssrc, ssrc) : NULL;
}

bool rtc_rtp_demux_dispatch(rtc_rtp_demux_t *d, const rtc_rtp_packet_t *pkt) {
    if (!d || !d->ready || !pkt)
        return false;
    void *user = rtc_u32_map_get(&d->by_ssrc, pkt->header.ssrc);
    if (!user && d->resolve) {
        user = d->resolve(pkt, d->resolve_user);
        if (user)
            (void)rtc_u32_map_set(&d->by_ssrc, pkt->header.ssrc, user);
    }
    if (!user)
        return false;
    d->sink(pkt, user);
    return true;
}
