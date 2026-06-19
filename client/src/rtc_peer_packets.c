/*
 * Peer Connection: transport-thread packet I/O.
 *
 * peer_rtp_resolve / peer_rtp_sink are installed as the transport's RTP
 * router (no peer-side SSRC map or trampoline). peer_rtcp_timer emits periodic
 * SR / RR. Inbound RTCP routing lives in the core rtc_rtcp_router; transport-
 * wide CC lives in the transport.
 */
#include "rtc_peer_internal.h"

#include <string.h>

/* Fallback for peers that omit a=ssrc: match an unbound SSRC to a receive
 * stream by payload type; the transport binds the returned stream. */
void *peer_rtp_resolve(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (!pc)
        return NULL;
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_receiver *rx = &pc->transceivers[i].receiver;
        if (rtc_rtp_recv_stream_can_receive(rx->stream, pkt->header.payload_type)) {
            rtc_rtp_recv_stream_set_ssrc(rx->stream, pkt->header.ssrc);
            return rx->stream;
        }
    }
    return NULL;
}

void peer_rtp_sink(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_rtp_recv_stream_on_packet((rtc_rtp_recv_stream_t *)user, pkt);
}

/* RTCP periodic send timer: fires on the transport thread every 5 seconds. */
void peer_rtcp_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    pc->runtime_rtcp_timer = RTC_WORKER_TIMER_INVALID;
    if (pc->connection_state != RTC_CONNECTION_CONNECTED)
        return;

    /* Build and send RTCP for each active transceiver */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[i];
        rtc_rtp_send_stream_emit_sr(t->sender.stream, pc->runtime_transport);
        rtc_rtp_recv_stream_emit_rr(t->receiver.stream, pc->runtime_transport);
    }

    /* Re-arm timer */
    if (pc->runtime_connected) {
        pc->runtime_rtcp_timer =
            rtc_worker_add_timer(rtc_client_runtime_worker(pc->runtime),
                                 rtc_time_ms() + RTCP_INTERVAL_MS, peer_rtcp_timer, pc);
    }
}
