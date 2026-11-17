/*
 * Conference library implementation.
 *
 * Manages peer connections, media encode/decode, signaling wiring, data channels.
 * Internally uses rtc (peer connection), media (codecs + pipeline), and signaling.
 */
#include "conference/conference.h"
#include <rtc/rtc_client.h>
#include <rtc/rtc_sdp.h>
#include <rtc/rtc_str_map.h>
#include <media/media_pipeline.h>
#include <signaling/signaling_client.h>
#include <signaling/signaling_msg.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

#define CONF_MAX_PEERS   16 /* Seven mesh sessions plus eight SFU sessions, and one spare. */
#define CONF_MAX_SOURCES 4
#define CONF_LABEL_SIZE  64

/* Per-source metadata; encoding is handled by media_pipeline. */
typedef struct {
    char label[CONF_LABEL_SIZE];
    bool is_video;
    bool muted;
    bool active;
    media_send_stream_t *stream; /* pipeline handle */
} conf_source_t;

typedef struct {
    char peer_id[CONF_LABEL_SIZE];
    char session_id[SIG_MAX_SESSION_ID];
    uint32_t epoch;
    sig_session_role_t role;
    bool sfu;
    rtc_peer_connection_t *pc;
    media_pipeline_t *receive_pipeline;
    _Atomic bool connected;
    _Atomic uint64_t video_frames;
    _Atomic uint64_t audio_frames;
    _Atomic uint64_t last_video_ms;
    _Atomic uint64_t last_audio_ms;
    media_recv_stream_t *recv_video;
    media_recv_stream_t *recv_audio;
    rtc_rtp_sender_t *video_sender;
    rtc_rtp_sender_t *audio_sender;
    rtc_rtp_receiver_t *video_receiver;
    _Atomic(rtc_data_channel_t *) dc;
    _Atomic bool dc_open;
    unsigned in_flight_sends;
    bool retiring;
    _Atomic bool active;
    _Atomic bool connected_pending;
    bool description_sent;
    rtc_ice_candidate_desc_t pending_candidates[ICE_MAX_CANDIDATES];
    int pending_candidate_count;
    bool gathering_done;
    rtc_mutex_t candidate_mutex;
    conference_t *conf; /* back-pointer for callbacks */
} conf_peer_t;

typedef struct {
    char id[SIG_MAX_PEER_ID];
    char name[SIG_MAX_DISPLAY_NAME];
    conference_source_stats_t stats;
    uint64_t recent_ids[64];
    unsigned recent_next;
    bool connected_notified;
} conf_participant_t;

struct conference {
    conference_config_t cfg;
    _Atomic(signaling_client_t *) signaling;
    char my_id[CONF_LABEL_SIZE];

    conf_source_t sources[CONF_MAX_SOURCES];
    int source_count;
    rtc_str_map_t source_index; /* label → conf_source_t * (borrowed keys) */

    conf_peer_t peers[CONF_MAX_PEERS];
    int peer_count;
    rtc_str_map_t peer_index; /* peer_id → conf_peer_t * (borrowed keys) */

    media_pipeline_t *pipeline; /* shared: encode+fanout (send) + decode (recv) */
    rtc_mutex_t send_mutex;
    rtc_mutex_t pipeline_mutex;
    rtc_cond_t send_cond;
    _Atomic bool sfu_mode;
    _Atomic bool leaving;
    _Atomic bool v2;
    _Atomic uint32_t active_epoch;
    uint32_t highest_epoch;
    uint32_t pending_epoch;
    bool pending_sfu;
    bool ready_sent;
    bool fallback_requested;
    conf_participant_t participants[SIG_MAX_PEERS];
    int participant_count;
    uint64_t next_message_id;
    rtc_mutex_t render_mutex;
    rtc_thread_t topology_thread;
    _Atomic bool stop_topology;
};

static void check_topology_ready(conference_t *c);
static int send_session(conf_peer_t *p, sig_msg_type_t type, const char *text);

static void report_error(conference_t *c, const char *message) {
    RTC_LOG_ERR("Conference: %s", message);
    if (c->cfg.callbacks.on_error)
        c->cfg.callbacks.on_error(message, c->cfg.user_data);
}

static conf_source_t *find_source(conference_t *c, const char *label) {
    return (conf_source_t *)rtc_str_map_get(&c->source_index, label);
}

static conf_peer_t *find_peer(conference_t *c, const char *peer_id) {
    if (!c->v2)
        return (conf_peer_t *)rtc_str_map_get(&c->peer_index, peer_id);
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active && c->peers[i].epoch == c->active_epoch &&
            strcmp(c->peers[i].peer_id, peer_id) == 0)
            return &c->peers[i];
    return NULL;
}

static conf_peer_t *alloc_session(conference_t *c, const char *peer_id, const char *session_id,
                                  uint32_t epoch, sig_session_role_t role, bool sfu) {
    rtc_mutex_lock(&c->send_mutex);
    /* Check for duplicate */
    conf_peer_t *existing = rtc_str_map_get(&c->peer_index, session_id);
    if (existing) {
        rtc_mutex_unlock(&c->send_mutex);
        return existing;
    }
    if (c->peer_count >= CONF_MAX_PEERS) {
        rtc_mutex_unlock(&c->send_mutex);
        return NULL;
    }
    conf_peer_t *p = NULL;
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        if (!c->peers[i].active && !c->peers[i].retiring) {
            p = &c->peers[i];
            break;
        }
    }
    if (!p) {
        rtc_mutex_unlock(&c->send_mutex);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    if (rtc_mutex_init(&p->candidate_mutex) != RTC_OK) {
        rtc_mutex_unlock(&c->send_mutex);
        return NULL;
    }
    snprintf(p->peer_id, sizeof(p->peer_id), "%s", peer_id);
    snprintf(p->session_id, sizeof(p->session_id), "%s", session_id);
    p->epoch = epoch;
    p->role = role;
    p->sfu = sfu;
    p->active = true;
    p->conf = c;
    if (rtc_str_map_set(&c->peer_index, p->session_id, p) != RTC_OK) {
        p->active = false;
        rtc_mutex_destroy(&p->candidate_mutex);
        rtc_mutex_unlock(&c->send_mutex);
        return NULL;
    }
    c->peer_count++;
    rtc_mutex_unlock(&c->send_mutex);
    return p;
}

static conf_peer_t *alloc_peer(conference_t *c, const char *peer_id) {
    return alloc_session(c, peer_id, peer_id, c->active_epoch, SIG_SESSION_MESH, c->sfu_mode);
}

static void destroy_peer(conference_t *c, conf_peer_t *p) {
    /* Unregister from pipeline fan-out */
    rtc_mutex_lock(&c->send_mutex);
    if (!p->active) {
        rtc_mutex_unlock(&c->send_mutex);
        return;
    }
    rtc_str_map_remove(&c->peer_index, p->session_id);
    p->active = false;
    p->retiring = true;
    p->dc_open = false;
    while (p->in_flight_sends)
        rtc_cond_wait_timeout(&c->send_cond, &c->send_mutex, 1000);
    rtc_mutex_unlock(&c->send_mutex);
    rtc_mutex_lock(&c->pipeline_mutex);
    if (c->pipeline)
        media_pipeline_remove_send_peer(c->pipeline, p->session_id);
    rtc_mutex_unlock(&c->pipeline_mutex);
    if (p->pc) {
        rtc_peer_connection_close(p->pc);
        rtc_peer_connection_destroy(p->pc);
    }
    /* Closing the connection first drains in-flight receive callbacks. */
    if (p->receive_pipeline) {
        media_pipeline_destroy(p->receive_pipeline);
        p->receive_pipeline = NULL;
    }
    rtc_mutex_destroy(&p->candidate_mutex);
    rtc_mutex_lock(&c->send_mutex);
    p->pc = NULL;
    p->dc = NULL;
    p->retiring = false;
    c->peer_count--;
    rtc_mutex_unlock(&c->send_mutex);
}

static void pipeline_on_video(const char *peer_id, const char *label, const video_frame_t *frame,
                              void *user) {
    conf_peer_t *p = user;
    conference_t *c = p->conf;
    atomic_fetch_add(&p->video_frames, 1);
    p->last_video_ms = rtc_time_ms();
    rtc_mutex_lock(&c->render_mutex);
    bool selected = p->active && !c->leaving && p->epoch == c->active_epoch;
    if (selected) {
        rtc_mutex_lock(&c->send_mutex);
        for (int i = 0; i < c->participant_count; i++)
            if (strcmp(c->participants[i].id, peer_id) == 0)
                c->participants[i].stats.video_frames++;
        rtc_mutex_unlock(&c->send_mutex);
    }
    if (selected && c->cfg.callbacks.on_remote_video)
        c->cfg.callbacks.on_remote_video(peer_id, label, frame, c->cfg.user_data);
    rtc_mutex_unlock(&c->render_mutex);
}

static void pipeline_on_audio(const char *peer_id, const char *label, const audio_frame_t *audio,
                              void *user) {
    conf_peer_t *p = user;
    conference_t *c = p->conf;
    atomic_fetch_add(&p->audio_frames, 1);
    p->last_audio_ms = rtc_time_ms();
    rtc_mutex_lock(&c->render_mutex);
    bool selected = p->active && !c->leaving && p->epoch == c->active_epoch;
    if (selected) {
        rtc_mutex_lock(&c->send_mutex);
        for (int i = 0; i < c->participant_count; i++)
            if (strcmp(c->participants[i].id, peer_id) == 0)
                c->participants[i].stats.audio_frames++;
        rtc_mutex_unlock(&c->send_mutex);
    }
    if (selected && c->cfg.callbacks.on_remote_audio)
        c->cfg.callbacks.on_remote_audio(peer_id, label, audio, c->cfg.user_data);
    rtc_mutex_unlock(&c->render_mutex);
}

static void on_conn_state(rtc_connection_state_t state, void *user) {
    conf_peer_t *p = (conf_peer_t *)user;
    if (state != RTC_CONNECTION_CONNECTED)
        p->connected = false;
    if (state == RTC_CONNECTION_CONNECTED) {
        if (!p->active)
            return;
        atomic_store(&p->connected_pending, true);
        p->connected = true;
        bool notify = !p->conf->v2;
        rtc_mutex_lock(&p->conf->send_mutex);
        for (int i = 0; i < p->conf->participant_count; i++) {
            conf_participant_t *participant = &p->conf->participants[i];
            if (p->epoch == p->conf->active_epoch && strcmp(participant->id, p->peer_id) == 0 &&
                !participant->connected_notified) {
                participant->connected_notified = true;
                notify = true;
            }
        }
        rtc_mutex_unlock(&p->conf->send_mutex);
        if (notify && p->conf->cfg.callbacks.on_connected)
            p->conf->cfg.callbacks.on_connected(p->peer_id, p->conf->cfg.user_data);
    } else if (state == RTC_CONNECTION_FAILED && p->conf->v2 && p->active && !p->conf->leaving) {
        conference_t *c = p->conf;
        rtc_mutex_lock(&c->send_mutex);
        bool pending = p->epoch == c->pending_epoch;
        bool request = !p->sfu && !c->fallback_requested && p->epoch == c->active_epoch;
        c->fallback_requested |= request;
        rtc_mutex_unlock(&c->send_mutex);
        if (request || pending) {
            sig_msg_t message = {.type =
                                     pending ? SIG_MSG_TOPOLOGY_ABORT : SIG_MSG_TOPOLOGY_REQUEST,
                                 .version = SIG_PROTOCOL_VERSION,
                                 .epoch = p->epoch};
            snprintf(message.mode, sizeof(message.mode), "%s", pending && !p->sfu ? "mesh" : "sfu");
            snprintf(message.reason, sizeof(message.reason), "%s",
                     pending ? "pending ICE connection failed" : "mesh ICE connection failed");
            signaling_send_message(c->signaling, &message);
            if (c->cfg.callbacks.on_topology)
                c->cfg.callbacks.on_topology(c->sfu_mode ? "sfu" : "mesh", "error", c->active_epoch,
                                             message.reason, c->cfg.user_data);
        }
    }
}

static bool register_connected_peers(conference_t *c) {
    /* Registration touches encoders, so callbacks only publish pending state. */
    bool registered = true;
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        conf_peer_t *p = &c->peers[i];
        if (p->active && atomic_exchange(&p->connected_pending, false) &&
            (!p->sfu || strcmp(p->peer_id, "sfu") == 0)) {
            if (media_pipeline_add_send_peer(c->pipeline, p->session_id, p->video_sender,
                                             p->audio_sender) != RTC_OK)
                registered = false;
        }
    }
    return registered;
}

static void on_bitrate(uint32_t bitrate_bps, void *user) {
    conf_peer_t *p = user;
    if (p->active && p->conf->pipeline)
        media_pipeline_set_peer_bitrate(p->conf->pipeline, p->session_id, bitrate_bps);
}

static int send_session(conf_peer_t *p, sig_msg_type_t type, const char *text) {
    conference_t *c = p->conf;
    if (c->leaving || !c->signaling)
        return RTC_ERR_INVALID;
    if (!c->v2) {
        if (type == SIG_MSG_OFFER)
            return signaling_send_offer(c->signaling, p->peer_id, text);
        if (type == SIG_MSG_ANSWER)
            return signaling_send_answer(c->signaling, p->peer_id, text);
        return signaling_send_candidate(c->signaling, p->peer_id, text);
    }
    sig_msg_t message = {.type = type, .version = SIG_PROTOCOL_VERSION, .epoch = p->epoch};
    snprintf(message.to, sizeof(message.to), "%s", p->peer_id);
    snprintf(message.session_id, sizeof(message.session_id), "%s", p->session_id);
    if (type == SIG_MSG_CANDIDATE)
        snprintf(message.candidate, sizeof(message.candidate), "%s", text);
    else
        snprintf(message.sdp, sizeof(message.sdp), "%s", text);
    return signaling_send_message(c->signaling, &message);
}

static void on_local_candidate(const rtc_ice_candidate_desc_t *candidate, void *user) {
    conf_peer_t *p = user;
    if (!p->active || p->conf->leaving)
        return;
    rtc_mutex_lock(&p->candidate_mutex);
    if (!p->description_sent) {
        if (!candidate)
            p->gathering_done = true;
        else if (p->pending_candidate_count < ICE_MAX_CANDIDATES)
            p->pending_candidates[p->pending_candidate_count++] = *candidate;
        else
            report_error(p->conf, "local candidate queue full");
        rtc_mutex_unlock(&p->candidate_mutex);
        return;
    }
    if (send_session(p, SIG_MSG_CANDIDATE, candidate ? candidate->candidate : "") != RTC_OK)
        report_error(p->conf, "failed to forward local ICE candidate");
    rtc_mutex_unlock(&p->candidate_mutex);
}

static void flush_candidates(conf_peer_t *p) {
    rtc_mutex_lock(&p->candidate_mutex);
    p->description_sent = true;
    for (int i = 0; i < p->pending_candidate_count; i++)
        if (send_session(p, SIG_MSG_CANDIDATE, p->pending_candidates[i].candidate) != RTC_OK)
            report_error(p->conf, "failed to forward queued ICE candidate");
    p->pending_candidate_count = 0;
    if (p->gathering_done)
        if (send_session(p, SIG_MSG_CANDIDATE, "") != RTC_OK)
            report_error(p->conf, "failed to forward end-of-candidates");
    rtc_mutex_unlock(&p->candidate_mutex);
}

static void begin_description(conf_peer_t *p) {
    rtc_mutex_lock(&p->candidate_mutex);
    p->description_sent = false;
    p->pending_candidate_count = 0;
    p->gathering_done = false;
    rtc_mutex_unlock(&p->candidate_mutex);
}

static void request_video_keyframe(void *user) {
    rtc_rtp_receiver_request_keyframe(user);
}

static void on_recv_video_rtp(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                              uint32_t ssrc, bool marker, void *user) {
    conf_peer_t *cp = (conf_peer_t *)user;
    if (!cp->active || !cp->receive_pipeline || !payload || len == 0)
        return;
    /* Lazily register recv stream on first packet, then cache the handle */
    if (!cp->recv_video) {
        const char *vc = cp->conf->cfg.video_codec ? cp->conf->cfg.video_codec : "VP8";
        cp->recv_video = media_pipeline_add_recv_stream(cp->receive_pipeline, cp->peer_id, "camera",
                                                        ssrc, true, vc);
        if (!cp->recv_video)
            return;
        if (media_pipeline_set_recv_feedback(cp->receive_pipeline, cp->recv_video,
                                             request_video_keyframe, cp->video_receiver) != RTC_OK)
            report_error(cp->conf, "could not install receive keyframe feedback");
    }
    media_pipeline_recv_rtp(cp->receive_pipeline, cp->recv_video, payload, (int)len, seq, timestamp,
                            marker);
}

static void on_recv_audio_rtp(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                              uint32_t ssrc, bool marker, void *user) {
    conf_peer_t *cp = (conf_peer_t *)user;
    (void)marker;
    if (!cp->active || !cp->receive_pipeline || !payload || len == 0)
        return;
    /* Lazily register recv stream on first packet, then cache the handle */
    if (!cp->recv_audio) {
        const char *ac = cp->conf->cfg.audio_codec ? cp->conf->cfg.audio_codec : "opus";
        cp->recv_audio = media_pipeline_add_recv_stream(cp->receive_pipeline, cp->peer_id, "mic",
                                                        ssrc, false, ac);
        if (!cp->recv_audio)
            return;
    }
    media_pipeline_recv_rtp(cp->receive_pipeline, cp->recv_audio, payload, (int)len, seq, timestamp,
                            false);
}

static void on_remote_track(rtc_rtp_receiver_t *receiver, void *user) {
    conf_peer_t *cp = (conf_peer_t *)user;
    if (!cp->conf || !cp->conf->pipeline)
        return;

    /* Wire callback based on track kind.
     * Recv streams are lazily registered on first RTP packet
     * (when the real SSRC is known from the RTP header). */
    rtc_kind_t kind = rtc_rtp_receiver_kind(receiver);
    if ((kind == RTC_KIND_VIDEO && cp->conf->cfg.disable_receive_video) ||
        (kind == RTC_KIND_AUDIO && cp->conf->cfg.disable_receive_audio))
        return;
    if (kind == RTC_KIND_VIDEO) {
        cp->video_receiver = receiver;
        rtc_rtp_receiver_on_frame(receiver, on_recv_video_rtp, cp);
        RTC_LOG_INFO("Conference: receiving video from %s", cp->peer_id);
    } else {
        rtc_rtp_receiver_on_frame(receiver, on_recv_audio_rtp, cp);
        RTC_LOG_INFO("Conference: receiving audio from %s", cp->peer_id);
    }
}

/* Data channel callbacks */
static void on_dc_open(void *user) {
    conf_peer_t *p = (conf_peer_t *)user;
    p->dc_open = p->active;
}

static void on_dc_message(const uint8_t *data, size_t len, void *user) {
    conf_peer_t *p = (conf_peer_t *)user;
    char source[SIG_MAX_PEER_ID];
    const char *peer_id = p->peer_id;
    conference_t *c = p->conf;
    if (!p->active || c->leaving)
        return;
    if (c->v2) {
        uint64_t message_id;
        if (sig_dc_read(data, len, source, &message_id, &data, &len) != 0 || !message_id ||
            (!p->sfu && strcmp(source, p->peer_id) != 0))
            return;
        bool accept = false;
        rtc_mutex_lock(&c->send_mutex);
        for (int i = 0; i < c->participant_count; i++) {
            conf_participant_t *participant = &c->participants[i];
            if (strcmp(participant->id, source) != 0)
                continue;
            accept = true;
            for (size_t j = 0; j < 64; j++)
                if (participant->recent_ids[j] == message_id)
                    accept = false;
            if (accept)
                participant->recent_ids[participant->recent_next++ % 64] = message_id;
            break;
        }
        rtc_mutex_unlock(&c->send_mutex);
        if (!accept)
            return;
        peer_id = source;
    } else if (p->sfu && len >= SIG_DC_HEADER_SIZE && memcmp(data, "MRTC", 4) == 0) {
        memcpy(source, data + 4, sizeof(source));
        source[sizeof(source) - 1] = '\0';
        peer_id = source;
        data += SIG_DC_HEADER_SIZE;
        len -= SIG_DC_HEADER_SIZE;
    }
    if (p->conf->cfg.callbacks.on_message)
        p->conf->cfg.callbacks.on_message(peer_id, data, len, p->conf->cfg.user_data);
}

static void on_dc_channel(rtc_data_channel_t *channel, void *user) {
    conf_peer_t *p = (conf_peer_t *)user;
    rtc_data_channel_on_open(channel, on_dc_open, p);
    rtc_data_channel_on_message(channel, on_dc_message, p);
    bool open = rtc_data_channel_state(channel) == RTC_DC_OPEN;
    if (p->active) {
        p->dc = channel;
        p->dc_open = open;
    }
}

static rtc_peer_connection_t *create_pc_with_tracks(conference_t *c, conf_peer_t *cp,
                                                    bool local_tracks) {
    if (cp->pc)
        return cp->pc;
    media_renderer_t renderer = {
        .on_video_frame = pipeline_on_video, .on_audio_samples = pipeline_on_audio, .user = cp};
    media_pipeline_config_t receive_config = {.renderer = &renderer};
    if (!cp->receive_pipeline)
        cp->receive_pipeline = media_pipeline_create(&receive_config);
    if (!cp->receive_pipeline)
        return NULL;
    rtc_config_t config;
    memset(&config, 0, sizeof(config));
    if (c->cfg.stun_server && c->cfg.stun_server[0]) {
        if (strchr(c->cfg.stun_server, ':')) {
            config.ice_servers[0].urls[0] = c->cfg.stun_server;
            config.ice_servers[0].url_count = 1;
            config.ice_server_count = 1;
        } else {
            config.stun_server = c->cfg.stun_server;
        }
    }

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    if (!pc)
        return NULL;

    rtc_peer_connection_on_connection_state(pc, on_conn_state, cp);
    rtc_peer_connection_on_data_channel(pc, on_dc_channel, cp);
    rtc_peer_connection_on_track(pc, on_remote_track, cp);
    rtc_peer_connection_on_ice_candidate(pc, on_local_candidate, cp);
    rtc_peer_connection_on_bitrate_estimate(pc, on_bitrate, cp);

    bool send_video = !c->v2 && !cp->sfu;
    bool send_audio = !c->v2 && !cp->sfu;
    if (!cp->sfu || strcmp(cp->peer_id, "sfu") == 0) {
        for (int i = 0; i < CONF_MAX_SOURCES; i++) {
            if (!c->sources[i].active)
                continue;
            if (c->sources[i].is_video)
                send_video = true;
            else
                send_audio = true;
        }
    }

    /* Add video track (VP8) */
    rtc_codec_t vp8;
    memset(&vp8, 0, sizeof(vp8));
    vp8.payload_type = 96;
    strcpy(vp8.mime_type, "video/VP8");
    vp8.clock_rate = 90000;

    /* Add audio track (Opus) */
    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    if (local_tracks) {
        bool receive_video = !cp->sfu && !c->cfg.disable_receive_video;
        bool receive_audio = !cp->sfu && !c->cfg.disable_receive_audio;
        const rtc_codec_t *codecs[] = {&vp8, &opus};
        const bool sending[] = {send_video, send_audio};
        const bool receiving[] = {receive_video, receive_audio};
        for (int i = 0; i < 2; i++) {
            if (!sending[i] && !receiving[i])
                continue;
            rtc_rtp_transceiver_init_t init = {
                .direction = sending[i] ? (receiving[i] ? RTC_DIR_SENDRECV : RTC_DIR_SENDONLY)
                                        : RTC_DIR_RECVONLY};
            rtc_rtp_transceiver_t *transceiver = rtc_peer_connection_add_transceiver(
                pc, i == 0 ? RTC_KIND_VIDEO : RTC_KIND_AUDIO, codecs[i], &init);
            if (!transceiver) {
                rtc_peer_connection_close(pc);
                rtc_peer_connection_destroy(pc);
                cp->video_sender = cp->audio_sender = NULL;
                return NULL;
            }
            if (sending[i]) {
                if (i == 0)
                    cp->video_sender = rtc_rtp_transceiver_sender(transceiver);
                else
                    cp->audio_sender = rtc_rtp_transceiver_sender(transceiver);
            }
        }
    }

    cp->pc = pc;
    return pc;
}

static rtc_peer_connection_t *create_pc(conference_t *c, conf_peer_t *cp) {
    return create_pc_with_tracks(c, cp, true);
}

static int configure_answer_senders(conference_t *c, conf_peer_t *cp) {
    rtc_rtp_transceiver_t *transceivers[RTC_MAX_TRANSCEIVERS];
    int count = RTC_MAX_TRANSCEIVERS;
    if (rtc_peer_connection_get_transceivers(cp->pc, transceivers, &count) != RTC_OK)
        return RTC_ERR_GENERIC;
    for (int i = 0; i < count; i++) {
        rtc_rtp_sender_t *sender = rtc_rtp_transceiver_sender(transceivers[i]);
        const rtc_codec_t *codec = rtc_rtp_sender_get_codec(sender);
        bool video = rtc_rtp_sender_kind(sender) == RTC_KIND_VIDEO;
        if (!codec || strcmp(codec->mime_type, video ? "video/VP8" : "audio/opus") != 0)
            return RTC_ERR_SDP;
        bool send = false;
        for (int j = 0; j < CONF_MAX_SOURCES; j++)
            send |= (!cp->sfu || cp->role == SIG_SESSION_PUBLISH) && c->sources[j].active &&
                    c->sources[j].is_video == video;
        bool receive = !(video ? c->cfg.disable_receive_video : c->cfg.disable_receive_audio);
        rtc_rtp_transceiver_set_direction(transceivers[i],
                                          send ? (receive ? RTC_DIR_SENDRECV : RTC_DIR_SENDONLY)
                                               : (receive ? RTC_DIR_RECVONLY : RTC_DIR_INACTIVE));
        if (send) {
            if (video)
                cp->video_sender = sender;
            else
                cp->audio_sender = sender;
        }
    }
    return RTC_OK;
}

static int offer_to_peer(conference_t *c, conf_peer_t *cp) {
    rtc_peer_connection_t *pc = create_pc(c, cp);
    if (!pc)
        return -1;

    /* Create data channel */
    rtc_data_channel_init_t dc_opts;
    memset(&dc_opts, 0, sizeof(dc_opts));
    dc_opts.ordered = true;
    dc_opts.max_retransmits = -1;
    dc_opts.max_packet_life = -1;
    dc_opts.id = -1;
    rtc_data_channel_t *channel = rtc_peer_connection_create_data_channel(pc, "chat", &dc_opts);
    if (channel) {
        rtc_mutex_lock(&c->send_mutex);
        cp->dc = channel;
        rtc_mutex_unlock(&c->send_mutex);
        rtc_data_channel_on_open(channel, on_dc_open, cp);
        rtc_data_channel_on_message(channel, on_dc_message, cp);
    }

    rtc_desc_t offer;
    begin_description(cp);
    int rc = rtc_peer_connection_create_offer(pc, &offer);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_peer_connection_set_local_desc(pc, &offer);
    if (rc != RTC_OK)
        return rc;

    rc = send_session(cp, SIG_MSG_OFFER, offer.sdp);
    if (rc != RTC_OK)
        return rc;
    flush_candidates(cp);
    RTC_LOG_INFO("Conference: sent offer to %s", cp->peer_id);
    return 0;
}

static int negotiate_answer_extensions(rtc_desc_t *answer, const rtc_desc_t *offer) {
    rtc_sdp_t local = {0}, remote = {0};
    int rc = rtc_sdp_parse(&local, answer->sdp, answer->sdp_len);
    if (rc == RTC_OK)
        rc = rtc_sdp_parse(&remote, offer->sdp, offer->sdp_len);
    if (rc == RTC_OK) {
        local.type = RTC_SDP_ANSWER;
        for (int i = 0; i < local.media_count; i++) {
            rtc_sdp_media_t *media = &local.media[i];
            int count = 0;
            for (int j = 0; j < remote.media_count; j++) {
                if (remote.media[j].mid_index != media->mid_index)
                    continue;
                for (int k = 0; k < media->extmap_count; k++) {
                    rtc_sdp_extmap_t extension = media->extmaps[k];
                    extension.id = rtc_sdp_media_find_extmap_id(&remote.media[j], extension.uri);
                    if (extension.id)
                        media->extmaps[count++] = extension;
                }
                break;
            }
            media->extmap_count = count;
        }
        rc = rtc_sdp_generate(&local);
        if (rc == RTC_OK) {
            memcpy(answer->sdp, local.raw, local.raw_len + 1);
            answer->sdp_len = local.raw_len;
        }
    }
    rtc_sdp_close(&local);
    rtc_sdp_close(&remote);
    return rc;
}

static int accept_offer(conference_t *c, conf_peer_t *cp, const char *sdp) {
    /* Let the offer establish MID order and payload types before attaching
     * local source senders; browsers need not offer video first. */
    rtc_peer_connection_t *pc = cp->pc ? cp->pc : create_pc_with_tracks(c, cp, false);
    if (!pc)
        return -1;

    rtc_desc_t offer_desc;
    memset(&offer_desc, 0, sizeof(offer_desc));
    offer_desc.type = RTC_SDP_OFFER;
    size_t sdp_len = strlen(sdp);
    if (sdp_len >= SDP_MAX_SIZE)
        return RTC_ERR_SDP;
    memcpy(offer_desc.sdp, sdp, sdp_len);
    offer_desc.sdp_len = sdp_len;

    int rc = rtc_peer_connection_set_remote_desc(pc, &offer_desc);
    if (rc != RTC_OK)
        return rc;
    rc = configure_answer_senders(c, cp);
    if (rc != RTC_OK)
        return rc;

    rtc_desc_t answer;
    begin_description(cp);
    rc = rtc_peer_connection_create_answer(pc, &answer);
    if (rc != RTC_OK)
        return rc;
    /* BUNDLE extension IDs belong to the offer, including IDs used by
     * extensions this client does not implement. Never reassign them. */
    rc = negotiate_answer_extensions(&answer, &offer_desc);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_peer_connection_set_local_desc(pc, &answer);
    if (rc != RTC_OK)
        return rc;

    rc = send_session(cp, SIG_MSG_ANSWER, answer.sdp);
    if (rc != RTC_OK)
        return rc;
    flush_candidates(cp);
    RTC_LOG_INFO("Conference: sent answer to %s", cp->peer_id);
    return 0;
}

static int handle_offer(conference_t *c, const char *from, const char *sdp) {
    conf_peer_t *cp = find_peer(c, from);
    if (!cp)
        cp = alloc_peer(c, from);
    return cp ? accept_offer(c, cp, sdp) : RTC_ERR_NOMEM;
}

static int accept_answer(conf_peer_t *cp, const char *sdp) {
    if (!cp || !cp->pc)
        return RTC_ERR_INVALID;
    rtc_desc_t answer_desc = {.type = RTC_SDP_ANSWER};
    size_t sdp_len = strlen(sdp);
    if (sdp_len >= SDP_MAX_SIZE)
        return RTC_ERR_SDP;
    memcpy(answer_desc.sdp, sdp, sdp_len);
    answer_desc.sdp_len = sdp_len;
    return rtc_peer_connection_set_remote_desc(cp->pc, &answer_desc);
}

static int handle_answer(conference_t *c, const char *from, const char *sdp) {
    conf_peer_t *cp = find_peer(c, from);
    return accept_answer(cp, sdp);
}

static void sig_on_joined(const char *my_id, const char **peer_ids, int n, void *user) {
    conference_t *c = (conference_t *)user;
    snprintf(c->my_id, sizeof(c->my_id), "%s", my_id);

    if (c->cfg.callbacks.on_joined)
        c->cfg.callbacks.on_joined(my_id, c->cfg.user_data);

    if (c->sfu_mode) {
        conf_peer_t *publisher = alloc_peer(c, "sfu");
        if (!publisher || offer_to_peer(c, publisher) != RTC_OK)
            report_error(c, "SFU publishing negotiation failed");
        for (int i = 0; i < n; i++)
            if (c->cfg.callbacks.on_peer_joined)
                c->cfg.callbacks.on_peer_joined(peer_ids[i], c->cfg.user_data);
        return;
    }
    for (int i = 0; i < n; i++) {
        conf_peer_t *cp = alloc_peer(c, peer_ids[i]);
        if (cp)
            offer_to_peer(c, cp);
    }
}

static void sig_on_peer_joined(const char *peer_id, void *user) {
    conference_t *c = (conference_t *)user;
    if (c->cfg.callbacks.on_peer_joined)
        c->cfg.callbacks.on_peer_joined(peer_id, c->cfg.user_data);
}

static void sig_on_peer_left(const char *peer_id, void *user) {
    conference_t *c = (conference_t *)user;
    conf_peer_t *cp = find_peer(c, peer_id);
    if (cp)
        destroy_peer(c, cp);
    if (c->cfg.callbacks.on_peer_left)
        c->cfg.callbacks.on_peer_left(peer_id, c->cfg.user_data);
}

static void sig_on_disconnected(void *user) {
    conference_t *c = user;
    if (!c->leaving && c->cfg.callbacks.on_topology)
        c->cfg.callbacks.on_topology(c->sfu_mode ? "sfu" : "mesh", "error", c->active_epoch,
                                     "signaling disconnected", c->cfg.user_data);
    c->leaving = true;
    if (c->v2) {
        for (int i = 0; i < CONF_MAX_PEERS; i++)
            if (c->peers[i].active)
                destroy_peer(c, &c->peers[i]);
        for (int i = 0; i < c->participant_count; i++)
            if (c->cfg.callbacks.on_peer_left)
                c->cfg.callbacks.on_peer_left(c->participants[i].id, c->cfg.user_data);
        rtc_mutex_lock(&c->send_mutex);
        c->participant_count = 0;
        c->pending_epoch = 0;
        rtc_mutex_unlock(&c->send_mutex);
        return;
    }
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        conf_peer_t *peer = &c->peers[i];
        if (!peer->active)
            continue;
        if (c->sfu_mode && strcmp(peer->peer_id, "sfu") == 0)
            destroy_peer(c, peer);
        else
            sig_on_peer_left(peer->peer_id, c);
    }
}

static void sig_on_offer(const char *from, const char *sdp, void *user) {
    conference_t *c = (conference_t *)user;
    if (handle_offer(c, from, sdp) != RTC_OK)
        report_error(c, "remote offer rejected");
}

static void sig_on_answer(const char *from, const char *sdp, void *user) {
    conference_t *c = (conference_t *)user;
    if (handle_answer(c, from, sdp) != RTC_OK)
        report_error(c, "remote answer rejected");
}

static void sig_on_candidate(const char *from, const char *cand, void *user) {
    conference_t *c = user;
    conf_peer_t *p = find_peer(c, from);
    rtc_ice_candidate_desc_t desc = {0};
    if (cand)
        snprintf(desc.candidate, sizeof(desc.candidate), "%s", cand);
    if (!p || !p->pc ||
        rtc_peer_connection_add_ice_candidate(p->pc, cand && cand[0] ? &desc : NULL) != RTC_OK)
        report_error(c, "remote ICE candidate rejected");
}

static void sig_on_mode(const char *mode, void *user) {
    conference_t *c = user;
    c->sfu_mode = strcmp(mode, "sfu") == 0;
}

static void sig_on_error(const char *msg, void *user) {
    conference_t *c = (conference_t *)user;
    if (c->cfg.callbacks.on_error)
        c->cfg.callbacks.on_error(msg, c->cfg.user_data);
}

static void topology_status(conference_t *c, const char *status, uint32_t epoch, bool sfu,
                            const char *reason) {
    if (c->cfg.callbacks.on_topology)
        c->cfg.callbacks.on_topology(sfu ? "sfu" : "mesh", status, epoch, reason, c->cfg.user_data);
}

static void retire_epoch(conference_t *c, uint32_t epoch) {
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active && c->peers[i].epoch == epoch)
            destroy_peer(c, &c->peers[i]);
}

static void abort_pending(conference_t *c, const char *reason) {
    rtc_mutex_lock(&c->send_mutex);
    uint32_t epoch = c->pending_epoch;
    c->pending_epoch = 0;
    c->ready_sent = false;
    rtc_mutex_unlock(&c->send_mutex);
    if (epoch)
        retire_epoch(c, epoch);
    topology_status(c, "error", c->active_epoch, c->sfu_mode, reason);
}

static bool participant_add_named(conference_t *c, const char *id, const char *name) {
    if (!id[0] || strcmp(id, c->my_id) == 0)
        return false;
    rtc_mutex_lock(&c->send_mutex);
    for (int i = 0; i < c->participant_count; i++)
        if (strcmp(c->participants[i].id, id) == 0) {
            if (name && name[0])
                snprintf(c->participants[i].name, sizeof(c->participants[i].name), "%s", name);
            rtc_mutex_unlock(&c->send_mutex);
            return false;
        }
    if (c->participant_count >= SIG_MAX_PEERS - 1) {
        rtc_mutex_unlock(&c->send_mutex);
        report_error(c, "participant limit exceeded");
        return false;
    }
    conf_participant_t *participant = &c->participants[c->participant_count++];
    memset(participant, 0, sizeof(*participant));
    snprintf(participant->id, sizeof(participant->id), "%s", id);
    snprintf(participant->name, sizeof(participant->name), "%s", name && name[0] ? name : id);
    rtc_mutex_unlock(&c->send_mutex);
    if (c->cfg.callbacks.on_peer_joined)
        c->cfg.callbacks.on_peer_joined(id, c->cfg.user_data);
    return true;
}

static int build_mesh_session(conference_t *c, const char *id, uint32_t epoch) {
    char session[SIG_MAX_SESSION_ID];
    if (sig_session_id(session, SIG_SESSION_MESH, epoch, c->my_id, id) != 0)
        return RTC_ERR_INVALID;
    conf_peer_t *peer = alloc_session(c, id, session, epoch, SIG_SESSION_MESH, false);
    if (!peer)
        return RTC_ERR_NOMEM;
    if (strcmp(c->my_id, id) < 0 && !peer->pc)
        return offer_to_peer(c, peer);
    return RTC_OK;
}

static int build_graph(conference_t *c, uint32_t epoch, bool sfu) {
    if (sfu) {
        char session[SIG_MAX_SESSION_ID];
        if (sig_session_id(session, SIG_SESSION_PUBLISH, epoch, c->my_id, NULL) != 0)
            return RTC_ERR_INVALID;
        conf_peer_t *publisher = alloc_session(c, "sfu", session, epoch, SIG_SESSION_PUBLISH, true);
        return publisher ? offer_to_peer(c, publisher) : RTC_ERR_NOMEM;
    }
    for (int i = 0; i < c->participant_count; i++)
        if (build_mesh_session(c, c->participants[i].id, epoch) != RTC_OK)
            return RTC_ERR_GENERIC;
    return RTC_OK;
}

static bool graph_ready_locked(conference_t *c, uint32_t epoch) {
    uint64_t now = rtc_time_ms();
    int transports = 0;
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        conf_peer_t *pending = &c->peers[i];
        if (!pending->active || pending->epoch != epoch)
            continue;
        transports++;
        if (!pending->connected || (pending->role != SIG_SESSION_SUBSCRIBE && !pending->dc_open))
            return false;
    }
    int necessary = c->pending_sfu ? 1 : c->participant_count;
    if (transports < necessary)
        return false;
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        conf_peer_t *old = &c->peers[i];
        if (!old->active || old->epoch != c->active_epoch || strcmp(old->peer_id, "sfu") == 0)
            continue;
        bool video = old->last_video_ms && now - old->last_video_ms < 1500;
        bool audio = old->last_audio_ms && now - old->last_audio_ms < 1500;
        if (!video && !audio)
            continue;
        conf_peer_t *pending = NULL;
        for (int j = 0; j < CONF_MAX_PEERS; j++)
            if (c->peers[j].active && c->peers[j].epoch == epoch &&
                strcmp(c->peers[j].peer_id, old->peer_id) == 0)
                pending = &c->peers[j];
        if (!pending || (video && !pending->video_frames) || (audio && !pending->audio_frames))
            return false;
    }
    return true;
}

static void check_topology_ready(conference_t *c) {
    rtc_mutex_lock(&c->send_mutex);
    bool ready = !c->leaving && c->pending_epoch && !c->ready_sent &&
                 graph_ready_locked(c, c->pending_epoch);
    uint32_t epoch = c->pending_epoch;
    bool sfu = c->pending_sfu;
    if (ready)
        c->ready_sent = true;
    /* Keep signaling alive until this queue operation completes. */
    if (ready && c->signaling) {
        sig_msg_t message = {
            .type = SIG_MSG_TOPOLOGY_READY, .version = SIG_PROTOCOL_VERSION, .epoch = epoch};
        snprintf(message.mode, sizeof(message.mode), "%s", sfu ? "sfu" : "mesh");
        if (signaling_send_message(c->signaling, &message) != RTC_OK)
            c->ready_sent = false;
    }
    rtc_mutex_unlock(&c->send_mutex);
}

static void *topology_loop(void *user) {
    conference_t *c = user;
    while (!c->stop_topology) {
        check_topology_ready(c);
        rtc_mutex_lock(&c->send_mutex);
        if (!c->stop_topology)
            rtc_cond_wait_timeout(&c->send_cond, &c->send_mutex, 50);
        rtc_mutex_unlock(&c->send_mutex);
    }
    return NULL;
}

static void sig_on_event(const sig_msg_t *message, void *user) {
    conference_t *c = user;
    if (c->leaving)
        return;
    if (message->version != SIG_PROTOCOL_VERSION) {
        report_error(c, "conference requires signaling protocol v2");
        return;
    }
    if (message->type == SIG_MSG_JOINED) {
        if (!message->epoch || (strcmp(message->mode, "mesh") && strcmp(message->mode, "sfu")) ||
            message->peer_count < 0 || message->peer_count >= SIG_MAX_PEERS) {
            report_error(c, "invalid joined topology");
            return;
        }
        c->v2 = true;
        snprintf(c->my_id, sizeof(c->my_id), "%s", message->peer_id);
        c->active_epoch = message->epoch;
        c->highest_epoch = message->epoch;
        c->sfu_mode = strcmp(message->mode, "sfu") == 0;
        if (c->cfg.callbacks.on_joined)
            c->cfg.callbacks.on_joined(c->my_id, c->cfg.user_data);
        for (int i = 0; i < message->peer_count; i++)
            participant_add_named(c, message->peers[i], message->peer_names[i]);
        topology_status(c, "active", c->active_epoch, c->sfu_mode, "");
        if (build_graph(c, c->active_epoch, c->sfu_mode) != RTC_OK)
            report_error(c, "initial topology negotiation failed");
        return;
    }
    if (message->type == SIG_MSG_ERROR) {
        sig_on_error(message->error_msg, c);
        return;
    }
    if (message->type == SIG_MSG_PEER_JOINED) {
        if (c->pending_epoch)
            abort_pending(c, "membership changed");
        if (participant_add_named(c, message->peer_id, message->display_name) && !c->sfu_mode &&
            build_mesh_session(c, message->peer_id, c->active_epoch) != RTC_OK)
            report_error(c, "mesh participant negotiation failed");
        return;
    }
    if (message->type == SIG_MSG_PEER_LEFT) {
        if (c->pending_epoch)
            abort_pending(c, "membership changed");
        for (int i = 0; i < CONF_MAX_PEERS; i++)
            if (c->peers[i].active && strcmp(c->peers[i].peer_id, message->peer_id) == 0)
                destroy_peer(c, &c->peers[i]);
        bool removed = false;
        rtc_mutex_lock(&c->send_mutex);
        for (int i = 0; i < c->participant_count; i++)
            if (strcmp(c->participants[i].id, message->peer_id) == 0) {
                c->participants[i] = c->participants[--c->participant_count];
                removed = true;
                break;
            }
        rtc_mutex_unlock(&c->send_mutex);
        if (removed && c->cfg.callbacks.on_peer_left)
            c->cfg.callbacks.on_peer_left(message->peer_id, c->cfg.user_data);
        return;
    }
    if (message->type == SIG_MSG_TOPOLOGY_PREPARE) {
        if (message->epoch <= c->highest_epoch ||
            (strcmp(message->mode, "mesh") && strcmp(message->mode, "sfu")) ||
            (strcmp(message->mode, "sfu") == 0) == c->sfu_mode)
            return;
        if (message->epoch == c->pending_epoch)
            return;
        if (message->peer_count < 0 || message->peer_count >= SIG_MAX_PEERS)
            return;
        for (int i = 0; i < message->peer_count; i++)
            participant_add_named(c, message->peers[i], message->peer_names[i]);
        if (c->pending_epoch)
            abort_pending(c, "topology superseded");
        rtc_mutex_lock(&c->send_mutex);
        c->highest_epoch = message->epoch;
        c->pending_epoch = message->epoch;
        c->pending_sfu = strcmp(message->mode, "sfu") == 0;
        c->ready_sent = false;
        rtc_mutex_unlock(&c->send_mutex);
        topology_status(c, "switching", message->epoch, c->pending_sfu, "");
        if (build_graph(c, message->epoch, c->pending_sfu) != RTC_OK) {
            sig_msg_t failure = {.type = SIG_MSG_TOPOLOGY_ABORT,
                                 .version = SIG_PROTOCOL_VERSION,
                                 .epoch = message->epoch};
            snprintf(failure.mode, sizeof(failure.mode), "%s", message->mode);
            snprintf(failure.reason, sizeof(failure.reason), "pending graph negotiation failed");
            signaling_send_message(c->signaling, &failure);
            abort_pending(c, failure.reason);
        }
        return;
    }
    if (message->type == SIG_MSG_TOPOLOGY_ABORT) {
        if (message->epoch == c->pending_epoch &&
            strcmp(message->mode, c->pending_sfu ? "sfu" : "mesh") == 0)
            abort_pending(c, message->reason);
        return;
    }
    if (message->type == SIG_MSG_TOPOLOGY_COMMIT) {
        rtc_mutex_lock(&c->send_mutex);
        bool prepared = message->epoch == c->pending_epoch && c->ready_sent &&
                        strcmp(message->mode, c->pending_sfu ? "sfu" : "mesh") == 0;
        rtc_mutex_unlock(&c->send_mutex);
        if (!prepared)
            return;
        rtc_mutex_lock(&c->render_mutex);
        rtc_mutex_lock(&c->send_mutex);
        uint32_t old_epoch = c->active_epoch;
        c->active_epoch = c->pending_epoch;
        c->sfu_mode = c->pending_sfu;
        c->pending_epoch = 0;
        c->ready_sent = false;
        rtc_mutex_unlock(&c->send_mutex);
        rtc_mutex_unlock(&c->render_mutex);
        sig_msg_t ack = {.type = SIG_MSG_TOPOLOGY_ACK,
                         .version = SIG_PROTOCOL_VERSION,
                         .epoch = c->active_epoch};
        snprintf(ack.mode, sizeof(ack.mode), "%s", c->sfu_mode ? "sfu" : "mesh");
        signaling_send_message(c->signaling, &ack);
        topology_status(c, "active", c->active_epoch, c->sfu_mode, "");
        retire_epoch(c, old_epoch);
        return;
    }
    if (message->type != SIG_MSG_OFFER && message->type != SIG_MSG_ANSWER &&
        message->type != SIG_MSG_CANDIDATE)
        return;
    if (!message->epoch ||
        (message->epoch != c->active_epoch && message->epoch != c->pending_epoch))
        return;
    bool sfu = message->epoch == c->active_epoch ? c->sfu_mode : c->pending_sfu;
    char expected[SIG_MAX_SESSION_ID];
    sig_session_role_t role = sfu ? SIG_SESSION_SUBSCRIBE : SIG_SESSION_MESH;
    if (sfu && strcmp(message->from, "sfu") == 0)
        role = SIG_SESSION_PUBLISH;
    if (sig_session_id(expected, role, message->epoch, c->my_id,
                       role == SIG_SESSION_PUBLISH ? NULL : message->from) != 0 ||
        strcmp(expected, message->session_id) != 0)
        return;
    conf_peer_t *peer = rtc_str_map_get(&c->peer_index, expected);
    bool established = peer && peer->pc && rtc_peer_connection_remote_desc(peer->pc) != NULL;
    if (message->type == SIG_MSG_OFFER && !sfu && strcmp(c->my_id, message->from) < 0 &&
        !established)
        return;
    if (!peer && message->type == SIG_MSG_OFFER && role != SIG_SESSION_PUBLISH) {
        bool member = false;
        for (int i = 0; i < c->participant_count; i++)
            member |= strcmp(c->participants[i].id, message->from) == 0;
        if (!member || (!sfu && strcmp(c->my_id, message->from) < 0))
            return;
        peer = alloc_session(c, message->from, expected, message->epoch, role, sfu);
    }
    if (!peer || !peer->active)
        return;
    int rc;
    if (message->type == SIG_MSG_OFFER)
        rc = accept_offer(c, peer, message->sdp);
    else if (message->type == SIG_MSG_ANSWER)
        rc = accept_answer(peer, message->sdp);
    else {
        rtc_ice_candidate_desc_t candidate = {0};
        snprintf(candidate.candidate, sizeof(candidate.candidate), "%s", message->candidate);
        rc = peer->pc
                 ? rtc_peer_connection_add_ice_candidate(peer->pc,
                                                         message->candidate[0] ? &candidate : NULL)
                 : RTC_ERR_INVALID;
    }
    if (rc != RTC_OK) {
        RTC_LOG_ERR("Conference session %s epoch=%u type=%d negotiation failed: %d",
                    peer->session_id, peer->epoch, (int)message->type, rc);
        report_error(c, "versioned session negotiation failed");
    }
}

conference_t *conference_create(const conference_config_t *cfg) {
    if (!cfg)
        return NULL;
    conference_t *c = (conference_t *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (rtc_mutex_init(&c->send_mutex) != RTC_OK) {
        RTC_LOG_ERR("Conference send mutex initialization failed");
        free(c);
        return NULL;
    }
    if (rtc_cond_init(&c->send_cond) != RTC_OK) {
        rtc_mutex_destroy(&c->send_mutex);
        free(c);
        return NULL;
    }
    if (rtc_mutex_init(&c->pipeline_mutex) != RTC_OK) {
        rtc_cond_destroy(&c->send_cond);
        rtc_mutex_destroy(&c->send_mutex);
        free(c);
        return NULL;
    }
    if (rtc_mutex_init(&c->render_mutex) != RTC_OK) {
        rtc_mutex_destroy(&c->pipeline_mutex);
        rtc_cond_destroy(&c->send_cond);
        rtc_mutex_destroy(&c->send_mutex);
        free(c);
        return NULL;
    }
    c->cfg = *cfg;
    rtc_str_map_init(&c->source_index);
    rtc_str_map_init(&c->peer_index);
    /* Set defaults */
    if (!c->cfg.video_codec)
        c->cfg.video_codec = "VP8";
    if (!c->cfg.audio_codec)
        c->cfg.audio_codec = "opus";
    if (c->cfg.video_width <= 0)
        c->cfg.video_width = 640;
    if (c->cfg.video_height <= 0)
        c->cfg.video_height = 480;
    if (c->cfg.video_fps <= 0)
        c->cfg.video_fps = 30;
    if (c->cfg.video_bitrate_kbps <= 0)
        c->cfg.video_bitrate_kbps = 500;
    if (c->cfg.audio_sample_rate <= 0)
        c->cfg.audio_sample_rate = 48000;
    if (c->cfg.audio_channels <= 0)
        c->cfg.audio_channels = 1;
    if (c->cfg.audio_bitrate_bps <= 0)
        c->cfg.audio_bitrate_bps = 32000;

    /* Create shared media pipeline */
    media_pipeline_config_t mp_cfg = {
        .default_video_codec = c->cfg.video_codec,
        .default_audio_codec = c->cfg.audio_codec,
    };
    c->pipeline = media_pipeline_create(&mp_cfg);
    if (!c->pipeline) {
        rtc_str_map_free(&c->source_index);
        rtc_str_map_free(&c->peer_index);
        rtc_mutex_destroy(&c->send_mutex);
        rtc_cond_destroy(&c->send_cond);
        rtc_mutex_destroy(&c->pipeline_mutex);
        rtc_mutex_destroy(&c->render_mutex);
        free(c);
        return NULL;
    }
    c->next_message_id = 1;
    if (rtc_thread_create(&c->topology_thread, topology_loop, c) != RTC_OK) {
        media_pipeline_destroy(c->pipeline);
        rtc_mutex_destroy(&c->render_mutex);
        rtc_mutex_destroy(&c->pipeline_mutex);
        rtc_cond_destroy(&c->send_cond);
        rtc_mutex_destroy(&c->send_mutex);
        free(c);
        return NULL;
    }
    return c;
}

int conference_join(conference_t *c, signaling_client_t *signaling) {
    if (!c || !signaling)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&c->send_mutex);
    c->signaling = signaling;
    c->leaving = false;
    c->fallback_requested = false;
    rtc_mutex_unlock(&c->send_mutex);
    return RTC_OK;
}

/* Get signaling config that wires to this conference (call before signaling_connect) */
void conference_get_signaling_callbacks(conference_t *c, signaling_config_t *out_cfg,
                                        const char *server_url, const char *meeting) {
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->server_url = server_url;
    out_cfg->meeting = meeting;
    out_cfg->on_joined = sig_on_joined;
    out_cfg->on_peer_joined = sig_on_peer_joined;
    out_cfg->on_peer_left = sig_on_peer_left;
    out_cfg->on_offer = sig_on_offer;
    out_cfg->on_answer = sig_on_answer;
    out_cfg->on_candidate = sig_on_candidate;
    out_cfg->on_error = sig_on_error;
    out_cfg->on_mode = sig_on_mode;
    out_cfg->on_disconnected = sig_on_disconnected;
    out_cfg->user_data = c;
    out_cfg->protocol_version = SIG_PROTOCOL_VERSION;
    out_cfg->display_name = c->cfg.display_name;
    out_cfg->can_mesh = true;
    out_cfg->can_sfu = true;
    out_cfg->can_switch = true;
    out_cfg->on_event = sig_on_event;
}

int conference_add_video_source(conference_t *c, const char *label, int width, int height, int fps,
                                int bitrate_kbps) {
    if (c->source_count >= CONF_MAX_SOURCES)
        return RTC_ERR_NOMEM;

    if (width <= 0)
        width = c->cfg.video_width;
    if (height <= 0)
        height = c->cfg.video_height;
    if (fps <= 0)
        fps = c->cfg.video_fps;
    if (bitrate_kbps <= 0)
        bitrate_kbps = c->cfg.video_bitrate_kbps;

    conf_source_t *s = &c->sources[c->source_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, CONF_LABEL_SIZE, "%s", label);
    s->is_video = true;
    s->active = true;

    /* Delegate encoding to the shared pipeline */
    s->stream = media_pipeline_add_send_stream(c->pipeline, label, true, c->cfg.video_codec, width,
                                               height, fps, bitrate_kbps);
    if (!s->stream) {
        s->active = false;
        return RTC_ERR_GENERIC;
    }
    rtc_str_map_set(&c->source_index, s->label, s);
    c->source_count++;

    RTC_LOG_INFO("Conference: added video source \"%s\" (%dx%d, %dfps, %dkbps)", label, width,
                 height, fps, bitrate_kbps);
    return RTC_OK;
}

int conference_add_audio_source(conference_t *c, const char *label, int sample_rate, int channels,
                                int bitrate_bps) {
    if (c->source_count >= CONF_MAX_SOURCES)
        return RTC_ERR_NOMEM;

    if (sample_rate <= 0)
        sample_rate = c->cfg.audio_sample_rate;
    if (channels <= 0)
        channels = c->cfg.audio_channels;
    if (bitrate_bps <= 0)
        bitrate_bps = c->cfg.audio_bitrate_bps;

    conf_source_t *s = &c->sources[c->source_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, CONF_LABEL_SIZE, "%s", label);
    s->is_video = false;
    s->active = true;

    /* Delegate encoding to the shared pipeline */
    s->stream = media_pipeline_add_send_stream(c->pipeline, label, false, c->cfg.audio_codec, 0, 0,
                                               0, bitrate_bps);
    if (!s->stream) {
        s->active = false;
        return RTC_ERR_GENERIC;
    }
    rtc_str_map_set(&c->source_index, s->label, s);
    c->source_count++;

    RTC_LOG_INFO("Conference: added audio source \"%s\" (%dHz, %dch, %dbps)", label, sample_rate,
                 channels, bitrate_bps);
    return RTC_OK;
}

static int pin_media_peers(conference_t *c, conf_peer_t **pinned) {
    int count = 0;
    rtc_mutex_lock(&c->send_mutex);
    bool registered = register_connected_peers(c);
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        conf_peer_t *peer = &c->peers[i];
        if (peer->active && (peer->video_sender || peer->audio_sender) &&
            (!peer->sfu || strcmp(peer->peer_id, "sfu") == 0)) {
            peer->in_flight_sends++;
            pinned[count++] = peer;
        }
    }
    rtc_mutex_unlock(&c->send_mutex);
    if (!registered)
        report_error(c, "could not register connected media sender");
    return count;
}

static void unpin_peers(conference_t *c, conf_peer_t **pinned, int count) {
    rtc_mutex_lock(&c->send_mutex);
    for (int i = 0; i < count; i++)
        pinned[i]->in_flight_sends--;
    rtc_cond_broadcast(&c->send_cond);
    rtc_mutex_unlock(&c->send_mutex);
}

int conference_push_video(conference_t *c, const char *label, const video_frame_t *frame) {
    conf_source_t *s = find_source(c, label);
    if (!s || !s->active || !s->is_video || s->muted || !s->stream)
        return RTC_ERR_INVALID;
    if (!c->pipeline)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&c->pipeline_mutex);
    conf_peer_t *pinned[CONF_MAX_PEERS];
    int count = pin_media_peers(c, pinned);
    int rc = media_pipeline_push_video(c->pipeline, s->stream, frame);
    unpin_peers(c, pinned, count);
    rtc_mutex_unlock(&c->pipeline_mutex);
    return rc;
}

int conference_push_audio(conference_t *c, const char *label, const audio_frame_t *audio) {
    conf_source_t *s = find_source(c, label);
    if (!s || !s->active || s->is_video || s->muted || !s->stream)
        return RTC_ERR_INVALID;
    if (!c->pipeline)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&c->pipeline_mutex);
    conf_peer_t *pinned[CONF_MAX_PEERS];
    int count = pin_media_peers(c, pinned);
    int rc = media_pipeline_push_audio(c->pipeline, s->stream, audio);
    unpin_peers(c, pinned, count);
    rtc_mutex_unlock(&c->pipeline_mutex);
    return rc;
}

void conference_mute_source(conference_t *c, const char *label, bool mute) {
    conf_source_t *s = find_source(c, label);
    if (s)
        s->muted = mute;
}

void conference_remove_source(conference_t *c, const char *label) {
    conf_source_t *s = find_source(c, label);
    if (!s)
        return;
    /* Pipeline owns the encoder — it will be cleaned up on pipeline destroy.
     * For now, mark the source inactive so push calls are rejected. */
    s->active = false;
}

int conference_send_message(conference_t *c, const char *peer_id, const uint8_t *data, size_t len) {
    if (!c || !peer_id || (!data && len))
        return RTC_ERR_INVALID;
    uint8_t *message = NULL;
    if (c->v2 || c->sfu_mode) {
        size_t header = c->v2 ? SIG_DC_V2_HEADER_SIZE : SIG_DC_HEADER_SIZE;
        if (strlen(peer_id) >= SIG_MAX_PEER_ID || len > 65536 - header)
            return RTC_ERR_INVALID;
        message = calloc(1, header + len);
        if (!message)
            return RTC_ERR_NOMEM;
    }
    rtc_mutex_lock(&c->send_mutex);
    conf_peer_t *p = find_peer(c, c->sfu_mode ? "sfu" : peer_id);
    if (c->leaving || !p || !p->active || !p->dc || !p->dc_open) {
        rtc_mutex_unlock(&c->send_mutex);
        free(message);
        return RTC_ERR_INVALID;
    }
    if (c->v2) {
        if (!c->next_message_id ||
            sig_dc_write(message, SIG_DC_V2_HEADER_SIZE + len, &len,
                         c->sfu_mode ? peer_id : c->my_id, c->next_message_id++, data, len) != 0) {
            rtc_mutex_unlock(&c->send_mutex);
            free(message);
            return RTC_ERR_INVALID;
        }
        data = message;
    } else if (c->sfu_mode) {
        memcpy(message, "MRTC", 4);
        memcpy(message + 4, peer_id, strlen(peer_id));
        if (len)
            memcpy(message + SIG_DC_HEADER_SIZE, data, len);
        data = message;
        len += SIG_DC_HEADER_SIZE;
    }
    p->in_flight_sends++;
    rtc_data_channel_t *channel = p->dc;
    rtc_mutex_unlock(&c->send_mutex);
    int rc = rtc_data_channel_send(channel, data, len);
    rtc_mutex_lock(&c->send_mutex);
    p->in_flight_sends--;
    rtc_cond_broadcast(&c->send_cond);
    rtc_mutex_unlock(&c->send_mutex);
    free(message);
    return rc;
}

int conference_broadcast_message(conference_t *c, const uint8_t *data, size_t len) {
    if (!c || (!data && len))
        return RTC_ERR_INVALID;
    if (!c->v2 && c->sfu_mode)
        return conference_send_message(c, "*", data, len);
    char peers[CONF_MAX_PEERS][CONF_LABEL_SIZE];
    int count = 0;
    rtc_mutex_lock(&c->send_mutex);
    if (c->v2) {
        for (int i = 0; i < c->participant_count; i++)
            snprintf(peers[count++], CONF_LABEL_SIZE, "%s", c->participants[i].id);
    } else {
        for (int i = 0; i < CONF_MAX_PEERS; i++)
            if (c->peers[i].active && c->peers[i].dc_open && c->peers[i].epoch == c->active_epoch)
                snprintf(peers[count++], CONF_LABEL_SIZE, "%s", c->peers[i].peer_id);
    }
    rtc_mutex_unlock(&c->send_mutex);
    int result = RTC_OK;
    for (int i = 0; i < count; i++)
        if (conference_send_message(c, peers[i], data, len) != RTC_OK)
            result = RTC_ERR_GENERIC;
    return result;
}

int conference_get_peer_count(conference_t *c) {
    int n = 0;
    rtc_mutex_lock(&c->send_mutex);
    if (c->v2) {
        n = c->participant_count;
        rtc_mutex_unlock(&c->send_mutex);
        return n;
    }
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active && (!c->sfu_mode || strcmp(c->peers[i].peer_id, "sfu") != 0))
            n++;
    rtc_mutex_unlock(&c->send_mutex);
    return n;
}

int conference_get_peer_name(conference_t *c, const char *peer_id, char *out, size_t size) {
    if (!c || !peer_id || !out || !size)
        return RTC_ERR_INVALID;
    int rc = RTC_ERR_INVALID;
    rtc_mutex_lock(&c->send_mutex);
    for (int i = 0; i < c->participant_count; i++) {
        if (strcmp(c->participants[i].id, peer_id) != 0)
            continue;
        const char *name = c->participants[i].name;
        if (strlen(name) < size) {
            memcpy(out, name, strlen(name) + 1);
            rc = RTC_OK;
        }
        break;
    }
    rtc_mutex_unlock(&c->send_mutex);
    return rc;
}

int conference_get_source_stats(conference_t *c, const char *peer_id,
                                conference_source_stats_t *out) {
    if (!c || !peer_id || !out)
        return RTC_ERR_INVALID;
    int rc = RTC_ERR_INVALID;
    rtc_mutex_lock(&c->send_mutex);
    for (int i = 0; i < c->participant_count; i++)
        if (strcmp(c->participants[i].id, peer_id) == 0) {
            *out = c->participants[i].stats;
            rc = RTC_OK;
            break;
        }
    rtc_mutex_unlock(&c->send_mutex);
    return rc;
}

void conference_leave(conference_t *c) {
    c->leaving = true;
    if (c->signaling) {
        signaling_leave(c->signaling);
        signaling_disconnect(c->signaling);
        rtc_mutex_lock(&c->send_mutex);
        c->signaling = NULL;
        rtc_mutex_unlock(&c->send_mutex);
    }
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active)
            destroy_peer(c, &c->peers[i]);
    rtc_mutex_lock(&c->send_mutex);
    c->participant_count = 0;
    c->pending_epoch = 0;
    c->active_epoch = 0;
    c->ready_sent = false;
    rtc_mutex_unlock(&c->send_mutex);
}

void conference_destroy(conference_t *c) {
    if (!c)
        return;
    conference_leave(c);
    c->stop_topology = true;
    rtc_mutex_lock(&c->send_mutex);
    rtc_cond_broadcast(&c->send_cond);
    rtc_mutex_unlock(&c->send_mutex);
    rtc_thread_join(&c->topology_thread);
    /* Destroy shared pipeline (owns all encoders/decoders) */
    if (c->pipeline)
        media_pipeline_destroy(c->pipeline);
    rtc_str_map_free(&c->source_index);
    rtc_str_map_free(&c->peer_index);
    rtc_cond_destroy(&c->send_cond);
    rtc_mutex_destroy(&c->pipeline_mutex);
    rtc_mutex_destroy(&c->render_mutex);
    rtc_mutex_destroy(&c->send_mutex);
    free(c);
}
