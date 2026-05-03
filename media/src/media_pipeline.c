/*
 * media_pipeline.c - Multi-stream media pipeline.
 *
 * Each send/recv stream has its own encoder/decoder, packetizer,
 * and jitter buffer. Streams are identified by label (send) or SSRC (recv).
 */
#include "media/media_pipeline.h"
#include "vp8_packetizer.h"
#include "rate_control.h"
#include "jitter_buffer.h"
#include <rtc/rtc_track.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Per-stream state ---- */
/* media_send_stream_t is the public opaque handle (for send streams).
   media_recv_stream_t is the public opaque handle (for recv streams).
   Both share the same underlying struct layout. */
typedef struct media_send_stream {
    char label[MP_LABEL_SIZE];
    char peer_id[MP_LABEL_SIZE]; /* recv only */
    uint32_t ssrc;
    bool is_video;
    bool active;

    /* Encode side (send streams) */
    video_encoder_t video_enc;
    audio_encoder_t audio_enc;
    bool has_encoder;
    uint8_t audio_out[4000];

    /* Decode side (recv streams) */
    video_decoder_t video_dec;
    audio_decoder_t audio_dec;
    rtc_vp8_depacketizer_t vp8_depack;
    jitter_buffer_t *jb;
    bool has_decoder;
} media_stream_t;

/* media_recv_stream_t is the same layout, defined for opaque pointer compatibility.
   We cast between media_stream_t* and media_recv_stream_t* internally. */
struct media_recv_stream {
    char _opaque;
};

/* ---- Per-peer send target ---- */
typedef struct {
    char peer_id[MP_LABEL_SIZE];
    rtc_rtp_sender_t *video_sender;
    rtc_rtp_sender_t *audio_sender;
    bool active;
} mp_send_peer_t;

struct media_pipeline {
    media_send_stream_t send[MP_MAX_SEND_STREAMS];
    int send_count;

    media_stream_t recv[MP_MAX_RECV_STREAMS];
    int recv_count;

    rate_controller_t *rate_ctrl; /* TODO: replace with rtc-layer BWE */
    mp_send_peer_t send_peers[MP_MAX_SEND_PEERS];
    int send_peer_count;
    media_renderer_t renderer;

    char default_video_codec[16];
    char default_audio_codec[16];
};

/* ---- Helpers ---- */

static media_stream_t *find_recv_by_ssrc(media_pipeline_t *p, uint32_t ssrc) {
    for (int i = 0; i < p->recv_count; i++)
        if (p->recv[i].active && p->recv[i].ssrc == ssrc)
            return &p->recv[i];
    return NULL;
}

static void stream_destroy(media_stream_t *s) {
    if (!s->active)
        return;
    if (s->has_encoder) {
        if (s->is_video)
            video_encoder_destroy(&s->video_enc);
        else
            audio_encoder_destroy(&s->audio_enc);
    }
    if (s->has_decoder) {
        if (s->is_video)
            video_decoder_destroy(&s->video_dec);
        else
            audio_decoder_destroy(&s->audio_dec);
    }
    if (s->jb)
        jitter_buffer_destroy(s->jb);
    s->active = false;
}

/* ---- Create / Destroy ---- */

media_pipeline_t *media_pipeline_create(const media_pipeline_config_t *cfg) {
    media_pipeline_t *p = (media_pipeline_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    if (cfg->renderer)
        p->renderer = *cfg->renderer;
    if (cfg->default_video_codec)
        snprintf(p->default_video_codec, sizeof(p->default_video_codec), "%s",
                 cfg->default_video_codec);
    if (cfg->default_audio_codec)
        snprintf(p->default_audio_codec, sizeof(p->default_audio_codec), "%s",
                 cfg->default_audio_codec);

    /* Shared rate controller */
    rate_control_config_t rc_cfg = {
        .target_bitrate_kbps = 500,
        .min_bitrate_kbps = 100,
        .max_bitrate_kbps = 2500,
    };
    p->rate_ctrl = rate_control_create(&rc_cfg);

    return p;
}

void media_pipeline_destroy(media_pipeline_t *p) {
    if (!p)
        return;
    for (int i = 0; i < p->send_count; i++)
        stream_destroy(&p->send[i]);
    for (int i = 0; i < p->recv_count; i++)
        stream_destroy(&p->recv[i]);
    if (p->rate_ctrl)
        rate_control_destroy(p->rate_ctrl);
    free(p);
}

int media_pipeline_add_send_peer(media_pipeline_t *p, const char *peer_id,
                                 rtc_rtp_sender_t *video_sender, rtc_rtp_sender_t *audio_sender) {
    if (!p || !peer_id)
        return RTC_ERR_INVALID;
    if (p->send_peer_count >= MP_MAX_SEND_PEERS)
        return RTC_ERR_NOMEM;

    /* Check for duplicate */
    for (int i = 0; i < p->send_peer_count; i++)
        if (p->send_peers[i].active && strcmp(p->send_peers[i].peer_id, peer_id) == 0)
            return RTC_OK;

    mp_send_peer_t *sp = &p->send_peers[p->send_peer_count++];
    memset(sp, 0, sizeof(*sp));
    snprintf(sp->peer_id, MP_LABEL_SIZE, "%s", peer_id);
    sp->video_sender = video_sender;
    sp->audio_sender = audio_sender;
    sp->active = true;

    RTC_LOG_INFO("Pipeline: added send peer \"%s\"", peer_id);
    return RTC_OK;
}

void media_pipeline_remove_send_peer(media_pipeline_t *p, const char *peer_id) {
    if (!p || !peer_id)
        return;
    for (int i = 0; i < p->send_peer_count; i++) {
        if (p->send_peers[i].active && strcmp(p->send_peers[i].peer_id, peer_id) == 0) {
            p->send_peers[i].active = false;
            RTC_LOG_INFO("Pipeline: removed send peer \"%s\"", peer_id);
            return;
        }
    }
}

/* ---- Add send stream ---- */

media_send_stream_t *media_pipeline_add_send_stream(media_pipeline_t *p, const char *label,
                                                    bool is_video, const char *codec, int width,
                                                    int height, int fps, int bitrate) {
    if (!p || p->send_count >= MP_MAX_SEND_STREAMS)
        return NULL;

    media_send_stream_t *s = &p->send[p->send_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, MP_LABEL_SIZE, "%s", label);
    s->is_video = is_video;
    s->active = true;

    if (is_video) {
        if (video_encoder_create(&s->video_enc, codec, width, height, bitrate,
                                 fps > 0 ? fps : 30) != RTC_OK) {
            s->active = false;
            return NULL;
        }
        /* Generate SSRC for video */
        uint8_t rnd[4];
        rtc_random_bytes(rnd, 4);
        s->ssrc =
            ((uint32_t)rnd[0] << 24) | ((uint32_t)rnd[1] << 16) | ((uint32_t)rnd[2] << 8) | rnd[3];
        s->has_encoder = true;
    } else {
        if (audio_encoder_create(&s->audio_enc, codec, 48000, 1, bitrate > 0 ? bitrate : 32000) !=
            RTC_OK) {
            s->active = false;
            return NULL;
        }
        /* Generate SSRC for audio */
        uint8_t rnd[4];
        rtc_random_bytes(rnd, 4);
        s->ssrc =
            ((uint32_t)rnd[0] << 24) | ((uint32_t)rnd[1] << 16) | ((uint32_t)rnd[2] << 8) | rnd[3];
        s->has_encoder = true;
    }

    p->send_count++;
    RTC_LOG_INFO("Pipeline: added send stream \"%s\" (ssrc=0x%08X, video=%d)", label, s->ssrc,
                 is_video);
    return s;
}

uint32_t media_send_stream_get_ssrc(const media_send_stream_t *s) {
    return s ? s->ssrc : 0;
}

/* ---- Add recv stream ---- */

media_recv_stream_t *media_pipeline_add_recv_stream(media_pipeline_t *p, const char *peer_id,
                                                    const char *label, uint32_t ssrc, bool is_video,
                                                    const char *codec) {
    if (!p || p->recv_count >= MP_MAX_RECV_STREAMS)
        return NULL;

    /* Check for duplicate SSRC — return existing handle */
    media_stream_t *existing = find_recv_by_ssrc(p, ssrc);
    if (existing)
        return (media_recv_stream_t *)existing;

    media_stream_t *s = &p->recv[p->recv_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, MP_LABEL_SIZE, "%s", label);
    snprintf(s->peer_id, MP_LABEL_SIZE, "%s", peer_id);
    s->ssrc = ssrc;
    s->is_video = is_video;
    s->active = true;

    if (is_video) {
        if (video_decoder_create(&s->video_dec, codec) != RTC_OK) {
            s->active = false;
            return NULL;
        }
        rtc_vp8_depacketizer_init(&s->vp8_depack);
        jitter_buffer_config_t jb_cfg = {.target_delay_ms = 50, .max_delay_ms = 500};
        s->jb = jitter_buffer_create(&jb_cfg);
        s->has_decoder = true;
    } else {
        if (audio_decoder_create(&s->audio_dec, codec, 48000, 1) != RTC_OK) {
            s->active = false;
            return NULL;
        }
        jitter_buffer_config_t jb_cfg = {.target_delay_ms = 40, .max_delay_ms = 300};
        s->jb = jitter_buffer_create(&jb_cfg);
        s->has_decoder = true;
    }

    p->recv_count++;
    RTC_LOG_INFO("Pipeline: added recv stream \"%s\" from %s (ssrc=0x%08X, video=%d)", label,
                 peer_id, ssrc, is_video);
    return (media_recv_stream_t *)s;
}

void media_pipeline_remove_recv_stream(media_pipeline_t *p, uint32_t ssrc) {
    for (int i = 0; i < p->recv_count; i++) {
        if (p->recv[i].active && p->recv[i].ssrc == ssrc) {
            stream_destroy(&p->recv[i]);
            return;
        }
    }
}

void media_pipeline_remove_peer(media_pipeline_t *p, const char *peer_id) {
    for (int i = 0; i < p->recv_count; i++) {
        if (p->recv[i].active && strcmp(p->recv[i].peer_id, peer_id) == 0)
            stream_destroy(&p->recv[i]);
    }
}

/* ---- Send path ---- */

static bool has_send_targets(media_pipeline_t *p) {
    for (int i = 0; i < p->send_peer_count; i++)
        if (p->send_peers[i].active)
            return true;
    return false;
}

static int push_video_to_stream(media_pipeline_t *p, media_send_stream_t *s,
                                const video_frame_t *frame) {
    if (!s->has_encoder || !has_send_targets(p))
        return RTC_ERR_INVALID;

    /* Rate control */
    if (p->rate_ctrl) {
        int br = rate_control_get_bitrate(p->rate_ctrl);
        video_encoder_set_bitrate(&s->video_enc, br);
        if (rate_control_should_keyframe(p->rate_ctrl))
            video_encoder_request_keyframe(&s->video_enc);
    }

    /* Encode */
    video_packet_t pkt;
    int rc = video_encode(&s->video_enc, frame, &pkt);
    if (rc != RTC_OK)
        return rc;

    /* Fragment into VP8 payloads (no RTP header) */
    rtc_vp8_payload_t payloads[64];
    int payload_count = 0;
    rc =
        rtc_vp8_packetize(pkt.data, (size_t)pkt.len, pkt.is_keyframe, payloads, 64, &payload_count);
    if (rc != RTC_OK)
        return rc;

    /* Fan out to all registered send peers */
    uint32_t samples = 90000 / 30; /* 3000 ticks per frame at 30fps */
    for (int peer = 0; peer < p->send_peer_count; peer++) {
        mp_send_peer_t *sp = &p->send_peers[peer];
        if (!sp->active || !sp->video_sender)
            continue;
        for (int i = 0; i < payload_count; i++)
            rtc_rtp_sender_send(sp->video_sender, payloads[i].data, payloads[i].len,
                                i == 0 ? samples : 0, payloads[i].marker);
    }

    return RTC_OK;
}

int media_pipeline_push_video(media_pipeline_t *p, media_send_stream_t *stream,
                              const video_frame_t *frame) {
    if (!p || !stream || !stream->is_video)
        return RTC_ERR_INVALID;
    return push_video_to_stream(p, stream, frame);
}

static int push_audio_to_stream(media_pipeline_t *p, media_send_stream_t *s,
                                const audio_frame_t *audio) {
    if (!s->has_encoder || !has_send_targets(p))
        return RTC_ERR_INVALID;

    int out_len = (int)sizeof(s->audio_out);
    int rc = audio_encode(&s->audio_enc, audio, s->audio_out, &out_len);
    if (rc != RTC_OK)
        return rc;

    /* Fan out to all registered send peers */
    for (int peer = 0; peer < p->send_peer_count; peer++) {
        mp_send_peer_t *sp = &p->send_peers[peer];
        if (!sp->active || !sp->audio_sender)
            continue;
        rtc_rtp_sender_send(sp->audio_sender, s->audio_out, (size_t)out_len, 960, false);
    }

    return RTC_OK;
}

int media_pipeline_push_audio(media_pipeline_t *p, media_send_stream_t *stream,
                              const audio_frame_t *audio) {
    if (!p || !stream || stream->is_video)
        return RTC_ERR_INVALID;
    return push_audio_to_stream(p, stream, audio);
}

/* ---- Recv path ---- */

static void recv_video_on_stream(media_pipeline_t *p, media_stream_t *s, const uint8_t *data,
                                 int len, uint16_t seq, uint32_t timestamp, bool marker) {
    (void)seq; /* seq available for future jitter buffer use */

    const uint8_t *frame_data;
    size_t frame_len;
    bool is_key;
    if (rtc_vp8_depacketize(&s->vp8_depack, data, (size_t)len, timestamp, marker, &frame_data,
                            &frame_len, &is_key) == RTC_OK) {
        if (frame_data && frame_len > 0) {
            video_frame_t decoded;
            int rc = video_decode(&s->video_dec, frame_data, (int)frame_len, &decoded);
            if (rc == RTC_OK) {
                RTC_LOG_DBG("Pipeline: decoded %dx%d frame (%zu bytes, key=%d)", decoded.width,
                            decoded.height, frame_len, is_key);
                if (p->renderer.on_video_frame)
                    p->renderer.on_video_frame(s->peer_id, s->label, &decoded, p->renderer.user);
            } else {
                RTC_LOG_WARN("Pipeline: VP8 decode failed (rc=%d, %zu bytes)", rc, frame_len);
            }
        }
    }
}

static void recv_audio_on_stream(media_pipeline_t *p, media_stream_t *s, const uint8_t *data,
                                 int len, uint16_t seq, uint32_t timestamp) {
    jitter_buffer_push(s->jb, data, len, seq, timestamp, false);

    jitter_buffer_packet_t jb_pkt;
    while (jitter_buffer_pop(s->jb, &jb_pkt) == 0) {
        audio_frame_t decoded;
        if (audio_decode(&s->audio_dec, jb_pkt.data, jb_pkt.len, &decoded) == RTC_OK) {
            if (p->renderer.on_audio_samples)
                p->renderer.on_audio_samples(s->peer_id, s->label, &decoded, p->renderer.user);
        }
    }
}

int media_pipeline_recv_rtp(media_pipeline_t *p, media_recv_stream_t *stream, const uint8_t *data,
                            int len, uint16_t seq, uint32_t timestamp, bool marker) {
    if (!p || !stream)
        return RTC_ERR_INVALID;

    media_stream_t *s = (media_stream_t *)stream;
    if (!s->active)
        return RTC_ERR_INVALID;

    if (s->is_video)
        recv_video_on_stream(p, s, data, len, seq, timestamp, marker);
    else
        recv_audio_on_stream(p, s, data, len, seq, timestamp);

    return RTC_OK;
}
