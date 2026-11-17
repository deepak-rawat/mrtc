/*
 * SDL3 native UI implementation.
 */
#include "ui_sdl3.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

int ui_sdl3_init(ui_sdl3_t *ui, int win_w, int win_h) {
    memset(ui, 0, sizeof(*ui));
    ui->focused_field = 0;
    snprintf(ui->server, sizeof(ui->server), "ws://127.0.0.1:8080");
    snprintf(ui->room, sizeof(ui->room), "demo");
    snprintf(ui->name, sizeof(ui->name), "Guest");
    snprintf(ui->mode, sizeof(ui->mode), "P2P");
    rtc_str_map_init_owned(&ui->tile_index);
    if (rtc_mutex_init(&ui->tiles_mutex) != RTC_OK) {
        RTC_LOG_ERR("UI tile mutex initialization failed");
        return -1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_CAMERA)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        ui_sdl3_close(ui);
        return -1;
    }

    ui->window = SDL_CreateWindow("mrtc Conference", win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!ui->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        ui_sdl3_close(ui);
        return -1;
    }
    SDL_SetWindowMinimumSize(ui->window, 640, 480);
    SDL_StartTextInput(ui->window);

    ui->renderer = SDL_CreateRenderer(ui->window, NULL);
    if (!ui->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        ui_sdl3_close(ui);
        return -1;
    }

    return 0;
}

int ui_sdl3_open_camera(ui_sdl3_t *ui, int width, int height) {
    int count = 0;
    SDL_CameraID *cameras = SDL_GetCameras(&count);
    if (!cameras || count == 0) {
        SDL_free(cameras);
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
    if (!SDL_ResumeAudioStreamDevice(ui->mic_stream))
        return -1;
    return ui_sdl3_open_speaker(ui);
}

int ui_sdl3_open_speaker(ui_sdl3_t *ui) {
    if (ui->speaker_stream)
        return 0;
    SDL_AudioSpec spk_spec;
    spk_spec.format = SDL_AUDIO_S16;
    spk_spec.channels = 1;
    spk_spec.freq = 48000;

    ui->speaker_stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spk_spec, NULL, NULL);
    if (!ui->speaker_stream) {
        fprintf(stderr, "Speaker open failed: %s\n", SDL_GetError());
        return -1;
    }
    return SDL_ResumeAudioStreamDevice(ui->speaker_stream) ? 0 : -1;
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
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return -1;

    /* SDL may give us I420 (IYUV) directly if camera supports it */
    if (surface->format == SDL_PIXELFORMAT_IYUV) {
        uint8_t *pixels = (uint8_t *)surface->pixels;
        out->planes[0] = pixels;
        int pitch = surface->pitch;
        int uv_pitch = (pitch + 1) / 2;
        out->planes[1] = pixels + pitch * h;
        out->planes[2] = pixels + pitch * h + uv_pitch * (h / 2);
        out->stride[0] = pitch;
        out->stride[1] = uv_pitch;
        out->stride[2] = uv_pitch;
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

/* Build composite key "peer_id:label" into out (size must be >= 64+1+32+1). */
static void ui_tile_key(char *out, size_t out_sz, const char *peer_id, const char *label) {
    snprintf(out, out_sz, "%s:%s", peer_id, label);
}

static ui_tile_t *find_tile(ui_sdl3_t *ui, const char *peer_id, const char *label) {
    char key[128];
    ui_tile_key(key, sizeof(key), peer_id, label);
    return (ui_tile_t *)rtc_str_map_get(&ui->tile_index, key);
}

static ui_tile_t *find_or_create_tile(ui_sdl3_t *ui, const char *peer_id, const char *label) {
    ui_tile_t *t = find_tile(ui, peer_id, label);
    if (t)
        return t->remove_pending ? NULL : t;

    for (int i = 0; i < ui->tile_count; i++) {
        if (!ui->tiles[i].active) {
            t = &ui->tiles[i];
            break;
        }
    }
    if (!t) {
        if (ui->tile_count >= UI_MAX_TILES) {
            RTC_LOG_WARN("UI tile limit reached for peer %s", peer_id);
            return NULL;
        }
        t = &ui->tiles[ui->tile_count++];
    }
    memset(t, 0, sizeof(*t));
    snprintf(t->peer_id, sizeof(t->peer_id), "%s", peer_id);
    snprintf(t->label, sizeof(t->label), "%s", label);
    snprintf(t->name, sizeof(t->name), "%s", peer_id);
    t->active = true;
    char key[128];
    ui_tile_key(key, sizeof(key), peer_id, label);
    if (rtc_str_map_set(&ui->tile_index, key, t) != RTC_OK) {
        t->active = false;
        RTC_LOG_ERR("UI tile index allocation failed for peer %s", peer_id);
        return NULL;
    }
    return t;
}

void ui_sdl3_on_peer_joined(const char *peer_id, void *user) {
    ui_sdl3_t *ui = user;
    rtc_mutex_lock(&ui->tiles_mutex);
    find_or_create_tile(ui, peer_id, "camera");
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_set_peer_name(ui_sdl3_t *ui, const char *peer_id, const char *name) {
    rtc_mutex_lock(&ui->tiles_mutex);
    ui_tile_t *tile = find_or_create_tile(ui, peer_id, "camera");
    if (tile)
        snprintf(tile->name, sizeof(tile->name), "%s", name && name[0] ? name : peer_id);
    for (int i = 0; i < ui->tile_count; i++)
        if (ui->tiles[i].active && !strcmp(ui->tiles[i].peer_id, peer_id))
            snprintf(ui->tiles[i].name, sizeof(ui->tiles[i].name), "%s",
                     name && name[0] ? name : peer_id);
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_on_video_frame(const char *peer_id, const char *label, const video_frame_t *frame,
                            void *user) {
    ui_sdl3_t *ui = (ui_sdl3_t *)user;
    if (!ui || !frame || !frame->planes[0])
        return;
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 8192 || frame->height > 8192 ||
        (frame->width & 1) || (frame->height & 1) || !frame->planes[1] || !frame->planes[2] ||
        frame->stride[0] < frame->width || frame->stride[1] < frame->width / 2 ||
        frame->stride[2] < frame->width / 2)
        return;

    rtc_mutex_lock(&ui->tiles_mutex);
    ui_tile_t *tile = find_or_create_tile(ui, peer_id, label);
    if (!tile) {
        rtc_mutex_unlock(&ui->tiles_mutex);
        return;
    }

    int w = frame->width, h = frame->height;
    int y_size = w * h;
    int uv_size = (w / 2) * (h / 2);
    int total = y_size + 2 * uv_size;

    if (!tile->pending_yuv || tile->pending_w != w || tile->pending_h != h) {
        uint8_t *buf = (uint8_t *)malloc((size_t)total);
        if (!buf) {
            RTC_LOG_ERR("UI frame allocation failed for peer %s", peer_id);
            rtc_mutex_unlock(&ui->tiles_mutex);
            return;
        }
        free(tile->pending_yuv);
        tile->pending_yuv = buf;
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
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_on_audio_samples(const char *peer_id, const char *label, const audio_frame_t *audio,
                              void *user) {
    ui_sdl3_t *ui = (ui_sdl3_t *)user;
    (void)label;
    if (!ui || !audio || !audio->samples || audio->sample_count <= 0 || audio->channels <= 0)
        return;

    /* Compute RMS audio level */
    float sum = 0;
    int n = audio->sample_count < 256 ? audio->sample_count : 256;
    for (int i = 0; i < n; i++)
        sum += (float)audio->samples[i] * (float)audio->samples[i];
    float level = sqrtf(sum / (float)n) / 32768.0f;

    /* Update tile's audio level */
    rtc_mutex_lock(&ui->tiles_mutex);
    ui_tile_t *tile = find_tile(ui, peer_id, "camera");
    if (!tile)
        tile = find_tile(ui, peer_id, "screen");
    if (tile && !tile->remove_pending) {
        tile->audio_level = level;
        tile->audio_time = rtc_time_ms();
    }
    size_t samples = (size_t)audio->sample_count * audio->channels;
    size_t capacity = sizeof(ui->pending_audio) / sizeof(ui->pending_audio[0]);
    if (samples <= capacity - ui->pending_audio_count) {
        memcpy(ui->pending_audio + ui->pending_audio_count, audio->samples,
               samples * sizeof(int16_t));
        ui->pending_audio_count += samples;
    }
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_on_topology(const char *mode, const char *status, uint32_t epoch, const char *reason,
                         void *user) {
    ui_sdl3_t *ui = user;
    rtc_mutex_lock(&ui->tiles_mutex);
    snprintf(ui->pending_status, sizeof(ui->pending_status), "mrtc — %s%s (epoch %u)%s%s",
             strcmp(status, "switching") == 0 ? "Switching to "
             : strcmp(status, "error") == 0   ? "Error — "
                                              : "",
             strcmp(mode, "sfu") == 0 ? "SFU" : "P2P", epoch, reason && reason[0] ? " — " : "",
             reason ? reason : "");
    ui->status_dirty = true;
    snprintf(ui->mode, sizeof(ui->mode), "%s", strcmp(mode, "sfu") == 0 ? "SFU" : "P2P");
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_on_error(const char *message, void *user) {
    ui_sdl3_t *ui = user;
    rtc_mutex_lock(&ui->tiles_mutex);
    snprintf(ui->pending_status, sizeof(ui->pending_status), "mrtc — Error: %s", message);
    ui->status_dirty = true;
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_set_status(ui_sdl3_t *ui, const char *message) {
    rtc_mutex_lock(&ui->tiles_mutex);
    snprintf(ui->pending_status, sizeof(ui->pending_status), "%s", message);
    ui->status_dirty = true;
    rtc_mutex_unlock(&ui->tiles_mutex);
}

/* DebugText has an ASCII atlas. Preserve UTF-8 input; display each non-ASCII
 * code point as one '?' rather than rendering its individual bytes. */
static void display_text(const char *input, char *out, size_t capacity) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && n + 1 < capacity; p++) {
        if (*p < 128)
            out[n++] = *p >= 32 ? (char)*p : ' ';
        else if ((*p & 0xc0) != 0x80)
            out[n++] = '?';
    }
    out[n] = 0;
}

static void text(ui_sdl3_t *ui, float x, float y, const char *value, float width) {
    char ascii[512];
    display_text(value, ascii, sizeof(ascii));
    size_t max_chars = width > 0 ? (size_t)(width / 16) : 0;
    if (max_chars < strlen(ascii)) {
        ascii[max_chars] = 0;
        if (max_chars >= 3)
            memcpy(ascii + max_chars - 3, "...", 3);
    }
    SDL_SetRenderScale(ui->renderer, 2, 2);
    SDL_SetRenderDrawColor(ui->renderer, 231, 236, 245, 255);
    SDL_RenderDebugText(ui->renderer, x / 2, y / 2, ascii);
    SDL_SetRenderScale(ui->renderer, 1, 1);
}

static void box(ui_sdl3_t *ui, SDL_FRect r, bool selected) {
    SDL_SetRenderDrawColor(ui->renderer, selected ? 42 : 29, selected ? 78 : 37,
                           selected ? 100 : 52, 255);
    SDL_RenderFillRect(ui->renderer, &r);
}

size_t ui_sdl3_wrap_line(const char *value, size_t columns) {
    size_t length = strlen(value);
    if (length <= columns)
        return length;
    for (size_t i = columns; i > 0; i--)
        if (value[i] == ' ')
            return i;
    return columns;
}

static int chat_text(ui_sdl3_t *ui, float x, float y, const char *value, float width) {
    char ascii[512];
    display_text(value, ascii, sizeof(ascii));
    size_t columns = width > 16 ? (size_t)(width / 16) : 1;
    size_t length = strlen(ascii), offset = 0;
    int rows = 0;
    while (offset < length && rows < 2) {
        char line[512];
        size_t count = ui_sdl3_wrap_line(ascii + offset, columns);
        memcpy(line, ascii + offset, count);
        line[count] = 0;
        if (rows == 1 && offset + count < length && count >= 3)
            memcpy(line + count - 3, "...", 3);
        text(ui, x, y + rows * 22, line, width);
        offset += count;
        while (ascii[offset] == ' ')
            offset++;
        rows++;
    }
    return rows ? rows : 1;
}

void ui_sdl3_layout(float w, float h, bool chat_open, ui_layout_t *out) {
    memset(out, 0, sizeof(*out));
    float chat_w = chat_open ? fminf(336, w * .38f) : 0;
    out->gallery = (SDL_FRect){16, 94, w - 32 - chat_w, fmaxf(0, h - 182)};
    out->chat = (SDL_FRect){w - chat_w, 94, chat_w - 16, fmaxf(0, h - 182)};
    float button_w = fminf(160, (w - 48) / 5);
    for (int i = 0; i < 5; i++)
        out->controls[i] = (SDL_FRect){16 + i * (button_w + 4), h - 68, button_w, 48};
    float form_w = fminf(560, w - 48);
    for (int i = 0; i < 3; i++)
        out->fields[i] = (SDL_FRect){24, 112 + i * 64, form_w, 38};
    for (int i = 0; i < 2; i++)
        out->sources[i] = (SDL_FRect){24 + i * (form_w / 2 + 4), 318, form_w / 2 - 4, 40};
    out->join = (SDL_FRect){24, 378, form_w, 46};
}

SDL_FRect ui_sdl3_fit(SDL_FRect r, int width, int height) {
    if (width <= 0 || height <= 0)
        return r;
    float scale = fminf(r.w / width, r.h / height);
    float w = width * scale, h = height * scale;
    return (SDL_FRect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
}

SDL_FRect ui_sdl3_tile_rect(SDL_FRect bounds, int count, int index) {
    int columns = 1;
    float best_area = -1;
    for (int candidate = 1; candidate <= count; candidate++) {
        int rows = (count + candidate - 1) / candidate;
        float width = fmaxf(0, bounds.w / candidate - 8);
        float height = fmaxf(0, bounds.h / rows - 42);
        SDL_FRect image = ui_sdl3_fit((SDL_FRect){0, 0, width, height}, 4, 3);
        float area = image.w * image.h;
        if (area > best_area) {
            columns = candidate;
            best_area = area;
        }
    }
    int rows = (count + columns - 1) / columns;
    float width = bounds.w / columns, height = bounds.h / (rows ? rows : 1);
    return (SDL_FRect){bounds.x + (index % columns) * width, bounds.y + (index / columns) * height,
                       fmaxf(0, width - 8), fmaxf(0, height - 8)};
}

static bool contains(SDL_FRect rect, float x, float y) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static char *edit_buffer(ui_sdl3_t *ui, size_t *capacity) {
    if (!ui->in_call && ui->focused_field >= 0 && ui->focused_field < 3) {
        char *fields[] = {ui->server, ui->room, ui->name};
        size_t sizes[] = {sizeof(ui->server), sizeof(ui->room), sizeof(ui->name)};
        *capacity = sizes[ui->focused_field];
        return fields[ui->focused_field];
    }
    if (ui->in_call && ui->chat_open && ui->focused_field == 3) {
        *capacity = sizeof(ui->chat_input);
        return ui->chat_input;
    }
    return NULL;
}

ui_action_t ui_sdl3_event(ui_sdl3_t *ui, const SDL_Event *event) {
    int w, h;
    SDL_GetWindowSize(ui->window, &w, &h);
    ui_layout_t layout;
    ui_sdl3_layout((float)w, (float)h, ui->chat_open, &layout);
    if (event->type == SDL_EVENT_QUIT)
        return UI_ACTION_LEAVE;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        float x = event->button.x, y = event->button.y;
        ui->focused_field = -1;
        if (!ui->in_call) {
            for (int i = 0; i < 3; i++)
                if (contains(layout.fields[i], x, y))
                    ui->focused_field = i;
            if (contains(layout.sources[0], x, y))
                ui->use_camera = !ui->use_camera;
            if (contains(layout.sources[1], x, y))
                ui->use_mic = !ui->use_mic;
            if (contains(layout.join, x, y))
                return UI_ACTION_JOIN;
        } else {
            const ui_action_t actions[] = {UI_ACTION_MUTE, UI_ACTION_CAMERA, UI_ACTION_CHAT,
                                           UI_ACTION_SEND, UI_ACTION_LEAVE};
            for (int i = 0; i < 5; i++)
                if (contains(layout.controls[i], x, y))
                    return actions[i];
            if (ui->chat_open && contains(layout.chat, x, y))
                ui->focused_field = 3;
        }
    }
    size_t capacity = 0;
    char *buffer = edit_buffer(ui, &capacity);
    if (event->type == SDL_EVENT_TEXT_INPUT && buffer) {
        size_t len = strlen(buffer), add = strlen(event->text.text);
        if (len + add < capacity)
            memcpy(buffer + len, event->text.text, add + 1);
    }
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat)
        return UI_ACTION_NONE;
    SDL_Keycode key = event->key.key;
    if (key == SDLK_ESCAPE) {
        ui->focused_field = -1;
        return UI_ACTION_NONE;
    }
    if (key == SDLK_TAB) {
        ui->focused_field = ui->in_call ? (ui->chat_open ? 3 : -1) : (ui->focused_field + 1) % 3;
        return UI_ACTION_NONE;
    }
    if (buffer) {
        if (key == SDLK_BACKSPACE) {
            size_t len = strlen(buffer);
            if (len) {
                do {
                    len--;
                } while (len && ((unsigned char)buffer[len] & 0xc0) == 0x80);
                buffer[len] = 0;
            }
        }
        if (key == SDLK_RETURN)
            return ui->in_call ? UI_ACTION_SEND : UI_ACTION_JOIN;
        return UI_ACTION_NONE;
    }
    if (ui->in_call && !(event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        if (key == SDLK_M)
            return UI_ACTION_MUTE;
        if (key == SDLK_V)
            return UI_ACTION_CAMERA;
        if (key == SDLK_C)
            return UI_ACTION_CHAT;
    }
    return UI_ACTION_NONE;
}

void ui_sdl3_add_chat(ui_sdl3_t *ui, const char *name, const uint8_t *data, size_t len) {
    if (!data || !len)
        return;
    rtc_mutex_lock(&ui->tiles_mutex);
    if (ui->chat_count == UI_CHAT_LINES) {
        memmove(ui->chat_lines, ui->chat_lines + 1,
                (UI_CHAT_LINES - 1) * sizeof(ui->chat_lines[0]));
        ui->chat_count--;
    }
    char message[257];
    /* Reject invalid UTF-8, including binary data; never truncate a code point. */
    size_t used = 0;
    while (used < len && used < sizeof(message) - 1) {
        unsigned char c = data[used];
        size_t bytes = c < 0x80                 ? 1
                       : c >= 0xc2 && c <= 0xdf ? 2
                       : c >= 0xe0 && c <= 0xef ? 3
                       : c >= 0xf0 && c <= 0xf4 ? 4
                                                : 0;
        if (!bytes || used + bytes > len)
            break;
        bool valid = true;
        for (size_t i = 1; i < bytes; i++)
            if ((data[used + i] & 0xc0) != 0x80)
                valid = false;
        if (bytes == 3 &&
            ((c == 0xe0 && data[used + 1] < 0xa0) || (c == 0xed && data[used + 1] >= 0xa0)))
            valid = false;
        if (bytes == 4 &&
            ((c == 0xf0 && data[used + 1] < 0x90) || (c == 0xf4 && data[used + 1] >= 0x90)))
            valid = false;
        if (!valid || c == 0)
            break;
        if (used + bytes > sizeof(message) - 1)
            break;
        memcpy(message + used, data + used, bytes);
        used += bytes;
    }
    message[used] = 0;
    if (used)
        snprintf(ui->chat_lines[ui->chat_count++], sizeof(ui->chat_lines[0]), "%s: %s", name,
                 message);
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_on_message(const char *peer_id, const uint8_t *data, size_t len, void *user) {
    ui_sdl3_t *ui = user;
    char name[128];
    rtc_mutex_lock(&ui->tiles_mutex);
    ui_tile_t *tile = find_tile(ui, peer_id, "camera");
    snprintf(name, sizeof(name), "%s", tile ? tile->name : peer_id);
    rtc_mutex_unlock(&ui->tiles_mutex);
    ui_sdl3_add_chat(ui, name, data, len);
}

static void draw_tile(ui_sdl3_t *ui, SDL_FRect r, SDL_Texture *texture, int w, int h,
                      const char *name, bool speaking, const char *placeholder) {
    box(ui, r, speaking);
    if (speaking) {
        SDL_SetRenderDrawColor(ui->renderer, 64, 210, 140, 255);
        SDL_RenderRect(ui->renderer, &r);
        SDL_FRect activity = {r.x + r.w - 16, r.y + r.h - 22, 8, 8};
        SDL_RenderFillRect(ui->renderer, &activity);
    }
    SDL_FRect image = {r.x + 3, r.y + 3, r.w - 6, fmaxf(0, r.h - 34)};
    if (texture) {
        SDL_FRect fit = ui_sdl3_fit(image, w, h);
        SDL_RenderTexture(ui->renderer, texture, NULL, &fit);
    } else {
        text(ui, r.x + 12, r.y + 18, placeholder, r.w - 24);
    }
    text(ui, r.x + 10, r.y + r.h - 25, name, r.w - 20);
}

static void draw_screen(ui_sdl3_t *ui) {
    int w, h;
    SDL_GetWindowSize(ui->window, &w, &h);
    ui_layout_t l;
    ui_sdl3_layout((float)w, (float)h, ui->chat_open, &l);
    if (!ui->in_call) {
        text(ui, 24, 20, "mrtc / Join a room", w - 48);
        text(ui, 24, 52, "Choose your sources, then join.", w - 48);
        const char *labels[] = {"Server", "Room", "Your name"};
        const char *values[] = {ui->server, ui->room, ui->name};
        for (int i = 0; i < 3; i++) {
            text(ui, 24, l.fields[i].y - 22, labels[i], w - 48);
            box(ui, l.fields[i], ui->focused_field == i);
            text(ui, 32, l.fields[i].y + 10, values[i], l.fields[i].w - 16);
        }
        box(ui, l.sources[0], ui->use_camera);
        box(ui, l.sources[1], ui->use_mic);
        text(ui, l.sources[0].x + 8, 330, ui->use_camera ? "Video: camera" : "Video: synthetic",
             l.sources[0].w - 16);
        text(ui, l.sources[1].x + 8, 330, ui->use_mic ? "Audio: mic" : "Audio: tone",
             l.sources[1].w - 16);
        box(ui, l.join, true);
        text(ui, 40, 392, "Join room", l.join.w - 32);
        if (w >= 1000)
            draw_tile(ui, (SDL_FRect){(float)w - 360, 112, 320, 260}, ui->local_preview,
                      ui->preview_w, ui->preview_h, "You / preview", false,
                      ui->use_camera ? "Waiting for camera" : "You / preview");
        else if (h >= 660)
            draw_tile(ui, (SDL_FRect){24, 450, fminf(320, w - 48), 180}, ui->local_preview,
                      ui->preview_w, ui->preview_h, "You / preview", false,
                      ui->use_camera ? "Waiting for camera" : "You / preview");
    } else {
        int participants = 1, count = 1;
        for (int i = 0; i < ui->tile_count; i++)
            if (ui->tiles[i].active && !ui->tiles[i].remove_pending) {
                count++;
                if (!strcmp(ui->tiles[i].label, "camera"))
                    participants++;
            }
        char heading[256];
        snprintf(heading, sizeof(heading), "%s | %s | %d participants", ui->room, ui->mode,
                 participants);
        text(ui, 16, 18, heading, w - 32);
        int index = 0;
        for (int i = 0; i <= ui->tile_count; i++) {
            ui_tile_t *t = i < ui->tile_count ? &ui->tiles[i] : NULL;
            if (t && (!t->active || t->remove_pending))
                continue;
            SDL_FRect rect = ui_sdl3_tile_rect(l.gallery, count, index);
            char label[192];
            if (t)
                snprintf(label, sizeof(label), "%s%s%s", t->name,
                         strcmp(t->label, "camera") ? " / " : "",
                         strcmp(t->label, "camera") ? t->label : "");
            else
                snprintf(label, sizeof(label), "You%s", ui->mic_muted ? " / muted" : "");
            bool speaking = t ? t->audio_level > .015f && rtc_time_ms() - t->audio_time < 400
                              : !ui->mic_muted && ui->local_audio_level > .015f;
            draw_tile(ui, rect,
                      t               ? t->texture
                      : ui->cam_muted ? NULL
                                      : ui->local_preview,
                      t ? t->width : ui->preview_w, t ? t->height : ui->preview_h, label, speaking,
                      t               ? "Waiting for video"
                      : ui->cam_muted ? "Camera off"
                                      : "You / preview");
            index++;
        }
        const char *buttons[] = {ui->mic_muted ? "Unmute" : "Mute",
                                 ui->cam_muted ? "Enable" : "Camera", "Chat", "Send", "Leave"};
        for (int i = 0; i < 5; i++) {
            box(ui, l.controls[i], i == 4 || (i == 2 && ui->chat_open));
            text(ui, l.controls[i].x + 8, l.controls[i].y + 16, buttons[i], l.controls[i].w - 16);
        }
        if (ui->chat_open) {
            box(ui, l.chat, false);
            text(ui, l.chat.x + 8, l.chat.y + 8, "Chat", l.chat.w - 16);
            int visible = (int)((l.chat.h - 110) / 48);
            if (visible < 0)
                visible = 0;
            int start = ui->chat_count > visible ? ui->chat_count - visible : 0;
            float message_y = l.chat.y + 38;
            for (int i = start; i < ui->chat_count; i++)
                message_y +=
                    22 * chat_text(ui, l.chat.x + 8, message_y, ui->chat_lines[i], l.chat.w - 16) +
                    4;
            SDL_FRect input = {l.chat.x + 4, l.chat.y + l.chat.h - 60, l.chat.w - 8, 52};
            box(ui, input, ui->focused_field == 3);
            text(ui, input.x + 4, input.y + 12,
                 ui->chat_input[0] ? ui->chat_input : "Type, Enter to send", input.w - 8);
        }
    }
    text(ui, 16, ui->in_call ? 56 : h - 38, ui->status, w - 32);
}

void ui_sdl3_render(ui_sdl3_t *ui) {
    SDL_SetRenderDrawColor(ui->renderer, 30, 30, 30, 255);
    SDL_RenderClear(ui->renderer);

    rtc_mutex_lock(&ui->tiles_mutex);
    if (ui->status_dirty) {
        SDL_SetWindowTitle(ui->window, ui->pending_status);
        snprintf(ui->status, sizeof(ui->status), "%s", ui->pending_status);
        ui->status_dirty = false;
    }
    if (ui->pending_audio_count) {
        if (ui->speaker_stream)
            SDL_PutAudioStreamData(ui->speaker_stream, ui->pending_audio,
                                   (int)(ui->pending_audio_count * sizeof(int16_t)));
        ui->pending_audio_count = 0;
    }
    /* Process pending frames from transport thread → SDL textures (main thread) */
    for (int i = 0; i < ui->tile_count; i++) {
        ui_tile_t *t = &ui->tiles[i];
        if (t->remove_pending) {
            char key[128];
            ui_tile_key(key, sizeof(key), t->peer_id, t->label);
            rtc_str_map_remove(&ui->tile_index, key);
            if (t->texture)
                SDL_DestroyTexture(t->texture);
            free(t->pending_yuv);
            memset(t, 0, sizeof(*t));
        }
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
            if (SDL_UpdateYUVTexture(t->texture, NULL, t->pending_yuv, w, t->pending_yuv + y_size,
                                     uv_stride, t->pending_yuv + y_size + uv_size, uv_stride))
                ui->remote_video_uploads++;
        }
        t->pending_ready = false;
    }

    draw_screen(ui);
    rtc_mutex_unlock(&ui->tiles_mutex);
#ifdef UI_TEST_SNAPSHOTS
    const char *snapshot = SDL_getenv("MRTC_UI_SNAPSHOT");
    if (snapshot && snapshot[0]) {
        SDL_Surface *surface = SDL_RenderReadPixels(ui->renderer, NULL);
        if (surface) {
            if (!SDL_SaveBMP(surface, snapshot))
                fprintf(stderr, "UI snapshot failed: %s\n", SDL_GetError());
            if (!ui->in_call) {
                char join_path[1024];
                int length = snprintf(join_path, sizeof(join_path), "%s.join.bmp", snapshot);
                if (length > 0 && (size_t)length < sizeof(join_path) &&
                    !SDL_SaveBMP(surface, join_path))
                    fprintf(stderr, "Join snapshot failed: %s\n", SDL_GetError());
            }
            SDL_DestroySurface(surface);
        }
    }
#endif
    SDL_RenderPresent(ui->renderer);
}

void ui_sdl3_remove_peer(ui_sdl3_t *ui, const char *peer_id) {
    rtc_mutex_lock(&ui->tiles_mutex);
    for (int i = 0; i < ui->tile_count; i++) {
        if (ui->tiles[i].active && strcmp(ui->tiles[i].peer_id, peer_id) == 0)
            ui->tiles[i].remove_pending = true;
    }
    rtc_mutex_unlock(&ui->tiles_mutex);
}

void ui_sdl3_on_peer_left(const char *peer_id, void *user) {
    ui_sdl3_remove_peer((ui_sdl3_t *)user, peer_id);
}

void ui_sdl3_close_devices(ui_sdl3_t *ui) {
    if (ui->camera)
        SDL_CloseCamera(ui->camera);
    if (ui->mic_stream) {
        SDL_DestroyAudioStream(ui->mic_stream);
    }
    if (ui->speaker_stream) {
        SDL_DestroyAudioStream(ui->speaker_stream);
    }
    ui->camera = NULL;
    ui->mic_stream = NULL;
    ui->speaker_stream = NULL;
}

void ui_sdl3_close(ui_sdl3_t *ui) {
    ui_sdl3_close_devices(ui);
    for (int i = 0; i < ui->tile_count; i++) {
        if (ui->tiles[i].texture)
            SDL_DestroyTexture(ui->tiles[i].texture);
        free(ui->tiles[i].pending_yuv);
    }
    rtc_str_map_free(&ui->tile_index);
    rtc_mutex_destroy(&ui->tiles_mutex);
    if (ui->local_preview)
        SDL_DestroyTexture(ui->local_preview);
    if (ui->renderer)
        SDL_DestroyRenderer(ui->renderer);
    if (ui->window)
        SDL_DestroyWindow(ui->window);
}
