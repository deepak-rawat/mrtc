/*
 * Track / Transceiver API - WebRTC-style media model.
 *
 * Mirrors the RTCRtpSender, RTCRtpReceiver, and RTCRtpTransceiver interfaces.
 * A transceiver pairs a sender (outgoing) with a receiver (incoming) and
 * maps to a single m= line in SDP.
 */
#ifndef RTC_TRACK_H
#define RTC_TRACK_H

#include "rtc_common.h"

#define RTC_MAX_TRANSCEIVERS 8

/* Media kind */
typedef enum {
    RTC_KIND_AUDIO,
    RTC_KIND_VIDEO,
} rtc_kind_t;

/* Codec parameters (mirrors RTCRtpCodecParameters) */
typedef struct {
    uint8_t payload_type; /* Dynamic payload type, e.g. 111 */
    char mime_type[64];   /* e.g. "audio/opus", "video/H264" */
    uint32_t clock_rate;  /* e.g. 48000 */
    int channels;         /* Audio channels, e.g. 2. 0 for video. */
} rtc_codec_t;

/* Transceiver direction (mirrors RTCRtpTransceiverDirection) */
typedef enum {
    RTC_DIR_SENDRECV,
    RTC_DIR_SENDONLY,
    RTC_DIR_RECVONLY,
    RTC_DIR_INACTIVE,
} rtc_direction_t;

/* Callback for received frames (fires on transport thread) */
typedef void (*rtc_on_frame_fn)(const uint8_t *payload, size_t len, uint16_t seq,
                                uint32_t timestamp, uint32_t ssrc, bool marker, void *user);

/* Opaque handles; internal structs are defined in rtc_track.c. */
typedef struct rtc_rtp_sender rtc_rtp_sender_t;
typedef struct rtc_rtp_receiver rtc_rtp_receiver_t;
typedef struct rtc_rtp_transceiver rtc_rtp_transceiver_t;

/* Send a media frame. Builds RTP + SRTP internally (fires on main thread). */
int rtc_rtp_sender_send(rtc_rtp_sender_t *sender, const uint8_t *payload, size_t len,
                        uint32_t samples, bool marker);

/* Get the codec parameters for this sender */
const rtc_codec_t *rtc_rtp_sender_get_codec(const rtc_rtp_sender_t *sender);

/* Get the media kind */
rtc_kind_t rtc_rtp_sender_kind(const rtc_rtp_sender_t *sender);

/* Get the target bitrate in kbps from the rate controller (0 if no rate control). */
int rtc_rtp_sender_get_target_bitrate(const rtc_rtp_sender_t *sender);

/* Returns true and clears if a keyframe was requested by rate control. */
bool rtc_rtp_sender_should_keyframe(rtc_rtp_sender_t *sender);

/* One per simulcast layer in the spec; we model just the primary layer. */
typedef struct {
    bool active;              /* false suppresses transmission */
    uint32_t max_bitrate_bps; /* 0 = unbounded */
} rtc_rtp_send_encoding_t;

typedef struct {
    rtc_rtp_send_encoding_t encodings[1];
    int encoding_count;
} rtc_rtp_send_params_t;

/* Snapshot the sender's current send parameters. */
int rtc_rtp_sender_get_parameters(const rtc_rtp_sender_t *sender, rtc_rtp_send_params_t *params);

/* Apply send parameters. Affects future sends:
 *   active=false   -> sender_send returns RTC_OK without transmitting
 *   max_bitrate_bps -> clamps rtc_rtp_sender_get_target_bitrate
 * Returns RTC_OK on success. */
int rtc_rtp_sender_set_parameters(rtc_rtp_sender_t *sender, const rtc_rtp_send_params_t *params);

/* Sender feedback callbacks fire on the transport thread. */

/* NACK: reports lost sequence numbers that were retransmitted. */
typedef void (*rtc_on_nack_fn)(const uint16_t *lost_seqs, int count, void *user);

/* PLI: remote peer requested a keyframe. */
typedef void (*rtc_on_pli_fn)(void *user);

/* Register optional notification callbacks on a sender. */
void rtc_rtp_sender_on_nack(rtc_rtp_sender_t *sender, rtc_on_nack_fn fn, void *user);
void rtc_rtp_sender_on_pli(rtc_rtp_sender_t *sender, rtc_on_pli_fn fn, void *user);

/* Set callback for incoming decoded frames (fires on transport thread) */
void rtc_rtp_receiver_on_frame(rtc_rtp_receiver_t *receiver, rtc_on_frame_fn fn, void *user);

/* Get the media kind */
rtc_kind_t rtc_rtp_receiver_kind(const rtc_rtp_receiver_t *receiver);

/* Get the codec parameters */
const rtc_codec_t *rtc_rtp_receiver_get_codec(const rtc_rtp_receiver_t *receiver);

/* Get/set direction */
rtc_direction_t rtc_rtp_transceiver_direction(const rtc_rtp_transceiver_t *t);
void rtc_rtp_transceiver_set_direction(rtc_rtp_transceiver_t *t, rtc_direction_t dir);

/* Access sender/receiver */
rtc_rtp_sender_t *rtc_rtp_transceiver_sender(rtc_rtp_transceiver_t *t);
rtc_rtp_receiver_t *rtc_rtp_transceiver_receiver(rtc_rtp_transceiver_t *t);

/* Get the media identification tag (m= line index) */
const char *rtc_rtp_transceiver_mid(const rtc_rtp_transceiver_t *t);

#endif /* RTC_TRACK_H */
