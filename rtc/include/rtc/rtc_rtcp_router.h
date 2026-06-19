/*
 * RTCP feedback router.
 *
 * Parses inbound (already SRTCP-unprotected) compound RTCP and routes each
 * report to the right RTP stream:
 *   - SR             -> the receive stream for the sender SSRC, resolved
 *                       through the owning transport's RTP demux.
 *   - RR             -> the send stream named by each report block (per-sender
 *                       AIMD rate control); RR fraction-lost is also fed into
 *                       the transport's bandwidth estimator.
 *   - NACK/PLI/FIR   -> the send stream named by the media SSRC.
 *
 * Owns an SSRC -> send-stream table; receive streams are resolved via the
 * transport so there is a single recv binding shared with the RTP path. This
 * is the reusable glue that used to live, hand-written, in the client peer
 * connection. Not thread safe: the owner drives add / remove / handle from the
 * worker loop (the transport on_rtcp callback runs there).
 */
#ifndef RTC_RTCP_ROUTER_H
#define RTC_RTCP_ROUTER_H

#include "rtc_common.h"
#include "rtc_rtp_stream.h"
#include "rtc_transport.h"
#include "rtc_u32_map.h"

typedef struct {
    rtc_u32_map_t send_streams;
    rtc_transport_t *transport;
    bool ready;
} rtc_rtcp_router_t;

/* Initialize the router. `transport` may be NULL (SR routing and the RR loss
 * feed are then skipped); otherwise it is used to resolve receive streams and
 * to report RR loss into the bandwidth estimator. */
rtc_err_t rtc_rtcp_router_init(rtc_rtcp_router_t *r, rtc_transport_t *transport);

/* Free the backing table. Safe on zero-init and idempotent. */
void rtc_rtcp_router_close(rtc_rtcp_router_t *r);

rtc_err_t rtc_rtcp_router_add_sender(rtc_rtcp_router_t *r, uint32_t ssrc,
                                     rtc_rtp_send_stream_t *stream);
void rtc_rtcp_router_remove_sender(rtc_rtcp_router_t *r, uint32_t ssrc);

/* Parse and route one compound RTCP packet (post SRTCP-unprotect). */
void rtc_rtcp_router_handle(rtc_rtcp_router_t *r, const uint8_t *buf, size_t len);

#endif /* RTC_RTCP_ROUTER_H */
