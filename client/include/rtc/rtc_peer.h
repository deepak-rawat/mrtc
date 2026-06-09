/*
 * Peer Connection - WebRTC-style API (mirrors RTCPeerConnection).
 *
 * Opaque type, heap-allocated. Owns a transport thread that drives
 * ICE, DTLS, and SRTP. All protocol state is touched only on the
 * transport thread — no mutexes on protocol state.
 *
 * Threading model:
 *   Main thread:   create, add_track, create_offer/answer,
 *                  set_local/remote_desc, sender_send, close, destroy
 *   Transport thread: ICE checks, DTLS handshake, SRTP, recv callbacks
 *
 * Connection is implicit: after both local and remote descriptions are
 * set, ICE + DTLS + SRTP start automatically on the transport thread.
 */
#ifndef RTC_PEER_H
#define RTC_PEER_H

#include "rtc_common.h"
#include "rtc_track.h"
#include "rtc_data_channel.h"

/* ---- Opaque peer connection ---- */
typedef struct rtc_peer_connection rtc_peer_connection_t;

/* ---- RTCSessionDescription ---- */
typedef struct {
    rtc_sdp_type_t type; /* offer / answer / pranswer */
    char sdp[SDP_MAX_SIZE];
    size_t sdp_len;
} rtc_desc_t;

/* ---- RTCIceCandidate (for signaling exchange) ---- */
typedef struct {
    char candidate[256]; /* a=candidate:... line */
    char mid[32];        /* sdpMid */
    int mid_index;       /* sdpMLineIndex */
} rtc_ice_candidate_desc_t;

/* ---- RTCIceServer ---- */
typedef struct {
    const char *urls[4];
    int url_count;
    const char *username;   /* TURN only (unused) */
    const char *credential; /* TURN only (unused) */
} rtc_ice_server_t;

/* ---- RTCConfiguration ---- */
typedef struct {
    rtc_ice_server_t ice_servers[4];
    int ice_server_count;
} rtc_config_t;

/* ---- State enums (mirror WebRTC spec names) ---- */

typedef enum {
    RTC_SIGNALING_STABLE,
    RTC_SIGNALING_HAVE_LOCAL_OFFER,
    RTC_SIGNALING_HAVE_REMOTE_OFFER,
    RTC_SIGNALING_HAVE_LOCAL_PRANSWER,
    RTC_SIGNALING_HAVE_REMOTE_PRANSWER,
    RTC_SIGNALING_CLOSED,
} rtc_signaling_state_t;

typedef enum {
    RTC_ICE_GATHERING_NEW,
    RTC_ICE_GATHERING_GATHERING,
    RTC_ICE_GATHERING_COMPLETE,
} rtc_ice_gathering_state_t;

typedef enum {
    RTC_ICE_CONNECTION_NEW,
    RTC_ICE_CONNECTION_CHECKING,
    RTC_ICE_CONNECTION_CONNECTED,
    RTC_ICE_CONNECTION_COMPLETED,
    RTC_ICE_CONNECTION_DISCONNECTED,
    RTC_ICE_CONNECTION_FAILED,
    RTC_ICE_CONNECTION_CLOSED,
} rtc_ice_connection_state_t;

typedef enum {
    RTC_CONNECTION_NEW,
    RTC_CONNECTION_CONNECTING,
    RTC_CONNECTION_CONNECTED,
    RTC_CONNECTION_DISCONNECTED,
    RTC_CONNECTION_FAILED,
    RTC_CONNECTION_CLOSED,
} rtc_connection_state_t;

/* ---- Callback typedefs (all fire on the transport thread) ---- */

typedef void (*rtc_on_signaling_state_fn)(rtc_signaling_state_t state, void *user);
typedef void (*rtc_on_ice_gathering_state_fn)(rtc_ice_gathering_state_t state, void *user);
typedef void (*rtc_on_ice_connection_state_fn)(rtc_ice_connection_state_t state, void *user);
typedef void (*rtc_on_connection_state_fn)(rtc_connection_state_t state, void *user);
typedef void (*rtc_on_ice_candidate_fn)(const rtc_ice_candidate_desc_t *cand, void *user);
typedef void (*rtc_on_track_fn)(rtc_rtp_receiver_t *receiver, void *user);
typedef void (*rtc_on_data_channel_fn)(rtc_data_channel_t *channel, void *user);

/* ---- Lifecycle ---- */

/* Create a new peer connection (heap-allocated, opaque). Returns NULL on error. */
rtc_peer_connection_t *rtc_peer_connection_create(const rtc_config_t *config);

/* Tear down the connection (stops ICE, DTLS, transport thread). */
void rtc_peer_connection_close(rtc_peer_connection_t *pc);

/* Free memory. Must be called after close(). */
void rtc_peer_connection_destroy(rtc_peer_connection_t *pc);

/* ---- Track management ---- */

/* Add a track (creates a transceiver with sendrecv direction).
 * Must be called before create_offer/create_answer.
 * Returns the sender, or NULL on error. */
rtc_rtp_sender_t *rtc_peer_connection_add_track(rtc_peer_connection_t *pc, rtc_kind_t kind,
                                                const rtc_codec_t *codec);

/* RTCRtpTransceiverInit (spec subset). Direction defaults to sendrecv. */
typedef struct {
    rtc_direction_t direction;
} rtc_rtp_transceiver_init_t;

/* Add a transceiver with explicit direction (mirrors addTransceiver()).
 * Must be called before create_offer/create_answer.
 * Returns the transceiver, or NULL on error. */
rtc_rtp_transceiver_t *rtc_peer_connection_add_transceiver(rtc_peer_connection_t *pc,
                                                           rtc_kind_t kind,
                                                           const rtc_codec_t *codec,
                                                           const rtc_rtp_transceiver_init_t *init);

/* Remove a sender's track (mirrors removeTrack()).
 * Marks the sender inactive and transitions the transceiver direction:
 *   sendrecv -> recvonly, sendonly -> inactive (others unchanged).
 * Returns RTC_OK on success, RTC_ERR_INVALID if sender is not part of pc. */
int rtc_peer_connection_remove_track(rtc_peer_connection_t *pc, rtc_rtp_sender_t *sender);

/* Get transceivers. Writes up to *count pointers; sets *count to actual count. */
int rtc_peer_connection_get_transceivers(rtc_peer_connection_t *pc, rtc_rtp_transceiver_t **out,
                                         int *count);

/* ---- SDP offer/answer ---- */

/* Generate a local offer SDP from transceivers + ICE + DTLS. */
int rtc_peer_connection_create_offer(rtc_peer_connection_t *pc, rtc_desc_t *desc);

/* Generate a local answer SDP (after setting remote offer). */
int rtc_peer_connection_create_answer(rtc_peer_connection_t *pc, rtc_desc_t *desc);

/* Apply local description. Starts ICE gathering. */
int rtc_peer_connection_set_local_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc);

/* Apply remote description. If both descriptions are now set,
 * connection (ICE + DTLS + SRTP) starts automatically. */
int rtc_peer_connection_set_remote_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc);

/* Access current descriptions (NULL if not set). */
const rtc_desc_t *rtc_peer_connection_local_desc(const rtc_peer_connection_t *pc);
const rtc_desc_t *rtc_peer_connection_remote_desc(const rtc_peer_connection_t *pc);

/* ---- Trickle ICE ---- */

/* Add a remote ICE candidate received via signaling. */
int rtc_peer_connection_add_ice_candidate(rtc_peer_connection_t *pc,
                                          const rtc_ice_candidate_desc_t *cand);

/* Trigger an ICE restart: regenerates local ufrag/pwd so the next
 * create_offer/create_answer carries fresh credentials. Must be called
 * before the connection has started (this build does not support
 * mid-session renegotiation). Returns RTC_ERR_INVALID otherwise. */
int rtc_peer_connection_restart_ice(rtc_peer_connection_t *pc);

/* ---- Data channels ---- */

/* Create a data channel with the given label and options.
 * Returns the channel, or NULL on error.
 * The channel fires on_open once the DTLS connection is established. */
rtc_data_channel_t *rtc_peer_connection_create_data_channel(rtc_peer_connection_t *pc,
                                                            const char *label,
                                                            const rtc_data_channel_init_t *opts);

/* ---- Event callbacks (set before connection starts) ---- */

void rtc_peer_connection_on_signaling_state(rtc_peer_connection_t *pc, rtc_on_signaling_state_fn fn,
                                            void *user);
void rtc_peer_connection_on_ice_gathering_state(rtc_peer_connection_t *pc,
                                                rtc_on_ice_gathering_state_fn fn, void *user);
void rtc_peer_connection_on_ice_connection_state(rtc_peer_connection_t *pc,
                                                 rtc_on_ice_connection_state_fn fn, void *user);
void rtc_peer_connection_on_connection_state(rtc_peer_connection_t *pc,
                                             rtc_on_connection_state_fn fn, void *user);
void rtc_peer_connection_on_ice_candidate(rtc_peer_connection_t *pc, rtc_on_ice_candidate_fn fn,
                                          void *user);
void rtc_peer_connection_on_track(rtc_peer_connection_t *pc, rtc_on_track_fn fn, void *user);
void rtc_peer_connection_on_data_channel(rtc_peer_connection_t *pc, rtc_on_data_channel_fn fn,
                                         void *user);

/* Bandwidth-estimate callback: fired on the transport thread when the
 * per-peer GCC estimator updates its target sending bitrate (in bits/sec).
 * Applications typically forward this to their encoder bitrate setter. */
typedef void (*rtc_on_bitrate_estimate_fn)(uint32_t bitrate_bps, void *user);
void rtc_peer_connection_on_bitrate_estimate(rtc_peer_connection_t *pc,
                                             rtc_on_bitrate_estimate_fn fn, void *user);

/* ---- State getters (safe to call from any thread) ---- */

rtc_signaling_state_t rtc_peer_connection_signaling_state(const rtc_peer_connection_t *pc);
rtc_ice_gathering_state_t rtc_peer_connection_ice_gathering_state(const rtc_peer_connection_t *pc);
rtc_ice_connection_state_t rtc_peer_connection_ice_connection_state(const rtc_peer_connection_t *pc);
rtc_connection_state_t rtc_peer_connection_connection_state(const rtc_peer_connection_t *pc);

/* ---- Identity / capability getters ---- */

/* Local DTLS certificate fingerprint (SHA-256, hex with colons).
 * Always returns a NUL-terminated string; never NULL after create. */
const char *rtc_peer_connection_local_fingerprint(const rtc_peer_connection_t *pc);

/* Remote DTLS certificate fingerprint as parsed from the remote SDP.
 * Returns "" until set_remote_desc completes. */
const char *rtc_peer_connection_remote_fingerprint(const rtc_peer_connection_t *pc);

/* True if the remote peer advertised trickle ICE support
 * (a=ice-options:trickle, RFC 8839 §5.4). False before set_remote_desc. */
bool rtc_peer_connection_can_trickle_ice_candidates(const rtc_peer_connection_t *pc);

#endif /* RTC_PEER_H */
