#include <conference/conference.h>
#include <media/test_pattern.h>
#include <media/test_tone.h>
#include <rtc/rtc_client.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define PROBE_SLEEP() Sleep(10)
#else
#  include <unistd.h>
#  define PROBE_SLEEP() usleep(10000)
#endif

typedef struct {
    conference_t *conference;
    _Atomic unsigned video;
    _Atomic unsigned audio;
    _Atomic unsigned errors;
    _Atomic bool reply;
    _Atomic bool received_reply;
} probe_t;

static volatile sig_atomic_t stopped;

static void stop_probe(int signal_number) {
    (void)signal_number;
    stopped = 1;
}

static void on_video(const char *peer, const char *label, const video_frame_t *frame, void *user) {
    (void)peer;
    (void)label;
    (void)frame;
    probe_t *probe = user;
    atomic_fetch_add(&probe->video, 1);
}

static void on_audio(const char *peer, const char *label, const audio_frame_t *frame, void *user) {
    (void)peer;
    (void)label;
    (void)frame;
    probe_t *probe = user;
    atomic_fetch_add(&probe->audio, 1);
}

static void on_message(const char *peer, const uint8_t *data, size_t len, void *user) {
    (void)peer;
    probe_t *probe = user;
    if (len == strlen("browser-datachannel") && memcmp(data, "browser-datachannel", len) == 0) {
        atomic_store(&probe->reply, true);
        puts("PROBE_DATA_OK");
        fflush(stdout);
    } else if (len == strlen("native-datachannel") &&
               memcmp(data, "native-datachannel", len) == 0) {
        atomic_store(&probe->received_reply, true);
    }
}

static void on_joined(const char *peer, void *user) {
    (void)user;
    printf("PROBE_JOINED %s\n", peer);
    fflush(stdout);
}

static void on_error(const char *message, void *user) {
    probe_t *probe = user;
    fprintf(stderr, "PROBE_ERROR %s\n", message);
    atomic_fetch_add(&probe->errors, 1);
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "ws://127.0.0.1:8080";
    const char *room = argc > 2 ? argv[2] : "interop";
    unsigned duration = argc > 3 ? (unsigned)atoi(argv[3]) : 45;
    unsigned cycles = argc > 4 ? (unsigned)atoi(argv[4]) : 1;
    if (!duration || !cycles)
        return 1;
    signal(SIGTERM, stop_probe);
    signal(SIGINT, stop_probe);
    if (rtc_client_init() != RTC_OK)
        return 1;
    probe_t probe = {0};
    conference_config_t config = {
        .callbacks = {.on_remote_video = on_video,
                      .on_remote_audio = on_audio,
                      .on_message = on_message,
                      .on_joined = on_joined,
                      .on_error = on_error},
        .user_data = &probe,
    };
    conference_t *conference = conference_create(&config);
    if (!conference)
        return 1;
    probe.conference = conference;
    test_pattern_t pattern;
    test_tone_t tone;
    if (test_pattern_init(&pattern, 160, 120) != RTC_OK ||
        test_tone_init(&tone, 48000, 1, 440) != RTC_OK ||
        conference_add_video_source(conference, "camera", 160, 120, 10, 160) != RTC_OK ||
        conference_add_audio_source(conference, "mic", 48000, 1, 32000) != RTC_OK)
        return 1;
    signaling_config_t signaling_config;
    conference_get_signaling_callbacks(conference, &signaling_config, url, room);
    bool media_ok = false, data_ok = false;
    unsigned completed = 0;
    for (unsigned cycle = 0; cycle < cycles && !stopped; cycle++) {
        unsigned initial_video = atomic_load(&probe.video);
        unsigned initial_audio = atomic_load(&probe.audio);
        atomic_store(&probe.received_reply, false);
        media_ok = data_ok = false;
        signaling_client_t *signaling = signaling_create(&signaling_config);
        if (!signaling)
            break;
        conference_join(conference, signaling);
        if (signaling_connect(signaling) != RTC_OK) {
            conference_leave(conference);
            signaling_destroy(signaling);
            break;
        }
        uint64_t deadline = rtc_time_ms() + duration * 1000;
        uint64_t last_video = 0, last_audio = 0;
        while (!stopped && rtc_time_ms() < deadline) {
            uint64_t now = rtc_time_ms();
            if (now - last_video >= 100) {
                last_video = now;
                test_pattern_next_frame(&pattern);
                conference_push_video(conference, "camera", test_pattern_get_frame(&pattern));
            }
            if (now - last_audio >= 20) {
                last_audio = now;
                audio_frame_t audio;
                test_tone_next_frame(&tone, &audio);
                conference_push_audio(conference, "mic", &audio);
            }
            if (atomic_load(&probe.reply)) {
                static const uint8_t reply[] = "native-datachannel";
                if (conference_broadcast_message(conference, reply, sizeof(reply) - 1) == RTC_OK) {
                    atomic_store(&probe.reply, false);
                    data_ok = true;
                }
            }
            if (cycles > 1 && !atomic_load(&probe.received_reply)) {
                static const uint8_t request[] = "browser-datachannel";
                conference_broadcast_message(conference, request, sizeof(request) - 1);
            }
            if (!media_ok && atomic_load(&probe.video) > initial_video + 2 &&
                atomic_load(&probe.audio) > initial_audio) {
                media_ok = true;
                puts("PROBE_MEDIA_OK");
                fflush(stdout);
            }
            if (cycles > 1 && media_ok && atomic_load(&probe.received_reply)) {
                data_ok = true;
                break;
            }
            PROBE_SLEEP();
        }
        conference_leave(conference);
        signaling_destroy(signaling);
        if (!media_ok || !data_ok)
            break;
        completed++;
    }
    conference_destroy(conference);
    test_pattern_close(&pattern);
    test_tone_close(&tone);
    rtc_client_cleanup();
    printf("PROBE_DONE video=%u audio=%u errors=%u\n", atomic_load(&probe.video),
           atomic_load(&probe.audio), atomic_load(&probe.errors));
    if (cycles > 1 && completed == cycles)
        printf("PROBE_CHURN_OK cycles=%u\n", completed);
    return media_ok && data_ok && completed == cycles && !atomic_load(&probe.errors) ? 0 : 1;
}
