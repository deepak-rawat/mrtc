#include "app_session.h"
#include <media/test_pattern.h>
#include <media/test_tone.h>
#include <rtc/rtc_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app_session {
    app_session_config_t config;
    rtc_mutex_t mutex;
    app_session_snapshot_t snapshot;
    conference_t *conference;
    signaling_client_t *signaling;
    signaling_config_t signaling_callbacks;
    app_media_mode_t video, audio;
    test_pattern_t pattern;
    test_tone_t tone;
    bool pattern_ready, tone_ready, runtime_ready;
    uint64_t next_video, next_audio;
    char server[APP_SESSION_SERVER_SIZE], room[SIG_MAX_MEETING];
    char name[SIG_MAX_DISPLAY_NAME], stun[256];
};

static void copy_text(char *dst, size_t size, const char *src) {
    snprintf(dst, size, "%s", src ? src : "");
}

static app_session_peer_t *peer_locked(app_session_t *s, const char *id) {
    for (size_t i = 0; i < s->snapshot.peer_count; ++i)
        if (!strcmp(s->snapshot.peers[i].id, id))
            return &s->snapshot.peers[i];
    if (s->snapshot.peer_count == SIG_MAX_PEERS)
        return NULL;
    app_session_peer_t *p = &s->snapshot.peers[s->snapshot.peer_count++];
    memset(p, 0, sizeof(*p));
    copy_text(p->id, sizeof(p->id), id);
    copy_text(p->name, sizeof(p->name), id);
    return p;
}

static void joined(const char *id, void *user) {
    app_session_t *s = user;
    rtc_mutex_lock(&s->mutex);
    s->snapshot.joined = true;
    copy_text(s->snapshot.local_id, sizeof(s->snapshot.local_id), id);
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), "joined");
    rtc_mutex_unlock(&s->mutex);
    if (s->config.callbacks.on_joined)
        s->config.callbacks.on_joined(id, s->config.user_data);
}

static void peer_joined(const char *id, void *user) {
    app_session_t *s = user;
    char name[SIG_MAX_DISPLAY_NAME] = "";
    conference_get_peer_name(s->conference, id, name, sizeof(name));
    rtc_mutex_lock(&s->mutex);
    app_session_peer_t *p = peer_locked(s, id);
    if (p && name[0])
        copy_text(p->name, sizeof(p->name), name);
    rtc_mutex_unlock(&s->mutex);
    if (s->config.callbacks.on_peer_joined)
        s->config.callbacks.on_peer_joined(id, s->config.user_data);
}

static void peer_left(const char *id, void *user) {
    app_session_t *s = user;
    rtc_mutex_lock(&s->mutex);
    for (size_t i = 0; i < s->snapshot.peer_count; ++i) {
        if (!strcmp(s->snapshot.peers[i].id, id)) {
            memmove(&s->snapshot.peers[i], &s->snapshot.peers[i + 1],
                    (s->snapshot.peer_count - i - 1) * sizeof(s->snapshot.peers[0]));
            --s->snapshot.peer_count;
            break;
        }
    }
    for (size_t i = 0; i < s->snapshot.source_count;) {
        if (!strcmp(s->snapshot.sources[i].peer_id, id)) {
            memmove(&s->snapshot.sources[i], &s->snapshot.sources[i + 1],
                    (s->snapshot.source_count - i - 1) * sizeof(s->snapshot.sources[0]));
            --s->snapshot.source_count;
        } else {
            ++i;
        }
    }
    rtc_mutex_unlock(&s->mutex);
    if (s->config.callbacks.on_peer_left)
        s->config.callbacks.on_peer_left(id, s->config.user_data);
}

static void connected(const char *id, void *user) {
    app_session_t *s = user;
    rtc_mutex_lock(&s->mutex);
    app_session_peer_t *p = peer_locked(s, id);
    if (p)
        p->connected = true;
    rtc_mutex_unlock(&s->mutex);
    if (s->config.callbacks.on_connected)
        s->config.callbacks.on_connected(id, s->config.user_data);
}

static void count_frame(app_session_t *s, const char *id, const char *label, bool video,
                        uint64_t samples) {
    rtc_mutex_lock(&s->mutex);
    app_session_peer_t *p = peer_locked(s, id);
    if (p) {
        p->video_frames += video;
        p->audio_frames += !video;
        p->audio_samples += samples;
    }
    app_session_source_t *source = NULL;
    for (size_t i = 0; i < s->snapshot.source_count; ++i) {
        app_session_source_t *candidate = &s->snapshot.sources[i];
        if (!strcmp(candidate->peer_id, id) && !strcmp(candidate->label, label)) {
            source = candidate;
            break;
        }
    }
    if (!source && s->snapshot.source_count < APP_SESSION_MAX_SOURCES) {
        source = &s->snapshot.sources[s->snapshot.source_count++];
        memset(source, 0, sizeof(*source));
        copy_text(source->peer_id, sizeof(source->peer_id), id);
        copy_text(source->label, sizeof(source->label), label);
    }
    if (source) {
        source->video_frames += video;
        source->audio_frames += !video;
        source->audio_samples += samples;
    }
    rtc_mutex_unlock(&s->mutex);
}

static void video(const char *id, const char *label, const video_frame_t *frame, void *user) {
    app_session_t *s = user;
    count_frame(s, id, label, true, 0);
    if (s->config.callbacks.on_remote_video)
        s->config.callbacks.on_remote_video(id, label, frame, s->config.user_data);
}

static void audio(const char *id, const char *label, const audio_frame_t *frame, void *user) {
    app_session_t *s = user;
    count_frame(s, id, label, false, (uint64_t)frame->sample_count);
    if (s->config.callbacks.on_remote_audio)
        s->config.callbacks.on_remote_audio(id, label, frame, s->config.user_data);
}

static void message(const char *id, const uint8_t *data, size_t len, void *user) {
    app_session_t *s = user;
    if (s->config.callbacks.on_message)
        s->config.callbacks.on_message(id, data, len, s->config.user_data);
}

static void error(const char *text, void *user) {
    app_session_t *s = user;
    rtc_mutex_lock(&s->mutex);
    copy_text(s->snapshot.error, sizeof(s->snapshot.error), text);
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), "error");
    rtc_mutex_unlock(&s->mutex);
    if (s->config.callbacks.on_error)
        s->config.callbacks.on_error(text, s->config.user_data);
}

static void topology(const char *mode, const char *status, uint32_t epoch, const char *reason,
                     void *user) {
    app_session_t *s = user;
    rtc_mutex_lock(&s->mutex);
    if (!strcmp(status, "active") && epoch != s->snapshot.epoch) {
        s->snapshot.epoch = epoch;
        s->snapshot.source_count = 0;
        for (size_t i = 0; i < s->snapshot.peer_count; ++i) {
            s->snapshot.peers[i].video_frames = 0;
            s->snapshot.peers[i].audio_frames = 0;
            s->snapshot.peers[i].audio_samples = 0;
        }
    }
    if (!strcmp(status, "active"))
        copy_text(s->snapshot.topology, sizeof(s->snapshot.topology), mode);
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), status);
    rtc_mutex_unlock(&s->mutex);
    if (s->config.callbacks.on_topology)
        s->config.callbacks.on_topology(mode, status, epoch, reason, s->config.user_data);
}

static void signaling_event(const sig_msg_t *event, void *user) {
    app_session_t *s = user;
    if (event->type == SIG_MSG_JOINED && event->version == SIG_PROTOCOL_VERSION && event->epoch &&
        (!strcmp(event->mode, "mesh") || !strcmp(event->mode, "sfu"))) {
        rtc_mutex_lock(&s->mutex);
        s->snapshot.epoch = event->epoch;
        copy_text(s->snapshot.topology, sizeof(s->snapshot.topology), event->mode);
        rtc_mutex_unlock(&s->mutex);
    }
    s->signaling_callbacks.on_event(event, s->signaling_callbacks.user_data);
}

static void signaling_error(const char *text, void *user) {
    app_session_t *s = user;
    if (s->signaling_callbacks.on_error)
        s->signaling_callbacks.on_error(text, s->signaling_callbacks.user_data);
}

static void signaling_disconnected(void *user) {
    app_session_t *s = user;
    rtc_mutex_lock(&s->mutex);
    s->snapshot.joined = false;
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), "disconnected");
    rtc_mutex_unlock(&s->mutex);
    if (s->signaling_callbacks.on_disconnected)
        s->signaling_callbacks.on_disconnected(s->signaling_callbacks.user_data);
}

app_session_t *app_session_create(const app_session_config_t *config) {
    app_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (rtc_mutex_init(&s->mutex) != RTC_OK) {
        free(s);
        return NULL;
    }
    if (config)
        s->config = *config;
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), "idle");
    return s;
}

int app_session_validate_options(const app_session_options_t *o) {
    if (!o || !o->server || !o->room || !o->room[0] ||
        strlen(o->server) >= APP_SESSION_SERVER_SIZE ||
        (strncmp(o->server, "ws://", 5) && strncmp(o->server, "wss://", 6)) ||
        strlen(o->room) >= SIG_MAX_MEETING ||
        (o->name && strlen(o->name) >= SIG_MAX_DISPLAY_NAME) ||
        (o->stun && strlen(o->stun) >= 256) || o->video < APP_MEDIA_NONE ||
        o->video > APP_MEDIA_EXTERNAL || o->audio < APP_MEDIA_NONE ||
        o->audio > APP_MEDIA_EXTERNAL ||
        (o->data_only && (o->video != APP_MEDIA_NONE || o->audio != APP_MEDIA_NONE)))
        return RTC_ERR_INVALID;
    const char *host = o->server + (!strncmp(o->server, "wss://", 6) ? 6 : 5);
    if (!host[0] || host[0] == '/' || host[0] == ':' || strpbrk(o->server, "\r\n\t "))
        return RTC_ERR_INVALID;
    const char *path = strchr(host, '/');
    const char *authority_end = path ? path : host + strlen(host);
    const char *colon = memchr(host, ':', (size_t)(authority_end - host));
    const char *host_end = colon ? colon : authority_end;
    if ((size_t)(host_end - host) >= 256 || memchr(host, '@', (size_t)(host_end - host)) ||
        (path && strlen(path) >= 256))
        return RTC_ERR_INVALID;
    if (colon) {
        unsigned port = 0;
        if (colon + 1 == authority_end)
            return RTC_ERR_INVALID;
        for (const char *digit = colon + 1; digit < authority_end; ++digit) {
            if (*digit < '0' || *digit > '9' || port > 6553)
                return RTC_ERR_INVALID;
            port = port * 10 + (unsigned)(*digit - '0');
        }
        if (!port || port > 65535)
            return RTC_ERR_INVALID;
    }
    return RTC_OK;
}

int app_session_start(app_session_t *s, const app_session_options_t *o) {
    if (!s || s->conference || app_session_validate_options(o) != RTC_OK)
        return RTC_ERR_INVALID;
    copy_text(s->server, sizeof(s->server), o->server);
    copy_text(s->room, sizeof(s->room), o->room);
    copy_text(s->name, sizeof(s->name), o->name);
    copy_text(s->stun, sizeof(s->stun), o->stun ? o->stun : APP_SESSION_DEFAULT_STUN);
    s->video = o->video;
    s->audio = o->audio;
    rtc_mutex_lock(&s->mutex);
    uint64_t generation = s->snapshot.generation + 1;
    memset(&s->snapshot, 0, sizeof(s->snapshot));
    s->snapshot.generation = generation;
    copy_text(s->snapshot.name, sizeof(s->snapshot.name), s->name);
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), "connecting");
    rtc_mutex_unlock(&s->mutex);
    int rc = rtc_client_init();
    if (rc != RTC_OK)
        goto failed;
    s->runtime_ready = true;
    conference_config_t config = {
        .stun_server = s->stun,
        .display_name = s->name,
        .video_width = APP_SESSION_VIDEO_WIDTH,
        .video_height = APP_SESSION_VIDEO_HEIGHT,
        .video_fps = APP_SESSION_VIDEO_FPS,
        .disable_receive_video = o->data_only,
        .disable_receive_audio = o->data_only,
        .callbacks = {.on_joined = joined,
                      .on_peer_joined = peer_joined,
                      .on_peer_left = peer_left,
                      .on_connected = connected,
                      .on_remote_video = video,
                      .on_remote_audio = audio,
                      .on_message = message,
                      .on_error = error,
                      .on_topology = topology},
        .user_data = s,
    };
    s->conference = conference_create(&config);
    if (!s->conference) {
        rc = RTC_ERR_NOMEM;
        goto failed;
    }
    if (s->video != APP_MEDIA_NONE &&
        (rc = conference_add_video_source(s->conference, "camera", APP_SESSION_VIDEO_WIDTH,
                                          APP_SESSION_VIDEO_HEIGHT, APP_SESSION_VIDEO_FPS, 350)) !=
            RTC_OK)
        goto failed;
    if (s->audio != APP_MEDIA_NONE &&
        (rc = conference_add_audio_source(s->conference, "mic", 48000, 1, 32000)) != RTC_OK)
        goto failed;
    if (s->video == APP_MEDIA_SYNTHETIC && !s->pattern_ready) {
        rc = test_pattern_init(&s->pattern, APP_SESSION_VIDEO_WIDTH, APP_SESSION_VIDEO_HEIGHT);
        if (rc != RTC_OK)
            goto failed;
        s->pattern_ready = true;
    }
    if (s->audio == APP_MEDIA_SYNTHETIC) {
        rc = test_tone_init(&s->tone, 48000, 1, 440);
        if (rc != RTC_OK)
            goto failed;
        s->tone_ready = true;
    }
    signaling_config_t signaling_config;
    conference_get_signaling_callbacks(s->conference, &signaling_config, s->server, s->room);
    s->signaling_callbacks = signaling_config;
    signaling_config.on_event = signaling_event;
    signaling_config.on_error = signaling_error;
    signaling_config.on_disconnected = signaling_disconnected;
    signaling_config.user_data = s;
    s->signaling = signaling_create(&signaling_config);
    if (!s->signaling) {
        rc = RTC_ERR_NOMEM;
        goto failed;
    }
    rc = conference_join(s->conference, s->signaling);
    if (rc != RTC_OK)
        goto failed;
    rtc_mutex_lock(&s->mutex);
    s->snapshot.active = true;
    rtc_mutex_unlock(&s->mutex);
    rc = signaling_connect(s->signaling);
    if (rc != RTC_OK)
        goto failed;
    s->next_video = s->next_audio = 0;
    return RTC_OK;
failed:
    app_session_stop(s);
    return rc;
}

void app_session_tick(app_session_t *s, uint64_t now) {
    if (!s || !s->conference)
        return;
    if (s->video == APP_MEDIA_SYNTHETIC && s->pattern_ready && now >= s->next_video) {
        test_pattern_next_frame(&s->pattern);
        if (s->config.on_local_video)
            s->config.on_local_video(test_pattern_get_frame(&s->pattern), s->config.user_data);
        conference_push_video(s->conference, "camera", test_pattern_get_frame(&s->pattern));
        s->next_video = now + 1000 / APP_SESSION_VIDEO_FPS;
    }
    if (s->tone_ready && now >= s->next_audio) {
        audio_frame_t frame;
        test_tone_next_frame(&s->tone, &frame);
        if (s->config.on_local_audio)
            s->config.on_local_audio(&frame, s->config.user_data);
        conference_push_audio(s->conference, "mic", &frame);
        s->next_audio = now + 20;
    }
}

void app_session_preview_tick(app_session_t *s, uint64_t now) {
    if (!s || s->conference)
        return;
    if (!s->pattern_ready) {
        if (test_pattern_init(&s->pattern, APP_SESSION_VIDEO_WIDTH, APP_SESSION_VIDEO_HEIGHT) !=
            RTC_OK)
            return;
        s->pattern_ready = true;
    }
    if (now >= s->next_video) {
        test_pattern_next_frame(&s->pattern);
        if (s->config.on_local_video)
            s->config.on_local_video(test_pattern_get_frame(&s->pattern), s->config.user_data);
        s->next_video = now + 1000 / APP_SESSION_VIDEO_FPS;
    }
}
void app_session_stop(app_session_t *s) {
    if (!s)
        return;
    if (s->conference)
        conference_leave(s->conference);
    if (s->signaling) {
        signaling_destroy(s->signaling);
        s->signaling = NULL;
    }
    if (s->conference) {
        conference_destroy(s->conference);
        s->conference = NULL;
    }
    if (s->tone_ready) {
        test_tone_close(&s->tone);
        s->tone_ready = false;
    }
    if (s->pattern_ready) {
        test_pattern_close(&s->pattern);
        s->pattern_ready = false;
    }
    if (s->runtime_ready) {
        rtc_client_cleanup();
        s->runtime_ready = false;
    }
    rtc_mutex_lock(&s->mutex);
    s->snapshot.active = false;
    s->snapshot.joined = false;
    copy_text(s->snapshot.status, sizeof(s->snapshot.status), "idle");
    rtc_mutex_unlock(&s->mutex);
}

void app_session_destroy(app_session_t *s) {
    if (!s)
        return;
    app_session_stop(s);
    rtc_mutex_destroy(&s->mutex);
    free(s);
}

int app_session_send_message(app_session_t *s, const char *id, const uint8_t *data, size_t len) {
    if (!s || !s->conference || !data || !len || len > 4096 ||
        (id && (!id[0] || strlen(id) >= SIG_MAX_PEER_ID)))
        return RTC_ERR_INVALID;
    return id ? conference_send_message(s->conference, id, data, len)
              : conference_broadcast_message(s->conference, data, len);
}

int app_session_mute(app_session_t *s, const char *source, bool muted) {
    if (!s || !s->conference || !source)
        return RTC_ERR_INVALID;
    bool is_video = !strcmp(source, "video");
    if ((!is_video && strcmp(source, "audio")) ||
        (is_video ? s->video : s->audio) == APP_MEDIA_NONE)
        return RTC_ERR_INVALID;
    conference_mute_source(s->conference, is_video ? "camera" : "mic", muted);
    rtc_mutex_lock(&s->mutex);
    if (is_video)
        s->snapshot.video_muted = muted;
    else
        s->snapshot.audio_muted = muted;
    rtc_mutex_unlock(&s->mutex);
    return RTC_OK;
}

void app_session_get_snapshot(app_session_t *s, app_session_snapshot_t *out) {
    if (!s || !out)
        return;
    rtc_mutex_lock(&s->mutex);
    *out = s->snapshot;
    rtc_mutex_unlock(&s->mutex);
}

conference_t *app_session_conference(app_session_t *s) {
    return s ? s->conference : NULL;
}
