#include "ui_sdl3.h"
#include <app_session.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void preview(const video_frame_t *frame, void *user) {
    ui_sdl3_update_preview(user, frame);
}

static void joined(const char *id, void *user) {
    (void)id;
    ui_sdl3_set_status(user, "Joined room. Waiting for participants.");
}

static int prepare_devices(ui_sdl3_t *ui) {
    ui_sdl3_close_devices(ui);
    if (ui->use_camera &&
        ui_sdl3_open_camera(ui, APP_SESSION_VIDEO_WIDTH, APP_SESSION_VIDEO_HEIGHT) != 0) {
        ui_sdl3_on_error("Camera unavailable; select Synthetic or allow camera access.", ui);
        goto failed;
    }
    if (ui->use_mic && ui_sdl3_open_audio(ui, 48000, 1) != 0) {
        ui_sdl3_on_error("Microphone unavailable; select Tone or allow microphone access.", ui);
        goto failed;
    }
    if (ui_sdl3_open_speaker(ui) != 0) {
        ui_sdl3_on_error("Speaker unavailable. Check your audio output device.", ui);
        goto failed;
    }
    return 0;
failed:
    ui_sdl3_close_devices(ui);
    return -1;
}

static void capture(ui_sdl3_t *ui, app_session_t *session) {
    conference_t *conference = app_session_conference(session);
    if (ui->camera) {
        SDL_Surface *surface = ui_sdl3_acquire_camera_frame(ui);
        if (surface) {
            video_frame_t frame = {0};
            if (ui_sdl3_surface_to_frame(surface, &frame) == 0) {
                ui_sdl3_update_preview(ui, &frame);
                if (conference)
                    conference_push_video(conference, "camera", &frame);
            } else {
                ui_sdl3_on_error("Camera format unsupported (requires I420). Select Synthetic.",
                                 ui);
            }
            ui_sdl3_release_camera_frame(ui, surface);
        }
    }
    if (ui->mic_stream) {
        int16_t pcm[960];
        if (SDL_GetAudioStreamAvailable(ui->mic_stream) >= (int)sizeof(pcm)) {
            int bytes = SDL_GetAudioStreamData(ui->mic_stream, pcm, sizeof(pcm));
            if (bytes == (int)sizeof(pcm)) {
                audio_frame_t audio = {
                    .samples = pcm, .sample_count = 960, .sample_rate = 48000, .channels = 1};
                ui_sdl3_on_local_audio(&audio, ui);
                conference_push_audio(conference, "mic", &audio);
            } else if (bytes < 0) {
                ui_sdl3_on_error("Microphone capture failed.", ui);
            }
        }
    }
}

static int write_stats(const char *path, ui_sdl3_t *ui, app_session_t *session) {
    app_session_snapshot_t snapshot;
    app_session_get_snapshot(session, &snapshot);
    uint64_t video = 0, audio = 0, samples = 0;
    for (size_t i = 0; i < snapshot.peer_count; i++) {
        video += snapshot.peers[i].video_frames;
        audio += snapshot.peers[i].audio_frames;
        samples += snapshot.peers[i].audio_samples;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Cannot write desktop stats: %s\n", path);
        return -1;
    }
    int result = fprintf(file,
                         "{\"joined\":%s,\"epoch\":%u,\"peers\":%zu,"
                         "\"video_frames\":%" PRIu64 ",\"audio_frames\":%" PRIu64 ","
                         "\"audio_samples\":%" PRIu64 ",\"video_uploads\":%" PRIu64 "}\n",
                         snapshot.joined ? "true" : "false", snapshot.epoch, snapshot.peer_count,
                         video, audio, samples, ui->remote_video_uploads);
    int closed = fclose(file);
    return result < 0 || closed != 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    ui_sdl3_t ui;
    if (ui_sdl3_init(&ui, 1120, 760))
        return 1;
    const char *stun = APP_SESSION_DEFAULT_STUN;
    const char *stats_path = NULL;
    uint64_t duration_ms = 0;
    bool auto_join = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--camera"))
            ui.use_camera = true;
        else if (!strcmp(argv[i], "--mic"))
            ui.use_mic = true;
        else if (!strcmp(argv[i], "--synthetic"))
            ui.use_camera = false;
        else if (!strcmp(argv[i], "--tone"))
            ui.use_mic = false;
        else if (!strcmp(argv[i], "--meeting") && i + 1 < argc) {
            snprintf(ui.room, sizeof(ui.room), "%s", argv[++i]);
            auto_join = true;
        } else if (!strcmp(argv[i], "--server") && i + 1 < argc)
            snprintf(ui.server, sizeof(ui.server), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(ui.name, sizeof(ui.name), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--stun") && i + 1 < argc)
            stun = argv[++i];
        else if (!strcmp(argv[i], "--stats-file") && i + 1 < argc)
            stats_path = argv[++i];
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            char *end = NULL;
            long seconds = strtol(argv[++i], &end, 10);
            if (!argv[i][0] || *end || seconds < 1 || seconds > 86400) {
                fprintf(stderr, "--duration must be 1..86400 seconds\n");
                ui_sdl3_close(&ui);
                SDL_Quit();
                return 1;
            }
            duration_ms = (uint64_t)seconds * 1000;
        } else {
            fprintf(stderr,
                    "Usage: %s [--meeting room] [--server ws://host:port] [--name name]"
                    " [--camera|--synthetic] [--mic|--tone] [--stun url]"
                    " [--duration seconds] [--stats-file path]\n"
                    "Labels use an ASCII font; UTF-8 is preserved but non-ASCII glyphs display as "
                    "?.\n",
                    argv[0]);
            ui_sdl3_close(&ui);
            SDL_Quit();
            return !strcmp(argv[i], "--help") ? 0 : 1;
        }
    }
    app_session_config_t config = {
        .callbacks = {.on_remote_video = ui_sdl3_on_video_frame,
                      .on_remote_audio = ui_sdl3_on_audio_samples,
                      .on_peer_joined = ui_sdl3_on_peer_joined,
                      .on_peer_left = ui_sdl3_on_peer_left,
                      .on_topology = ui_sdl3_on_topology,
                      .on_error = ui_sdl3_on_error,
                      .on_joined = joined,
                      .on_message = ui_sdl3_on_message},
        .on_local_video = preview,
        .on_local_audio = ui_sdl3_on_local_audio,
        .user_data = &ui,
    };
    app_session_t *session = app_session_create(&config);
    if (!session) {
        ui_sdl3_close(&ui);
        SDL_Quit();
        return 1;
    }
    bool running = true;
    int exit_code = 0;
    bool preview_camera_selected = false;
    uint64_t last_capture = 0;
    uint64_t started = rtc_time_ms(), last_stats = 0;
    while (running) {
        if (duration_ms && rtc_time_ms() - started >= duration_ms)
            running = false;
        ui_action_t action = auto_join ? UI_ACTION_JOIN : UI_ACTION_NONE;
        auto_join = false;
        SDL_Event event;
        if (SDL_PollEvent(&event)) {
            ui_action_t event_action = ui_sdl3_event(&ui, &event);
            if (event_action != UI_ACTION_NONE)
                action = event_action;
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            if (event.type == SDL_EVENT_CAMERA_DEVICE_DENIED)
                ui_sdl3_on_error("Camera access denied. Select Synthetic or allow access.", &ui);
        }
        switch (action) {
            case UI_ACTION_JOIN: {
                if (ui.in_call)
                    break;
                app_session_options_t options = {
                    .server = ui.server,
                    .room = ui.room,
                    .name = ui.name,
                    .stun = stun,
                    .video = ui.use_camera ? APP_MEDIA_EXTERNAL : APP_MEDIA_SYNTHETIC,
                    .audio = ui.use_mic ? APP_MEDIA_EXTERNAL : APP_MEDIA_SYNTHETIC};
                if (app_session_validate_options(&options) != RTC_OK) {
                    ui_sdl3_on_error("Enter a ws:// or wss:// server, room and name.", &ui);
                    break;
                }
                if (prepare_devices(&ui) != 0)
                    break;
                ui_sdl3_set_status(&ui, "Connecting...");
                if (app_session_start(session, &options) != RTC_OK) {
                    ui_sdl3_close_devices(&ui);
                    ui_sdl3_on_error("Could not join. Check the server address and connection.",
                                     &ui);
                    break;
                }
                ui.in_call = true;
                ui.focused_field = -1;
                ui.mic_muted = ui.cam_muted = false;
                break;
            }
            case UI_ACTION_MUTE:
                ui.mic_muted = !ui.mic_muted;
                app_session_mute(session, "audio", ui.mic_muted);
                break;
            case UI_ACTION_CAMERA:
                ui.cam_muted = !ui.cam_muted;
                app_session_mute(session, "video", ui.cam_muted);
                break;
            case UI_ACTION_CHAT:
                ui.chat_open = !ui.chat_open;
                ui.focused_field = ui.chat_open ? 3 : -1;
                break;
            case UI_ACTION_SEND:
                if (ui.chat_input[0]) {
                    if (app_session_send_message(session, NULL, (uint8_t *)ui.chat_input,
                                                 strlen(ui.chat_input)) == RTC_OK) {
                        ui_sdl3_add_chat(&ui, "You", (uint8_t *)ui.chat_input,
                                         strlen(ui.chat_input));
                        ui.chat_input[0] = 0;
                    } else
                        ui_sdl3_on_error("Message not sent; peer data channel is not ready.", &ui);
                }
                break;
            case UI_ACTION_LEAVE:
                if (!ui.in_call) {
                    running = false;
                    break;
                }
                app_session_stop(session);
                ui_sdl3_close_devices(&ui);
                for (int i = 0; i < ui.tile_count; i++)
                    if (ui.tiles[i].active)
                        ui_sdl3_remove_peer(&ui, ui.tiles[i].peer_id);
                ui.in_call = false;
                preview_camera_selected = false;
                ui.focused_field = 0;
                ui.chat_count = 0;
                ui.chat_input[0] = 0;
                ui_sdl3_set_status(&ui, "Left room. Ready to join again.");
                break;
            default:
                break;
        }
        if (ui.in_call) {
            uint64_t now = rtc_time_ms();
            app_session_tick(session, now);
            if (now - last_capture >= 10) {
                capture(&ui, session);
                last_capture = now;
            }
            app_session_snapshot_t snapshot;
            app_session_get_snapshot(session, &snapshot);
            for (size_t i = 0; i < snapshot.peer_count; i++)
                ui_sdl3_set_peer_name(&ui, snapshot.peers[i].id, snapshot.peers[i].name);
        } else {
            if (ui.use_camera != preview_camera_selected) {
                ui_sdl3_close_devices(&ui);
                if (ui.local_preview)
                    SDL_DestroyTexture(ui.local_preview);
                ui.local_preview = NULL;
                preview_camera_selected = ui.use_camera;
                if (ui.use_camera && ui_sdl3_open_camera(&ui, APP_SESSION_VIDEO_WIDTH,
                                                         APP_SESSION_VIDEO_HEIGHT) != 0)
                    ui_sdl3_on_error("Camera unavailable; select Synthetic or allow access.", &ui);
            }
            if (ui.camera)
                capture(&ui, session);
            else if (!ui.use_camera)
                app_session_preview_tick(session, rtc_time_ms());
        }
        ui_sdl3_render(&ui);
        if (stats_path && rtc_time_ms() - last_stats >= 1000) {
            if (write_stats(stats_path, &ui, session) != 0) {
                exit_code = 1;
                running = false;
            }
            last_stats = rtc_time_ms();
        }
        SDL_Delay(8);
    }
    if (stats_path && write_stats(stats_path, &ui, session) != 0)
        exit_code = 1;
    app_session_destroy(session);
    ui_sdl3_close(&ui);
    SDL_Quit();
    return exit_code;
}
