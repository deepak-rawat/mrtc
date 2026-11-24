/*
 * SDL3 native UI: camera capture, audio I/O, video tile grid.
 */
#ifndef UI_SDL3_H
#define UI_SDL3_H

#include <SDL3/SDL.h>
#include <media/video_codec.h>
#include <media/audio_codec.h>
#include <rtc/rtc_str_map.h>
#include <stdbool.h>

#define UI_MAX_TILES  16
#define UI_CHAT_LINES 32

typedef enum {
    UI_ACTION_NONE,
    UI_ACTION_JOIN,
    UI_ACTION_MUTE,
    UI_ACTION_CAMERA,
    UI_ACTION_CHAT,
    UI_ACTION_SEND,
    UI_ACTION_LEAVE
} ui_action_t;

typedef struct {
    SDL_FRect gallery, chat, controls[5], fields[3], sources[2], join;
} ui_layout_t;

typedef struct {
    float level;
    uint64_t updated_at, active_until;
} ui_audio_activity_t;

typedef struct {
    char peer_id[64];
    char label[32];
    char name[128];
    SDL_Texture *texture;
    int width, height;
    ui_audio_activity_t audio_activity;
    bool active;
    bool remove_pending;

    /* Protected by tiles_mutex, including upload on the main thread. */
    uint8_t *pending_yuv;
    int pending_w, pending_h;
    bool pending_ready;
} ui_tile_t;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;

    /* Camera */
    SDL_Camera *camera;
    int cam_width, cam_height;

    /* Audio */
    SDL_AudioStream *mic_stream;
    SDL_AudioStream *speaker_stream;

    /* Tiles */
    ui_tile_t tiles[UI_MAX_TILES];
    int tile_count;
    rtc_str_map_t tile_index; /* "peer_id:label" → ui_tile_t * (owned keys) */
    rtc_mutex_t tiles_mutex;
    char pending_status[256];
    bool status_dirty;
    int16_t pending_audio[48000];
    size_t pending_audio_count;

    /* Local preview */
    SDL_Texture *local_preview;
    int preview_w, preview_h;
    ui_audio_activity_t local_audio_activity;
    uint64_t remote_video_uploads;
    bool in_call, mic_muted, cam_muted, chat_open, use_camera, use_mic;
    int focused_field;
    char server[256], room[128], name[128], chat_input[256];
    char chat_lines[UI_CHAT_LINES][512];
    int chat_count;
    char mode[16], status[256];
} ui_sdl3_t;

void ui_sdl3_layout(float width, float height, bool chat_open, ui_layout_t *out);
SDL_FRect ui_sdl3_fit(SDL_FRect bounds, int width, int height);
SDL_FRect ui_sdl3_tile_rect(SDL_FRect bounds, int count, int index);
size_t ui_sdl3_wrap_line(const char *text, size_t columns);
void ui_sdl3_update_audio_activity(ui_audio_activity_t *activity, float level, uint64_t now);
bool ui_sdl3_audio_active(const ui_audio_activity_t *activity, uint64_t now);
void ui_sdl3_on_local_audio(const audio_frame_t *audio, void *user);
ui_action_t ui_sdl3_event(ui_sdl3_t *ui, const SDL_Event *event);
void ui_sdl3_on_peer_joined(const char *peer_id, void *user);
void ui_sdl3_set_peer_name(ui_sdl3_t *ui, const char *peer_id, const char *name);
void ui_sdl3_on_message(const char *peer_id, const uint8_t *data, size_t len, void *user);
void ui_sdl3_add_chat(ui_sdl3_t *ui, const char *name, const uint8_t *data, size_t len);
int ui_sdl3_open_speaker(ui_sdl3_t *ui);
void ui_sdl3_close_devices(ui_sdl3_t *ui);
void ui_sdl3_set_status(ui_sdl3_t *ui, const char *message);

/* Initialize SDL3 and create window */
int ui_sdl3_init(ui_sdl3_t *ui, int win_w, int win_h);

/* Open camera (returns 0 on success) */
int ui_sdl3_open_camera(ui_sdl3_t *ui, int width, int height);

/* Open audio devices */
int ui_sdl3_open_audio(ui_sdl3_t *ui, int sample_rate, int channels);

/* Get a camera frame (caller must release). Returns NULL if no frame ready. */
SDL_Surface *ui_sdl3_acquire_camera_frame(ui_sdl3_t *ui);
void ui_sdl3_release_camera_frame(ui_sdl3_t *ui, SDL_Surface *frame);

/* Convert SDL_Surface to video_frame_t (points into surface pixels, no copy) */
int ui_sdl3_surface_to_frame(SDL_Surface *surface, video_frame_t *out);

/* Update local preview texture from a camera frame */
void ui_sdl3_update_preview(ui_sdl3_t *ui, const video_frame_t *frame);

/* Renderer callback: update a peer's tile texture */
void ui_sdl3_on_video_frame(const char *peer_id, const char *label, const video_frame_t *frame,
                            void *user);

/* Renderer callback: play decoded audio */
void ui_sdl3_on_audio_samples(const char *peer_id, const char *label, const audio_frame_t *audio,
                              void *user);

/* Render the tiled grid + local preview */
void ui_sdl3_render(ui_sdl3_t *ui);

/* Remove a peer's tiles */
void ui_sdl3_remove_peer(ui_sdl3_t *ui, const char *peer_id);
void ui_sdl3_on_peer_left(const char *peer_id, void *user);
void ui_sdl3_on_topology(const char *mode, const char *status, uint32_t epoch, const char *reason,
                         void *user);
void ui_sdl3_on_error(const char *message, void *user);

/* Cleanup */
void ui_sdl3_close(ui_sdl3_t *ui);

#endif /* UI_SDL3_H */
