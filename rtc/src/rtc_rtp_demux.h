/*
 * SSRC -> consumer demultiplexer for inbound RTP.
 *
 * Owns one SSRC->user table, a uniform delivery sink, and an optional
 * resolver that maps a first-seen SSRC to a consumer (typically by payload
 * type) and auto-binds it for O(1) dispatch thereafter.
 *
 * Not thread safe: the owner serializes bind / unbind / dispatch. The runtime
 * transport drives all three from its worker loop thread, so no per-demux lock
 * is needed. This is the single routing primitive shared by the client peer
 * connection (SSRC -> receive stream) and the SFU router (SSRC -> producer).
 */
#ifndef RTC_RTP_DEMUX_H
#define RTC_RTP_DEMUX_H

#include "rtc/rtc_rtp.h"
#include "rtc/rtc_u32_map.h"

typedef struct {
    rtc_u32_map_t by_ssrc;
    rtc_rtp_sink_fn sink;
    rtc_rtp_resolve_fn resolve;
    void *resolve_user;
    bool ready;
} rtc_rtp_demux_t;

/* Initialize an empty demux. `sink` is required; `resolve` may be NULL. */
rtc_err_t rtc_rtp_demux_init(rtc_rtp_demux_t *d, rtc_rtp_sink_fn sink, rtc_rtp_resolve_fn resolve,
                             void *resolve_user);

/* Free the backing table. Safe on zero-init and idempotent. */
void rtc_rtp_demux_close(rtc_rtp_demux_t *d);

rtc_err_t rtc_rtp_demux_bind(rtc_rtp_demux_t *d, uint32_t ssrc, void *user);
void rtc_rtp_demux_unbind(rtc_rtp_demux_t *d, uint32_t ssrc);
void *rtc_rtp_demux_get(const rtc_rtp_demux_t *d, uint32_t ssrc);

/* Route one parsed RTP packet: look up by SSRC (resolving + binding on a
 * miss), then deliver via the sink. Returns true if delivered. */
bool rtc_rtp_demux_dispatch(rtc_rtp_demux_t *d, const rtc_rtp_packet_t *pkt);

#endif /* RTC_RTP_DEMUX_H */
