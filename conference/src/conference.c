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

#define CONF_MAX_PEERS   9 /* Eight participants plus the SFU publishing session. */
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
    rtc_peer_connection_t *pc;
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

struct conference {
    conference_config_t cfg;
    signaling_client_t *signaling;
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
};

static void report_error(conference_t *c, const char *message) {
    RTC_LOG_ERR("Conference: %s", message);
    if (c->cfg.callbacks.on_error)
        c->cfg.callbacks.on_error(message, c->cfg.user_data);
}

static conf_source_t *find_source(conference_t *c, const char *label) {
    return (conf_source_t *)rtc_str_map_get(&c->source_index, label);
}

static conf_peer_t *find_peer(conference_t *c, const char *peer_id) {
    return (conf_peer_t *)rtc_str_map_get(&c->peer_index, peer_id);
}

static conf_peer_t *alloc_peer(conference_t *c, const char *peer_id) {
    rtc_mutex_lock(&c->send_mutex);
    /* Check for duplicate */
    conf_peer_t *existing = find_peer(c, peer_id);
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
    p->active = true;
    p->conf = c;
    if (rtc_str_map_set(&c->peer_index, p->peer_id, p) != RTC_OK) {
        p->active = false;
        rtc_mutex_destroy(&p->candidate_mutex);
        rtc_mutex_unlock(&c->send_mutex);
        return NULL;
    }
    c->peer_count++;
    rtc_mutex_unlock(&c->send_mutex);
    return p;
}

static void destroy_peer(conference_t *c, conf_peer_t *p) {
    /* Unregister from pipeline fan-out */
    rtc_mutex_lock(&c->send_mutex);
    if (!p->active) {
        rtc_mutex_unlock(&c->send_mutex);
        return;
    }
    rtc_str_map_remove(&c->peer_index, p->peer_id);
    p->active = false;
    p->retiring = true;
    p->dc_open = false;
    while (p->in_flight_sends)
        rtc_cond_wait_timeout(&c->send_cond, &c->send_mutex, 1000);
    rtc_mutex_unlock(&c->send_mutex);
    rtc_mutex_lock(&c->pipeline_mutex);
    if (c->pipeline)
        media_pipeline_remove_send_peer(c->pipeline, p->peer_id);
    rtc_mutex_unlock(&c->pipeline_mutex);
    if (p->pc) {
        rtc_peer_connection_close(p->pc);
        rtc_peer_connection_destroy(p->pc);
    }
    /* Closing the connection first drains in-flight receive callbacks. */
    rtc_mutex_lock(&c->pipeline_mutex);
    if (c->pipeline)
        media_pipeline_remove_peer(c->pipeline, p->peer_id);
    rtc_mutex_unlock(&c->pipeline_mutex);
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
    conference_t *c = (conference_t *)user;
    if (c->cfg.callbacks.on_remote_video)
        c->cfg.callbacks.on_remote_video(peer_id, label, frame, c->cfg.user_data);
}

static void pipeline_on_audio(const char *peer_id, const char *label, const audio_frame_t *audio,
                              void *user) {
    conference_t *c = (conference_t *)user;
    if (c->cfg.callbacks.on_remote_audio)
        c->cfg.callbacks.on_remote_audio(peer_id, label, audio, c->cfg.user_data);
}

static void on_conn_state(rtc_connection_state_t state, void *user) {
    conf_peer_t *p = (conf_peer_t *)user;
    if (state == RTC_CONNECTION_CONNECTED) {
        if (!p->active)
            return;
        atomic_store(&p->connected_pending, true);
        if (p->conf->cfg.callbacks.on_connected)
            p->conf->cfg.callbacks.on_connected(p->peer_id, p->conf->cfg.user_data);
    }
}

static bool register_connected_peers(conference_t *c) {
    /* Registration touches encoders, so callbacks only publish pending state. */
    bool registered = true;
    for (int i = 0; i < CONF_MAX_PEERS; i++) {
        conf_peer_t *p = &c->peers[i];
        if (p->active && atomic_exchange(&p->connected_pending, false) &&
            (!c->sfu_mode || strcmp(p->peer_id, "sfu") == 0)) {
            if (media_pipeline_add_send_peer(c->pipeline, p->peer_id, p->video_sender,
                                             p->audio_sender) != RTC_OK)
                registered = false;
        }
    }
    return registered;
}

static void on_bitrate(uint32_t bitrate_bps, void *user) {
    conf_peer_t *p = user;
    if (p->active && p->conf->pipeline)
        media_pipeline_set_peer_bitrate(p->conf->pipeline, p->peer_id, bitrate_bps);
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
    if (signaling_send_candidate(p->conf->signaling, p->peer_id,
                                 candidate ? candidate->candidate : "") != RTC_OK)
        report_error(p->conf, "failed to forward local ICE candidate");
    rtc_mutex_unlock(&p->candidate_mutex);
}

static void flush_candidates(conf_peer_t *p) {
    rtc_mutex_lock(&p->candidate_mutex);
    p->description_sent = true;
    for (int i = 0; i < p->pending_candidate_count; i++)
        if (signaling_send_candidate(p->conf->signaling, p->peer_id,
                                     p->pending_candidates[i].candidate) != RTC_OK)
            report_error(p->conf, "failed to forward queued ICE candidate");
    p->pending_candidate_count = 0;
    if (p->gathering_done)
        if (signaling_send_candidate(p->conf->signaling, p->peer_id, "") != RTC_OK)
            report_error(p->conf, "failed to forward end-of-candidates");
    rtc_mutex_unlock(&p->candidate_mutex);
}

static void request_video_keyframe(void *user) {
    rtc_rtp_receiver_request_keyframe(user);
}

static void on_recv_video_rtp(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                              uint32_t ssrc, bool marker, void *user) {
    conf_peer_t *cp = (conf_peer_t *)user;
    if (!cp->conf->pipeline || !payload || len == 0)
        return;
    /* Lazily register recv stream on first packet, then cache the handle */
    if (!cp->recv_video) {
        const char *vc = cp->conf->cfg.video_codec ? cp->conf->cfg.video_codec : "VP8";
        cp->recv_video = media_pipeline_add_recv_stream(cp->conf->pipeline, cp->peer_id, "camera",
                                                        ssrc, true, vc);
        if (!cp->recv_video)
            return;
        if (media_pipeline_set_recv_feedback(cp->conf->pipeline, cp->recv_video,
                                             request_video_keyframe, cp->video_receiver) != RTC_OK)
            report_error(cp->conf, "could not install receive keyframe feedback");
    }
    media_pipeline_recv_rtp(cp->conf->pipeline, cp->recv_video, payload, (int)len, seq, timestamp,
                            marker);
}

static void on_recv_audio_rtp(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                              uint32_t ssrc, bool marker, void *user) {
    conf_peer_t *cp = (conf_peer_t *)user;
    (void)marker;
    if (!cp->conf->pipeline || !payload || len == 0)
        return;
    /* Lazily register recv stream on first packet, then cache the handle */
    if (!cp->recv_audio) {
        const char *ac = cp->conf->cfg.audio_codec ? cp->conf->cfg.audio_codec : "opus";
        cp->recv_audio =
            media_pipeline_add_recv_stream(cp->conf->pipeline, cp->peer_id, "mic", ssrc, false, ac);
        if (!cp->recv_audio)
            return;
    }
    media_pipeline_recv_rtp(cp->conf->pipeline, cp->recv_audio, payload, (int)len, seq, timestamp,
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
    if (p->conf->sfu_mode && len >= SIG_DC_HEADER_SIZE && memcmp(data, "MRTC", 4) == 0) {
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

static rtc_peer_connection_t *create_pc(conference_t *c, conf_peer_t *cp) {
    if (cp->pc)
        return cp->pc;
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

    bool send_video = !c->sfu_mode;
    bool send_audio = !c->sfu_mode;
    if (c->sfu_mode && strcmp(cp->peer_id, "sfu") == 0) {
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
    if (send_video)
        cp->video_sender = rtc_peer_connection_add_track(pc, RTC_KIND_VIDEO, &vp8);

    /* Add audio track (Opus) */
    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    if (send_audio)
        cp->audio_sender = rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);

    cp->pc = pc;
    return pc;
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
    int rc = rtc_peer_connection_create_offer(pc, &offer);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_peer_connection_set_local_desc(pc, &offer);
    if (rc != RTC_OK)
        return rc;

    rc = signaling_send_offer(c->signaling, cp->peer_id, offer.sdp);
    if (rc != RTC_OK)
        return rc;
    flush_candidates(cp);
    RTC_LOG_INFO("Conference: sent offer to %s", cp->peer_id);
    return 0;
}

static int handle_offer(conference_t *c, const char *from, const char *sdp) {
    conf_peer_t *cp = find_peer(c, from);
    if (!cp) {
        cp = alloc_peer(c, from);
        if (!cp)
            return -1;
    }

    rtc_peer_connection_t *pc = cp->pc ? cp->pc : create_pc(c, cp);
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

    rtc_desc_t answer;
    rc = rtc_peer_connection_create_answer(pc, &answer);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_peer_connection_set_local_desc(pc, &answer);
    if (rc != RTC_OK)
        return rc;

    rc = signaling_send_answer(c->signaling, from, answer.sdp);
    if (rc != RTC_OK)
        return rc;
    flush_candidates(cp);
    RTC_LOG_INFO("Conference: sent answer to %s", from);
    return 0;
}

static int handle_answer(conference_t *c, const char *from, const char *sdp) {
    conf_peer_t *cp = find_peer(c, from);
    if (!cp || !cp->pc)
        return -1;

    rtc_desc_t answer_desc;
    memset(&answer_desc, 0, sizeof(answer_desc));
    answer_desc.type = RTC_SDP_ANSWER;
    size_t sdp_len = strlen(sdp);
    if (sdp_len >= SDP_MAX_SIZE)
        return RTC_ERR_SDP;
    memcpy(answer_desc.sdp, sdp, sdp_len);
    answer_desc.sdp_len = sdp_len;

    return rtc_peer_connection_set_remote_desc(cp->pc, &answer_desc);
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
    c->leaving = true;
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
    media_renderer_t renderer = {
        .on_video_frame = pipeline_on_video,
        .on_audio_samples = pipeline_on_audio,
        .user = c,
    };
    media_pipeline_config_t mp_cfg = {
        .default_video_codec = c->cfg.video_codec,
        .default_audio_codec = c->cfg.audio_codec,
        .renderer = &renderer,
    };
    c->pipeline = media_pipeline_create(&mp_cfg);
    if (!c->pipeline) {
        rtc_str_map_free(&c->source_index);
        rtc_str_map_free(&c->peer_index);
        rtc_mutex_destroy(&c->send_mutex);
        rtc_cond_destroy(&c->send_cond);
        rtc_mutex_destroy(&c->pipeline_mutex);
        free(c);
        return NULL;
    }
    return c;
}

int conference_join(conference_t *c, signaling_client_t *signaling) {
    if (!c || !signaling)
        return RTC_ERR_INVALID;
    c->signaling = signaling;
    c->leaving = false;
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
            (!c->sfu_mode || strcmp(peer->peer_id, "sfu") == 0)) {
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
    if (c->sfu_mode) {
        if (strlen(peer_id) >= SIG_MAX_PEER_ID || len > 65536 - SIG_DC_HEADER_SIZE)
            return RTC_ERR_INVALID;
        message = calloc(1, SIG_DC_HEADER_SIZE + len);
        if (!message)
            return RTC_ERR_NOMEM;
        memcpy(message, "MRTC", 4);
        memcpy(message + 4, peer_id, strlen(peer_id));
        if (len)
            memcpy(message + SIG_DC_HEADER_SIZE, data, len);
        data = message;
        len += SIG_DC_HEADER_SIZE;
    }
    rtc_mutex_lock(&c->send_mutex);
    conf_peer_t *p = find_peer(c, c->sfu_mode ? "sfu" : peer_id);
    if (!p || !p->active || !p->dc || !p->dc_open) {
        rtc_mutex_unlock(&c->send_mutex);
        free(message);
        return RTC_ERR_INVALID;
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
    if (c->sfu_mode)
        return conference_send_message(c, "*", data, len);
    char peers[CONF_MAX_PEERS][CONF_LABEL_SIZE];
    int count = 0;
    rtc_mutex_lock(&c->send_mutex);
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active && c->peers[i].dc_open)
            snprintf(peers[count++], CONF_LABEL_SIZE, "%s", c->peers[i].peer_id);
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
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active && (!c->sfu_mode || strcmp(c->peers[i].peer_id, "sfu") != 0))
            n++;
    rtc_mutex_unlock(&c->send_mutex);
    return n;
}

void conference_leave(conference_t *c) {
    c->leaving = true;
    if (c->signaling) {
        signaling_leave(c->signaling);
        signaling_disconnect(c->signaling);
        c->signaling = NULL;
    }
    for (int i = 0; i < CONF_MAX_PEERS; i++)
        if (c->peers[i].active)
            destroy_peer(c, &c->peers[i]);
}

void conference_destroy(conference_t *c) {
    if (!c)
        return;
    conference_leave(c);
    /* Destroy shared pipeline (owns all encoders/decoders) */
    if (c->pipeline)
        media_pipeline_destroy(c->pipeline);
    rtc_str_map_free(&c->source_index);
    rtc_str_map_free(&c->peer_index);
    rtc_cond_destroy(&c->send_cond);
    rtc_mutex_destroy(&c->pipeline_mutex);
    rtc_mutex_destroy(&c->send_mutex);
    free(c);
}
