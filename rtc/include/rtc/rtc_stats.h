/*
 * Stats API - mirrors WebRTC RTCStatsReport (W3C webrtc-stats).
 *
 * `rtc_peer_connection_get_stats` produces a snapshot of the per-transceiver
 * counters maintained by the RTCP stats path. The values are monotonic
 * counters; the report is a best-effort, lock-free read across threads
 * (RTP send updates on the main thread, RTP recv updates on the transport
 * thread). Individual fields may be slightly stale but the read is safe.
 */
#ifndef RTC_STATS_H
#define RTC_STATS_H

#include "rtc_common.h"
#include "rtc_track.h"

/* Per-transceiver stats (one entry per active m-line). */
typedef struct {
    char mid[32];           /* transceiver mid, "" if unset */
    rtc_kind_t kind;        /* RTC_KIND_AUDIO or RTC_KIND_VIDEO */
    rtc_direction_t dir;    /* current negotiated direction */

    /* Outbound (RTCOutboundRtpStreamStats subset) */
    uint32_t out_ssrc;          /* 0 if sender inactive */
    uint32_t out_packets_sent;
    uint32_t out_bytes_sent;    /* RTP payload octets (RFC 3550 sender octet count) */

    /* Inbound (RTCInboundRtpStreamStats subset) */
    uint32_t in_ssrc;              /* 0 if receiver hasn't seen any packet */
    uint32_t in_packets_received;
    uint32_t in_packets_lost;
    uint32_t in_jitter_q16;        /* Q16.16 fixed-point (RFC 3550 interarrival jitter) */
} rtc_transceiver_stats_t;

typedef struct {
    int transceiver_count;
    rtc_transceiver_stats_t transceivers[RTC_MAX_TRANSCEIVERS];
} rtc_stats_report_t;

/* Forward decl from rtc_peer.h */
typedef struct rtc_peer_connection rtc_peer_connection_t;

/* Fill `report` with a snapshot of the peer connection's current stats.
 * Returns RTC_OK on success. The report is overwritten on each call. */
int rtc_peer_connection_get_stats(const rtc_peer_connection_t *pc, rtc_stats_report_t *report);

#endif /* RTC_STATS_H */
