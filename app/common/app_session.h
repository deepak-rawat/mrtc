#ifndef APP_SESSION_H
#define APP_SESSION_H

#include <conference/conference.h>

#define APP_SESSION_MAX_SOURCES    32
#define APP_SESSION_SERVER_SIZE    512
#define APP_SESSION_DEFAULT_SERVER "ws://127.0.0.1:8080"
#define APP_SESSION_DEFAULT_STUN   "stun:stun.l.google.com:19302"
#define APP_SESSION_VIDEO_WIDTH    320
#define APP_SESSION_VIDEO_HEIGHT   240
#define APP_SESSION_VIDEO_FPS      15

typedef enum { APP_MEDIA_NONE, APP_MEDIA_SYNTHETIC, APP_MEDIA_EXTERNAL } app_media_mode_t;

typedef struct {
    const char *server;
    const char *room;
    const char *name;
    const char *stun;
    app_media_mode_t video;
    app_media_mode_t audio;
    bool data_only;
} app_session_options_t;

typedef struct {
    conference_callbacks_t callbacks;
    void *user_data;
    void (*on_local_video)(const video_frame_t *frame, void *user);
    void (*on_local_audio)(const audio_frame_t *frame, void *user);
} app_session_config_t;

typedef struct {
    char id[SIG_MAX_PEER_ID];
    char name[SIG_MAX_DISPLAY_NAME];
    bool connected;
    uint64_t video_frames;
    uint64_t audio_frames;
    uint64_t audio_samples;
} app_session_peer_t;

typedef struct {
    char peer_id[SIG_MAX_PEER_ID];
    char label[64];
    uint64_t video_frames;
    uint64_t audio_frames;
    uint64_t audio_samples;
} app_session_source_t;

typedef struct {
    uint64_t generation;
    uint32_t epoch;
    bool active;
    bool joined;
    bool video_muted;
    bool audio_muted;
    char local_id[SIG_MAX_PEER_ID];
    char name[SIG_MAX_DISPLAY_NAME];
    char topology[8];
    char status[32];
    char error[256];
    size_t peer_count;
    app_session_peer_t peers[SIG_MAX_PEERS];
    size_t source_count;
    app_session_source_t sources[APP_SESSION_MAX_SOURCES];
} app_session_snapshot_t;

typedef struct app_session app_session_t;

/* Lifecycle, tick, send and mute belong to the owning application thread.
 * Snapshot reads are thread-safe. Forwarded callbacks run on RTC threads,
 * after snapshot updates; do not stop/destroy the session from a callback. */
app_session_t *app_session_create(const app_session_config_t *config);
int app_session_validate_options(const app_session_options_t *options);
int app_session_start(app_session_t *session, const app_session_options_t *options);
void app_session_tick(app_session_t *session, uint64_t now_ms);
/* Synthetic join-screen preview without starting RTC or signaling. */
void app_session_preview_tick(app_session_t *session, uint64_t now_ms);
void app_session_stop(app_session_t *session);
void app_session_destroy(app_session_t *session);
int app_session_send_message(app_session_t *session, const char *peer_id, const uint8_t *data,
                             size_t length);
int app_session_mute(app_session_t *session, const char *source, bool muted);
void app_session_get_snapshot(app_session_t *session, app_session_snapshot_t *snapshot);
/* Borrowed handle for external media producers, valid until stop. */
conference_t *app_session_conference(app_session_t *session);

#endif
