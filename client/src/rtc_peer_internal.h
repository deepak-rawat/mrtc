/*
 * Shared internal definitions for rtc_peer.c. NOT part of the public API.
 *
 * Contains the concrete struct layouts for peer_connection, sender,
 * receiver, and transceiver plus helpers used across the client TUs.
 */
#ifndef RTC_PEER_INTERNAL_H
#define RTC_PEER_INTERNAL_H

#include "rtc/rtc_peer.h"
#include "rtc/rtc_stats.h"
#include "rtc/rtc_u32_map.h"
#include "rtc_data_channel_internal.h"
#include "rtc/rtc_rtp.h"
#include "rtc/rtc_rtp_ext.h"
#include "rtc/rtc_rtcp.h"
#include "rtc/rtc_sdp.h"
#include "rtc_client_runtime.h"
#include "rtc/rtc_listener.h"
#include "rtc/rtc_media_session.h"
#include "rtc/rtc_rtp_stream.h"
#include "rtc/rtc_transport.h"
#include "rtc/rtc_worker.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Internal transceiver structs; layout shared with rtc_track.c. The core
 * streams are codec-agnostic, so codec + kind live here on the facade. */

struct rtc_rtp_sender {
    rtc_rtp_send_stream_t *stream;
    rtc_codec_t codec;
    rtc_kind_t kind;
};

struct rtc_rtp_receiver {
    rtc_rtp_recv_stream_t *stream;
    rtc_codec_t codec;
    rtc_kind_t kind;
};

struct rtc_rtp_transceiver {
    struct rtc_rtp_sender sender;
    struct rtc_rtp_receiver receiver;
    rtc_direction_t direction;
    char mid[32];
    int mid_index;
    bool used;
};

struct rtc_peer_connection {
    /* Private logical runtime used by the peer facade. */
    rtc_client_runtime_t *runtime;
    rtc_transport_t *runtime_transport;
    bool runtime_registered;
    rtc_worker_timer_t runtime_connect_timer;
    char runtime_fingerprint[RTC_DTLS_FINGERPRINT_MAX];
    bool runtime_connected;

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

    /* Connection started flag */
    bool connect_started;

    /* Per-peer RTP/RTCP media session: owns the send/receive streams over the
     * runtime transport and drives RTP routing, RTCP feedback, and SR/RR. */
    rtc_media_session_t media_session;

#ifdef MRTC_ENABLE_TWCC
    /* Negotiated transport-cc header extension id (0 = not negotiated). The
     * TWCC sender/receiver, BWE, and 100 ms feedback timer all live in the
     * runtime transport; the peer only carries the id needed to tag senders. */
    uint8_t twcc_ext_id;
#endif
};

/* State helpers used by both the peer and packets TUs. */

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

/* Internal transceiver helpers from rtc_track.c.
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

/* Wire a sender to a logical transport. The logical transport owns SRTP. */
void rtc_rtp_sender_attach_logical(struct rtc_rtp_sender *s, rtc_transport_t *transport);

/* Bind transport-cc tagging on a sender (no-op when MRTC_ENABLE_TWCC is off). */
void rtc_rtp_sender_attach_twcc(struct rtc_rtp_sender *s, uint8_t ext_id);

/* Allocate NACK ring + rate controller for a video sender. No-op for audio. */
void rtc_rtp_sender_arm_video(struct rtc_rtp_sender *s);

/* Flip a receiver to active (called after remote description is set). */
void rtc_rtp_receiver_activate(struct rtc_rtp_receiver *r);

/* Serialize a single transceiver into an SDP m= section. */
void rtc_rtp_transceiver_fill_sdp_media(const struct rtc_rtp_transceiver *t, rtc_sdp_media_t *m);

#endif /* RTC_PEER_INTERNAL_H */
