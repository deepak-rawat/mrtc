/*
 * conference.h — High-level conferencing API.
 *
 * Manages multi-party WebRTC sessions: peer connections, media encode/decode,
 * signaling wiring, and data channels. Applications push raw frames and
 * receive decoded remote frames via callbacks.
 *
 * Usage:
 *   1. conference_create(config + callbacks)
 *   2. conference_add_video_source("camera", 640, 480, 30, 500)
 *   3. conference_add_audio_source("mic", 48000, 1, 32000)
 *   4. conference_join(conf, signaling_client)
 *   5. Loop: conference_push_video("camera", frame)
 *   6. conference_leave() + conference_destroy()
 *
 * Remote decoded frames arrive via on_remote_video / on_remote_audio callbacks.
 */
#ifndef CONFERENCE_H
#define CONFERENCE_H

#include <rtc/rtc_types.h>
#include <media/video_codec.h>
#include <media/audio_codec.h>
#include <signaling/signaling_client.h>
#include <stdbool.h>

/* ---- Callbacks (fire from internal threads — app must handle thread safety) ---- */
typedef struct {
    /* Remote media decoded and ready for display/playback */
    void (*on_remote_video)(const char *peer_id, const char *label, const video_frame_t *frame,
                            void *user);
    void (*on_remote_audio)(const char *peer_id, const char *label, const audio_frame_t *audio,
                            void *user);

    /* Peer lifecycle */
    void (*on_peer_joined)(const char *peer_id, void *user);
    void (*on_peer_left)(const char *peer_id, void *user);
    void (*on_connected)(const char *peer_id, void *user);

    /* Data channel messages */
    void (*on_message)(const char *peer_id, const uint8_t *data, size_t len, void *user);

    /* Status */
    void (*on_joined)(const char *my_id, void *user);
    void (*on_error)(const char *message, void *user);
} conference_callbacks_t;

/* ---- Configuration ---- */
typedef struct {
    const char *video_codec; /* "VP8" (default) */
    const char *audio_codec; /* "opus" (default) */
    int video_width;         /* 640 */
    int video_height;        /* 480 */
    int video_fps;           /* 30 */
    int video_bitrate_kbps;  /* 500 */
    int audio_sample_rate;   /* 48000 */
    int audio_channels;      /* 1 */
    int audio_bitrate_bps;   /* 32000 */
    const char *stun_server; /* "stun.l.google.com" or NULL */

    conference_callbacks_t callbacks;
    void *user_data;
} conference_config_t;

typedef struct conference conference_t;

/* ---- Lifecycle ---- */
conference_t *conference_create(const conference_config_t *cfg);

/*
 * Get signaling config wired to this conference.
 * Call this BEFORE signaling_create() + signaling_connect().
 * The signaling client's callbacks will route to conference internals.
 */
void conference_get_signaling_callbacks(conference_t *c, signaling_config_t *out_cfg,
                                        const char *server_url, const char *meeting);

/* Join: store signaling client reference (must already be connected) */
int conference_join(conference_t *c, signaling_client_t *signaling);
void conference_leave(conference_t *c);
void conference_destroy(conference_t *c);

/* ---- Media sources (local capture → encode → send to all peers) ---- */

/*
 * Add a video source (e.g. "camera", "screen").
 * Each source gets its own encoder. Multiple video sources can be active.
 * Pass 0 for width/height/fps/bitrate to use config defaults.
 */
int conference_add_video_source(conference_t *c, const char *label, int width, int height, int fps,
                                int bitrate_kbps);

/* Add an audio source (e.g. "mic"). */
int conference_add_audio_source(conference_t *c, const char *label, int sample_rate, int channels,
                                int bitrate_bps);

/* Push a frame from a specific source */
int conference_push_video(conference_t *c, const char *label, const video_frame_t *frame);
int conference_push_audio(conference_t *c, const char *label, const audio_frame_t *audio);

/* Mute/unmute a source (stops encoding+sending, keeps the stream) */
void conference_mute_source(conference_t *c, const char *label, bool mute);

/* Remove a source entirely (e.g., stop screen sharing) */
void conference_remove_source(conference_t *c, const char *label);

/* ---- Data channel messaging ---- */
int conference_send_message(conference_t *c, const char *peer_id, const uint8_t *data, size_t len);
int conference_broadcast_message(conference_t *c, const uint8_t *data, size_t len);

/* ---- Status ---- */
int conference_get_peer_count(conference_t *c);

#endif /* CONFERENCE_H */
