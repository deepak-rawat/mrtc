/*
 * mrtc Video Conference — SDL3 Native Client
 *
 * Usage:
 *   conf_sdl --meeting <name> [--server ws://host:port] [--stun ip]
 *            [--camera] [--mic]
 *
 * By default, sends animated test pattern video and 440Hz sine tone audio.
 * Use --camera to use the real webcam, --mic to use the real microphone.
 *
 * Prerequisites:
 *   1. Start signaling server:  ./signaling/signaling_server 8080
 *   2. Run this app in two+ terminals with the same meeting name
 *
 * Controls:
 *   M — toggle mute microphone / test tone
 *   V — toggle mute camera / test pattern
 *   Q — quit
 */
#include "ui_sdl3.h"
#include <conference/conference.h>
#include <media/test_pattern.h>
#include <media/test_tone.h>
#include <signaling/signaling_client.h>
#include <rtc/rtc_client.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char *argv[]) {
    const char *meeting = NULL;
    const char *server_url = "ws://127.0.0.1:8080";
    const char *stun_ip = "stun.l.google.com";
    bool use_camera = false;
    bool use_mic = false;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--meeting") == 0 && i + 1 < argc)
            meeting = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server_url = argv[++i];
        else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc)
            stun_ip = argv[++i];
        else if (strcmp(argv[i], "--camera") == 0)
            use_camera = true;
        else if (strcmp(argv[i], "--mic") == 0)
            use_mic = true;
    }

    if (!meeting) {
        printf("Usage: %s --meeting <name> [--server <url>] [--stun <ip>]"
               " [--camera] [--mic]\n\n",
               argv[0]);
        printf("  --meeting <name>   Meeting to join (required)\n");
        printf("  --server <url>     Signaling server (default: ws://127.0.0.1:8080)\n");
        printf("  --stun <ip>        STUN server IP (default: stun.l.google.com)\n");
        printf("  --camera           Use real webcam (default: test pattern)\n");
        printf("  --mic              Use real microphone (default: 440Hz test tone)\n");
        return 1;
    }

    printf("================================================\n");
    printf("  mrtc Video Conference (SDL3)\n");
    printf("================================================\n");
    printf("  Meeting:  %s\n", meeting);
    printf("  Server:   %s\n", server_url);
    printf("  STUN:     %s\n", stun_ip);
    printf("  Video:    %s\n", use_camera ? "webcam" : "test pattern");
    printf("  Audio:    %s\n", use_mic ? "microphone" : "440Hz test tone");
    printf("  Controls: M=mute, V=mute video, Q=quit\n");
    printf("================================================\n\n");

    /* Init mrtc */
    rtc_client_init();

    /* Log to timestamped file + stderr */
    char log_path[128];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(log_path, sizeof(log_path), "mrtc_%Y%m%d_%H%M%S.log", tm);
    rtc_set_log_file(log_path, true);
    printf("  Log file: %s\n", log_path);

    /* Init SDL3 UI */
    ui_sdl3_t ui;
    if (ui_sdl3_init(&ui, 960, 720) != 0) {
        printf("  [FAIL] SDL3 init failed\n");
        return 1;
    }

    /* Video source: camera or test pattern */
    test_pattern_t test_vid;
    bool has_camera = false;

    if (use_camera) {
        if (ui_sdl3_open_camera(&ui, 640, 480) == 0) {
            has_camera = true;
        } else {
            printf("  [WARN] Camera not available — falling back to test pattern\n");
        }
    }
    if (!has_camera) {
        test_pattern_init(&test_vid, 640, 480);
        printf("  [INFO] Using video test pattern (bouncing box)\n");
    }

    /* Audio source: mic or test tone */
    test_tone_t test_audio;
    bool has_mic = false;

    if (use_mic) {
        if (ui_sdl3_open_audio(&ui, 48000, 1) == 0)
            has_mic = true;
        else
            printf("  [WARN] Microphone not available — falling back to test tone\n");
    }
    if (!has_mic) {
        test_tone_init(&test_audio, 48000, 1, 440);
        printf("  [INFO] Using 440Hz audio test tone\n");
    }

    /* Always open speaker for playback */
    if (!has_mic) {
        SDL_AudioSpec spk_spec = {.format = SDL_AUDIO_S16, .channels = 1, .freq = 48000};
        ui.speaker_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spk_spec, NULL, NULL);
        if (ui.speaker_stream)
            SDL_ResumeAudioStreamDevice(ui.speaker_stream);
    }

    /* Create conference */
    bool mic_muted = false, cam_muted = false;
    conference_config_t conf_cfg = {
        .video_codec = "VP8",
        .audio_codec = "opus",
        .video_width = 640,
        .video_height = 480,
        .video_fps = 30,
        .video_bitrate_kbps = 500,
        .audio_sample_rate = 48000,
        .audio_channels = 1,
        .audio_bitrate_bps = 32000,
        .stun_server = stun_ip,
        .callbacks =
            {
                .on_remote_video = ui_sdl3_on_video_frame,
                .on_remote_audio = ui_sdl3_on_audio_samples,
                .on_peer_joined = NULL,
                .on_peer_left = NULL,
                .on_joined = NULL,
            },
        .user_data = &ui,
    };
    conference_t *conf = conference_create(&conf_cfg);
    if (!conf) {
        printf("  [FAIL] Conference create failed\n");
        ui_sdl3_close(&ui);
        return 1;
    }

    /* Add media sources */
    conference_add_video_source(conf, "camera", 640, 480, 30, 500);
    conference_add_audio_source(conf, "mic", 48000, 1, 32000);

    /* Create signaling client wired to conference */
    signaling_config_t sig_cfg;
    conference_get_signaling_callbacks(conf, &sig_cfg, server_url, meeting);
    signaling_client_t *sig = signaling_create(&sig_cfg);
    if (!sig) {
        printf("  [FAIL] Signaling create failed\n");
        conference_destroy(conf);
        ui_sdl3_close(&ui);
        return 1;
    }

    if (signaling_connect(sig) != RTC_OK) {
        printf("  [FAIL] Could not connect. Is the signaling server running?\n");
        printf("    Start it: ./signaling/signaling_server 8080\n");
        signaling_destroy(sig);
        conference_destroy(conf);
        ui_sdl3_close(&ui);
        return 1;
    }

    conference_join(conf, sig);
    printf("  Connected! Waiting for peers...\n\n");

    /* ---- Main loop ---- */
    bool running = true;
    int16_t pcm_buf[960];
    uint64_t last_video_ms = 0;
    uint64_t last_audio_ms = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                    case SDLK_Q:
                        running = false;
                        break;
                    case SDLK_M:
                        mic_muted = !mic_muted;
                        conference_mute_source(conf, "mic", mic_muted);
                        printf("  [mic %s]\n", mic_muted ? "muted" : "unmuted");
                        break;
                    case SDLK_V:
                        cam_muted = !cam_muted;
                        conference_mute_source(conf, "camera", cam_muted);
                        printf("  [camera %s]\n", cam_muted ? "muted" : "unmuted");
                        break;
                    default:
                        break;
                }
            }
        }

        uint64_t now = rtc_time_ms();

        /* Video source → conference (~30fps) */
        if (now - last_video_ms >= 33) {
            last_video_ms = now;
            if (has_camera) {
                SDL_Surface *cam = ui_sdl3_acquire_camera_frame(&ui);
                if (cam) {
                    video_frame_t vf;
                    if (ui_sdl3_surface_to_frame(cam, &vf) == 0) {
                        ui_sdl3_update_preview(&ui, &vf);
                        conference_push_video(conf, "camera", &vf);
                    }
                    ui_sdl3_release_camera_frame(&ui, cam);
                }
            } else {
                test_pattern_next_frame(&test_vid);
                const video_frame_t *vf = test_pattern_get_frame(&test_vid);
                ui_sdl3_update_preview(&ui, vf);
                conference_push_video(conf, "camera", vf);
            }
        }

        /* Audio source → conference (~20ms) */
        if (now - last_audio_ms >= 20) {
            last_audio_ms = now;
            if (has_mic && ui.mic_stream) {
                int frame_bytes = 960 * (int)sizeof(int16_t);
                if (SDL_GetAudioStreamAvailable(ui.mic_stream) >= frame_bytes) {
                    SDL_GetAudioStreamData(ui.mic_stream, pcm_buf, frame_bytes);
                    audio_frame_t af = {.samples = pcm_buf,
                                        .sample_count = 960,
                                        .sample_rate = 48000,
                                        .channels = 1};
                    conference_push_audio(conf, "mic", &af);
                }
            } else {
                audio_frame_t af;
                test_tone_next_frame(&test_audio, &af);
                conference_push_audio(conf, "mic", &af);
            }
        }

        /* Render */
        ui_sdl3_render(&ui);
        SDL_Delay(1);
    }

    /* Cleanup */
    printf("\n  Leaving meeting...\n");
    conference_leave(conf);
    signaling_destroy(sig);
    conference_destroy(conf);
    if (!has_camera)
        test_pattern_close(&test_vid);
    if (!has_mic)
        test_tone_close(&test_audio);
    ui_sdl3_close(&ui);
    SDL_Quit();
    rtc_log_close();
    rtc_client_cleanup();

    printf("  Bye!\n");
    return 0;
}
