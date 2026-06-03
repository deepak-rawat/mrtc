/*
 * rtc_peer_internal.h - Shared internal definitions for rtc_peer.c and
 * rtc_peer_packets.c.  NOT part of the public API.
 *
 * Contains the concrete struct layouts for peer_connection, sender,
 * receiver, and transceiver plus helpers used by both translation units.
 */
#ifndef RTC_PEER_INTERNAL_H
#define RTC_PEER_INTERNAL_H

#include "rtc/rtc_peer.h"
#include "rtc/rtc_stats.h"
#include "rtc/rtc_u32_map.h"
#include "rtc_data_channel_internal.h"
#include "rtc_rtp.h"
#include "rtc_rtp_ext.h"
#include "rtc_rtcp.h"
#include "rtc_transport.h"
#include "rtc_ice.h"
#include "rtc_dtls.h"
#include "rtc_srtp.h"
#include "rtc_sdp.h"
#include "rtc_nack_buf.h"

#ifdef MRTC_ENABLE_RATE_CONTROL
#  include "rtc_rate_control.h"
#endif

#ifdef MRTC_ENABLE_TWCC
#  include "rtc_twcc_sender.h"
#  include "rtc_twcc_receiver.h"
#  include "rtc_bwe.h"
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Internal transceiver structs (layout shared with rtc_track.c) ---- */

struct rtc_rtp_sender {
    rtc_codec_t codec;
    rtc_kind_t kind;
    rtc_rtp_session_t rtp_session;
    rtc_srtp_ctx_t *srtp;
    void *transport;
    rtc_rtcp_stats_t rtcp_stats;
#ifdef MRTC_ENABLE_RATE_CONTROL
    rtc_rate_controller_t *rate_ctrl;
#endif
    rtc_nack_buf_t *nack_buf;
    bool active;

    /* Feedback callbacks */
    rtc_on_nack_fn on_nack;
    void *on_nack_user;
    rtc_on_pli_fn on_pli;
    void *on_pli_user;

    /* Rate limiters for inbound feedback (transport thread only). */
    uint64_t last_pli_handled_ms;

#ifdef MRTC_ENABLE_TWCC
    /* Transport-CC tagging (set after SDP negotiation) */
    void *twcc;          /* borrowed rtc_twcc_sender_t* */
    uint8_t twcc_ext_id; /* 0 = not negotiated */
#endif
};

struct rtc_rtp_receiver {
    rtc_codec_t codec;
    rtc_kind_t kind;
    rtc_on_frame_fn on_frame;
    void *on_frame_user;
    uint32_t ssrc;
    rtc_rtcp_stats_t rtcp_stats;
    bool active;
};

struct rtc_rtp_transceiver {
    struct rtc_rtp_sender sender;
    struct rtc_rtp_receiver receiver;
    rtc_direction_t direction;
    char mid[32];
    int mid_index;
    bool used;
};

/* ---- Peer connection internal struct ---- */

struct rtc_peer_connection {
    /* Transport (owns socket and I/O thread) */
    rtc_transport_t transport;

    /* Protocol components (touched only on transport thread after connect) */
    rtc_ice_agent_t ice;
    rtc_dtls_transport_t dtls;
    rtc_srtp_ctx_t srtp_send; /* after init: main thread only */
    rtc_srtp_ctx_t srtp_recv; /* transport thread only */

    /* Data channel manager */
    rtc_dc_manager_t dc_manager;

    /* Transceivers (main thread before connect, transport thread after) */
    struct rtc_rtp_transceiver transceivers[RTC_MAX_TRANSCEIVERS];
    int transceiver_count;

    /* Descriptions */
    rtc_desc_t local_desc;
    rtc_desc_t remote_desc;
    bool has_local_desc;
    bool has_remote_desc;
    rtc_sdp_t local_sdp;
    rtc_sdp_t remote_sdp;

    /* States */
    _Atomic rtc_signaling_state_t signaling_state;
    _Atomic rtc_ice_gathering_state_t ice_gathering_state;
    _Atomic rtc_ice_connection_state_t ice_connection_state;
    _Atomic rtc_connection_state_t connection_state;

    /* Callbacks */
    rtc_on_signaling_state_fn on_signaling_state;
    void *on_signaling_state_user;
    rtc_on_ice_gathering_state_fn on_ice_gathering_state;
    void *on_ice_gathering_state_user;
    rtc_on_ice_connection_state_fn on_ice_connection_state;
    void *on_ice_connection_state_user;
    rtc_on_connection_state_fn on_connection_state;
    void *on_connection_state_user;
    rtc_on_ice_candidate_fn on_ice_candidate;
    void *on_ice_candidate_user;
    rtc_on_track_fn on_track;
    void *on_track_user;
    rtc_on_data_channel_fn on_data_channel;
    void *on_data_channel_user;

    /* Config */
    char stun_server[64];
    uint16_t stun_port;

    /* Connection started flag */
    bool connect_started;

    /* RTCP send timer */
    rtc_timer_id_t rtcp_timer_id;

#ifdef MRTC_ENABLE_RATE_CONTROL
    /* Shared rate controller fallback (created on connection, fed by RTCP RR) */
    rtc_rate_controller_t *rate_ctrl;
#endif

    /* Fast SSRC → receiver lookup */
    rtc_u32_map_t recv_map;

    /* Fast SSRC → sender lookup */
    rtc_u32_map_t send_map;

    /* DTLS application-data receive buffer */
    uint8_t *app_buf;
    size_t app_buf_cap;

#ifdef MRTC_ENABLE_TWCC
    /* Transport-Wide Congestion Control */
    rtc_twcc_sender_t twcc_sender;
    rtc_twcc_receiver_t twcc_receiver;
    uint8_t twcc_ext_id_send;
    uint8_t twcc_ext_id_recv;
    uint32_t twcc_local_ssrc;
    uint32_t twcc_remote_ssrc;
    rtc_timer_id_t twcc_fb_timer_id;
    bool twcc_have_packets;

    /* Bandwidth estimator (consumes TWCC feedback + RR loss) */
    rtc_bwe_t *bwe;
    rtc_on_bitrate_estimate_fn on_bitrate_estimate;
    void *on_bitrate_estimate_user;
#endif
};

/* ---- State helpers (used by both peer and packets TUs) ---- */

static inline void peer_set_signaling(rtc_peer_connection_t *pc, rtc_signaling_state_t s) {
    pc->signaling_state = s;
    if (pc->on_signaling_state)
        pc->on_signaling_state(s, pc->on_signaling_state_user);
}

static inline void peer_set_ice_gathering(rtc_peer_connection_t *pc, rtc_ice_gathering_state_t s) {
    pc->ice_gathering_state = s;
    if (pc->on_ice_gathering_state)
        pc->on_ice_gathering_state(s, pc->on_ice_gathering_state_user);
}

static inline void peer_set_ice_connection(rtc_peer_connection_t *pc,
                                           rtc_ice_connection_state_t s) {
    pc->ice_connection_state = s;
    if (pc->on_ice_connection_state)
        pc->on_ice_connection_state(s, pc->on_ice_connection_state_user);
}

static inline void peer_set_connection(rtc_peer_connection_t *pc, rtc_connection_state_t s) {
    pc->connection_state = s;
    if (pc->on_connection_state)
        pc->on_connection_state(s, pc->on_connection_state_user);
}

/* ---- Internal handlers from rtc_track.c ---- */

void rtc_rtp_sender_handle_nack(rtc_rtp_sender_t *sender, const uint16_t *lost_seqs, int count);
void rtc_rtp_sender_handle_pli(rtc_rtp_sender_t *sender);

/* ---- Internal transceiver helpers from rtc_track.c ----
 * These operate on a single transceiver/sender/receiver. The peer connection
 * owns the array of transceivers and the SSRC maps; it loops over slots and
 * calls these helpers for the per-slot work.
 */

/* Initialize a freshly-allocated transceiver slot: codec, kind, RTP session,
 * RTCP stats, mid string. Sender starts active; receiver starts inactive
 * (activated by rtc_rtp_receiver_activate when the remote description arrives). */
void rtc_rtp_transceiver_init_slot(struct rtc_rtp_transceiver *t, int mid_index, rtc_kind_t kind,
                                   const rtc_codec_t *codec);

/* Free per-sender resources allocated by rtc_rtp_sender_arm_video. Idempotent. */
void rtc_rtp_transceiver_close_resources(struct rtc_rtp_transceiver *t);

/* Wire a sender to its SRTP send context + transport so it can send. */
void rtc_rtp_sender_attach(struct rtc_rtp_sender *s, rtc_srtp_ctx_t *srtp_send,
                           rtc_transport_t *transport);

/* Bind transport-cc tagging on a sender (no-op when MRTC_ENABLE_TWCC is off). */
void rtc_rtp_sender_attach_twcc(struct rtc_rtp_sender *s, void *twcc_sender, uint8_t ext_id);

/* Allocate NACK ring + rate controller for a video sender. No-op for audio. */
void rtc_rtp_sender_arm_video(struct rtc_rtp_sender *s);

/* Flip a receiver to active (called after remote description is set). */
void rtc_rtp_receiver_activate(struct rtc_rtp_receiver *r);

/* Build SR / RR for a single sender or receiver, SRTCP-protect, send. */
void rtc_rtp_sender_emit_sr(struct rtc_rtp_sender *s, rtc_srtp_ctx_t *srtp_send,
                            rtc_transport_t *transport);
void rtc_rtp_receiver_emit_rr(struct rtc_rtp_receiver *r, rtc_srtp_ctx_t *srtp_send,
                              rtc_transport_t *transport);

/* Serialize a single transceiver into an SDP m= section. */
void rtc_rtp_transceiver_fill_sdp_media(const struct rtc_rtp_transceiver *t, rtc_sdp_media_t *m);

/* ---- Cross-TU function used by rtc_peer_packets.c, defined in rtc_peer.c ---- */

void peer_complete_connection(rtc_peer_connection_t *pc);

/* ---- Entry points defined in rtc_peer_packets.c ---- */

/* Transport recv callback (registered with rtc_transport_init). */
void peer_transport_recv(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                         const rtc_addr_t *from, void *user);

/* RTCP send timer + interval (scheduled from peer_complete_connection). */
#define RTCP_INTERVAL_MS 5000
void peer_rtcp_timer(void *user);

#ifdef MRTC_ENABLE_TWCC
/* TWCC feedback send timer + interval. */
#  define TWCC_FB_INTERVAL_MS 100
void peer_twcc_fb_timer(void *user);

/* BWE bitrate-change trampoline (registered with rtc_bwe_on_bitrate_change). */
void peer_on_bwe_bitrate(uint32_t bitrate_bps, void *user);
#endif

#endif /* RTC_PEER_INTERNAL_H */
