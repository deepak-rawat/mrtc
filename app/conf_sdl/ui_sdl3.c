/*
 * ui_sdl3.c - SDL3 native UI implementation.
 */
#include "ui_sdl3.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

int ui_sdl3_init(ui_sdl3_t *ui, int win_w, int win_h) {
    memset(ui, 0, sizeof(*ui));

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_CAMERA)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    ui->window = SDL_CreateWindow("mrtc Conference", win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!ui->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    ui->renderer = SDL_CreateRenderer(ui->window, NULL);
    if (!ui->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

int ui_sdl3_open_camera(ui_sdl3_t *ui, int width, int height) {
    int count = 0;
    SDL_CameraID *cameras = SDL_GetCameras(&count);
    if (!cameras || count == 0) {
        fprintf(stderr, "No cameras found\n");
        return -1;
    }

    SDL_CameraSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.format = SDL_PIXELFORMAT_IYUV;
    spec.width = width;
    spec.height = height;
    spec.framerate_numerator = 30;
    spec.framerate_denominator = 1;

    ui->camera = SDL_OpenCamera(cameras[0], &spec);
    SDL_free(cameras);

    if (!ui->camera) {
        fprintf(stderr, "SDL_OpenCamera failed: %s\n", SDL_GetError());
        return -1;
    }

    ui->cam_width = width;
    ui->cam_height = height;
    return 0;
}

int ui_sdl3_open_audio(ui_sdl3_t *ui, int sample_rate, int channels) {
    SDL_AudioSpec mic_spec;
    mic_spec.format = SDL_AUDIO_S16;
    mic_spec.channels = channels;
    mic_spec.freq = sample_rate;

    /* Microphone capture */
    ui->mic_stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &mic_spec, NULL, NULL);
    if (!ui->mic_stream) {
        fprintf(stderr, "Mic open failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_ResumeAudioStreamDevice(ui->mic_stream);

    /* Speaker playback */
    SDL_AudioSpec spk_spec;
    spk_spec.format = SDL_AUDIO_S16;
    spk_spec.channels = channels;
    spk_spec.freq = sample_rate;

    ui->speaker_stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spk_spec, NULL, NULL);
    if (!ui->speaker_stream) {
        fprintf(stderr, "Speaker open failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_ResumeAudioStreamDevice(ui->speaker_stream);

    return 0;
}

SDL_Surface *ui_sdl3_acquire_camera_frame(ui_sdl3_t *ui) {
    if (!ui->camera)
        return NULL;
    return SDL_AcquireCameraFrame(ui->camera, NULL);
}

void ui_sdl3_release_camera_frame(ui_sdl3_t *ui, SDL_Surface *frame) {
    if (ui->camera && frame)
        SDL_ReleaseCameraFrame(ui->camera, frame);
}

int ui_sdl3_surface_to_frame(SDL_Surface *surface, video_frame_t *out) {
    if (!surface || !surface->pixels)
        return -1;

    int w = surface->w;
    int h = surface->h;

    /* SDL may give us I420 (IYUV) directly if camera supports it */
    if (surface->format == SDL_PIXELFORMAT_IYUV) {
        uint8_t *pixels = (uint8_t *)surface->pixels;
        out->planes[0] = pixels;
        out->planes[1] = pixels + w * h;
        out->planes[2] = pixels + w * h + (w / 2) * (h / 2);
        out->stride[0] = w;
        out->stride[1] = w / 2;
        out->stride[2] = w / 2;
        out->width = w;
        out->height = h;
        return 0;
    }

    /* For other formats, we'd need conversion — not handled yet */
    return -1;
}

void ui_sdl3_update_preview(ui_sdl3_t *ui, const video_frame_t *frame) {
    if (!frame || !frame->planes[0])
        return;

    if (!ui->local_preview || ui->preview_w != frame->width || ui->preview_h != frame->height) {
        if (ui->local_preview)
            SDL_DestroyTexture(ui->local_preview);
        ui->local_preview =
            SDL_CreateTexture(ui->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                              frame->width, frame->height);
        ui->preview_w = frame->width;
        ui->preview_h = frame->height;
    }

    if (ui->local_preview) {
        SDL_UpdateYUVTexture(ui->local_preview, NULL, frame->planes[0], frame->stride[0],
                             frame->planes[1], frame->stride[1], frame->planes[2],
                             frame->stride[2]);
    }
}

/* ---- Tile management ---- */

static ui_tile_t *find_tile(ui_sdl3_t *ui, const char *peer_id, const char *label) {
    for (int i = 0; i < ui->tile_count; i++) {
        if (ui->tiles[i].active && strcmp(ui->tiles[i].peer_id, peer_id) == 0 &&
            strcmp(ui->tiles[i].label, label) == 0)
            return &ui->tiles[i];
    }
    return NULL;
}

static ui_tile_t *find_or_create_tile(ui_sdl3_t *ui, const char *peer_id, const char *label) {
    ui_tile_t *t = find_tile(ui, peer_id, label);
    if (t)
        return t;

    if (ui->tile_count >= UI_MAX_TILES)
        return NULL;
    t = &ui->tiles[ui->tile_count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->peer_id, sizeof(t->peer_id), "%s", peer_id);
    snprintf(t->label, sizeof(t->label), "%s", label);
    t->active = true;
    return t;
}

/* ---- Renderer callbacks ---- */

void ui_sdl3_on_video_frame(const char *peer_id, const char *label, const video_frame_t *frame,
                            void *user) {
    ui_sdl3_t *ui = (ui_sdl3_t *)user;
    if (!ui || !frame || !frame->planes[0])
        return;
    if (frame->width <= 0 || frame->height <= 0)
        return;

    ui_tile_t *tile = find_or_create_tile(ui, peer_id, label);
    if (!tile)
        return;

    int w = frame->width, h = frame->height;
    int y_size = w * h;
    int uv_size = (w / 2) * (h / 2);
    int total = y_size + 2 * uv_size;

    /*
     * Copy frame into a pending buffer (transport thread safe).
     * The main thread will upload to SDL texture in ui_sdl3_render().
     */
    if (!tile->pending_yuv || tile->pending_w != w || tile->pending_h != h) {
        free(tile->pending_yuv);
        tile->pending_yuv = (uint8_t *)malloc((size_t)total);
        tile->pending_w = w;
        tile->pending_h = h;
    }

    if (tile->pending_yuv) {
        /* Copy Y */
        for (int y = 0; y < h; y++)
            memcpy(tile->pending_yuv + y * w, frame->planes[0] + y * frame->stride[0], (size_t)w);
        /* Copy U */
        for (int y = 0; y < h / 2; y++)
            memcpy(tile->pending_yuv + y_size + y * (w / 2),
                   frame->planes[1] + y * frame->stride[1], (size_t)(w / 2));
        /* Copy V */
        for (int y = 0; y < h / 2; y++)
            memcpy(tile->pending_yuv + y_size + uv_size + y * (w / 2),
                   frame->planes[2] + y * frame->stride[2], (size_t)(w / 2));
        tile->pending_ready = true;
    }
}

void ui_sdl3_on_audio_samples(const char *peer_id, const char *label, const audio_frame_t *audio,
                              void *user) {
    ui_sdl3_t *ui = (ui_sdl3_t *)user;
    (void)label;
    if (!ui || !audio || !audio->samples)
        return;

    /* Compute RMS audio level */
    float sum = 0;
    int n = audio->sample_count < 256 ? audio->sample_count : 256;
    for (int i = 0; i < n; i++)
        sum += (float)audio->samples[i] * (float)audio->samples[i];
    float level = sqrtf(sum / (float)n) / 32768.0f;

    /* Update tile's audio level */
    ui_tile_t *tile = find_tile(ui, peer_id, "camera");
    if (!tile)
        tile = find_tile(ui, peer_id, "screen");
    if (tile)
        tile->audio_level = level;

    /* Play through speaker */
    if (ui->speaker_stream)
        SDL_PutAudioStreamData(ui->speaker_stream, audio->samples,
                               audio->sample_count * audio->channels * (int)sizeof(int16_t));
}

/* ---- Rendering ---- */

void ui_sdl3_render(ui_sdl3_t *ui) {
    SDL_SetRenderDrawColor(ui->renderer, 30, 30, 30, 255);
    SDL_RenderClear(ui->renderer);

    /* Process pending frames from transport thread → SDL textures (main thread) */
    for (int i = 0; i < ui->tile_count; i++) {
        ui_tile_t *t = &ui->tiles[i];
        if (!t->active || !t->pending_ready || !t->pending_yuv)
            continue;

        int w = t->pending_w, h = t->pending_h;

        /* Create/recreate texture if needed */
        if (!t->texture || t->width != w || t->height != h) {
            if (t->texture)
                SDL_DestroyTexture(t->texture);
            t->texture = SDL_CreateTexture(ui->renderer, SDL_PIXELFORMAT_IYUV,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
            t->width = w;
            t->height = h;
        }

        if (t->texture) {
            int y_size = w * h;
            int uv_stride = w / 2;
            int uv_size = uv_stride * (h / 2);
            SDL_UpdateYUVTexture(t->texture, NULL, t->pending_yuv, w, t->pending_yuv + y_size,
                                 uv_stride, t->pending_yuv + y_size + uv_size, uv_stride);
        }
        t->pending_ready = false;
    }

    /* Calculate adaptive grid layout */
    int n = 0;
    for (int i = 0; i < ui->tile_count; i++)
        if (ui->tiles[i].active && ui->tiles[i].texture)
            n++;

    if (n > 0) {
        int cols = (n <= 1) ? 1 : (n <= 4) ? 2 : 3;
        int rows = (n + cols - 1) / cols;
        int win_w, win_h;
        SDL_GetWindowSize(ui->window, &win_w, &win_h);
        float tw = (float)win_w / (float)cols;
        float th = (float)win_h / (float)rows;

        int idx = 0;
        for (int i = 0; i < ui->tile_count; i++) {
            if (!ui->tiles[i].active || !ui->tiles[i].texture)
                continue;
            SDL_FRect dst = {(float)(idx % cols) * tw, (float)(idx / cols) * th, tw, th};
            SDL_RenderTexture(ui->renderer, ui->tiles[i].texture, NULL, &dst);

            /* Audio level bar at bottom of tile */
            float level = ui->tiles[i].audio_level;
            if (level > 1.0f)
                level = 1.0f;
            float bar_h = 6.0f;
            float bar_w = tw * level;
            SDL_FRect bar = {dst.x, dst.y + th - bar_h, bar_w, bar_h};
            /* Green when active, dark gray background */
            SDL_SetRenderDrawColor(ui->renderer, 50, 50, 50, 200);
            SDL_FRect bar_bg = {dst.x, dst.y + th - bar_h, tw, bar_h};
            SDL_RenderFillRect(ui->renderer, &bar_bg);
            if (level > 0.005f) {
                uint8_t g = (uint8_t)(100 + level * 155);
                SDL_SetRenderDrawColor(ui->renderer, 30, g, 30, 255);
                SDL_RenderFillRect(ui->renderer, &bar);
            }

            /* Decay level for next frame */
            ui->tiles[i].audio_level *= 0.85f;

            idx++;
        }
    }

    /* Local preview (picture-in-picture, bottom-right) */
    if (ui->local_preview) {
        int win_w, win_h;
        SDL_GetWindowSize(ui->window, &win_w, &win_h);
        float pip_w = 160, pip_h = 120;
        SDL_FRect pip = {(float)win_w - pip_w - 10, (float)win_h - pip_h - 10, pip_w, pip_h};
        SDL_RenderTexture(ui->renderer, ui->local_preview, NULL, &pip);

        /* Local mic level bar below preview */
        float level = ui->local_audio_level;
        if (level > 1.0f)
            level = 1.0f;
        float bar_h = 4.0f;
        SDL_FRect mic_bg = {pip.x, pip.y + pip_h + 2, pip_w, bar_h};
        SDL_SetRenderDrawColor(ui->renderer, 50, 50, 50, 200);
        SDL_RenderFillRect(ui->renderer, &mic_bg);
        if (level > 0.005f) {
            float bar_w = pip_w * level;
            SDL_FRect mic_bar = {pip.x, pip.y + pip_h + 2, bar_w, bar_h};
            uint8_t g = (uint8_t)(100 + level * 155);
            SDL_SetRenderDrawColor(ui->renderer, 30, g, 30, 255);
            SDL_RenderFillRect(ui->renderer, &mic_bar);
        }
        ui->local_audio_level *= 0.85f;
    }

    SDL_RenderPresent(ui->renderer);
}

void ui_sdl3_remove_peer(ui_sdl3_t *ui, const char *peer_id) {
    for (int i = 0; i < ui->tile_count; i++) {
        if (ui->tiles[i].active && strcmp(ui->tiles[i].peer_id, peer_id) == 0) {
            if (ui->tiles[i].texture)
                SDL_DestroyTexture(ui->tiles[i].texture);
            ui->tiles[i].active = false;
            ui->tiles[i].texture = NULL;
        }
    }
}

void ui_sdl3_close(ui_sdl3_t *ui) {
    if (ui->camera)
        SDL_CloseCamera(ui->camera);
    if (ui->mic_stream) {
        SDL_DestroyAudioStream(ui->mic_stream);
    }
    if (ui->speaker_stream) {
        SDL_DestroyAudioStream(ui->speaker_stream);
    }
    for (int i = 0; i < ui->tile_count; i++) {
        if (ui->tiles[i].texture)
            SDL_DestroyTexture(ui->tiles[i].texture);
        free(ui->tiles[i].pending_yuv);
    }
    if (ui->local_preview)
        SDL_DestroyTexture(ui->local_preview);
    if (ui->renderer)
        SDL_DestroyRenderer(ui->renderer);
    if (ui->window)
        SDL_DestroyWindow(ui->window);
}
