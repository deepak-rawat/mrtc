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

#define UI_MAX_TILES 16

typedef struct {
    char peer_id[64];
    char label[32];
    SDL_Texture *texture;
    int width, height;
    float audio_level; /* 0.0-1.0, updated from audio callback */
    bool active;

    /* Pending frame from transport thread (thread-safe handoff) */
    uint8_t *pending_yuv; /* I420 buffer, atomically swapped */
    int pending_w, pending_h;
    volatile bool pending_ready;
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

    /* Local preview */
    SDL_Texture *local_preview;
    int preview_w, preview_h;
    float local_audio_level; /* 0.0-1.0, for mic level bar */
} ui_sdl3_t;

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

/* Cleanup */
void ui_sdl3_close(ui_sdl3_t *ui);

#endif /* UI_SDL3_H */
