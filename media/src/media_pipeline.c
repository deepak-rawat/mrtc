/*
 * Multi-stream media pipeline.
 *
 * Each send/recv stream has its own encoder/decoder, packetizer,
 * and jitter buffer. Streams are identified by label (send) or SSRC (recv).
 */
#include "media/media_pipeline.h"
#include "media/video_stats.h"
#include "video_dump.h"
#include "video_debug.h"
#include "video_payload.h"
#include "jitter_buffer.h"
#include <rtc/rtc_track.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>

struct media_send_stream {
    char label[MP_LABEL_SIZE];
    uint32_t ssrc;
    bool is_video;
    bool active;
    uint32_t fps;
    uint32_t timestamp_remainder;

    /* Encode side */
    video_encoder_t video_enc;
    audio_encoder_t audio_enc;
    bool has_encoder;
    const video_payload_format_t *video_format;
    uint8_t audio_out[4000];

    /* Debug stats */
    video_send_stats_t send_stats;
};

struct media_recv_stream {
    char label[MP_LABEL_SIZE];
    char peer_id[MP_LABEL_SIZE];
    uint32_t ssrc;
    bool is_video;
    bool active;

    /* Decode side */
    video_decoder_t video_dec;
    audio_decoder_t audio_dec;
    const video_payload_format_t *video_format;
    void *video_depacketizer;
    jitter_buffer_t *jb;
    bool has_decoder;
    bool waiting_keyframe;
    void (*request_keyframe)(void *);
    void *feedback_user;
    uint64_t last_keyframe_request_ms;

    /* Debug stats */
    video_recv_stats_t recv_stats;
};

typedef struct {
    char peer_id[MP_LABEL_SIZE];
    rtc_rtp_sender_t *video_sender;
    rtc_rtp_sender_t *audio_sender;
    bool active;
    _Atomic uint32_t bitrate_bps;
    int64_t budget_bytes;
    uint64_t budget_time_ms;
    uint32_t pending_samples;
} mp_send_peer_t;

struct media_pipeline {
    media_send_stream_t send[MP_MAX_SEND_STREAMS];
    int send_count;

    media_recv_stream_t recv[MP_MAX_RECV_STREAMS];
    int recv_count;

    mp_send_peer_t send_peers[MP_MAX_SEND_PEERS];
    int send_peer_count;
    rtc_mutex_t peer_mutex;
    media_renderer_t renderer;

    char default_video_codec[16];
    char default_audio_codec[16];

    /* Debug state */
    struct {
        video_dump_t *send_dump;
        video_dump_t *recv_dump;
        const video_payload_format_t *send_format;
        const video_payload_format_t *recv_format;
        bool frame_checksum;
        int psnr_interval;
    } dbg;
};

static media_recv_stream_t *find_recv_by_ssrc(media_pipeline_t *p, uint32_t ssrc) {
    for (int i = 0; i < p->recv_count; i++)
        if (p->recv[i].active && p->recv[i].ssrc == ssrc)
            return &p->recv[i];
    return NULL;
}

static void send_stream_destroy(media_send_stream_t *s) {
    if (!s->active)
        return;
    if (s->has_encoder) {
        if (s->is_video)
            video_encoder_destroy(&s->video_enc);
        else
            audio_encoder_destroy(&s->audio_enc);
    }
    s->active = false;
}

static void recv_stream_destroy(media_recv_stream_t *s) {
    if (!s->active)
        return;
    if (s->has_decoder) {
        if (s->is_video)
            video_decoder_destroy(&s->video_dec);
        else
            audio_decoder_destroy(&s->audio_dec);
    }
    if (s->jb)
        jitter_buffer_destroy(s->jb);
    if (s->video_depacketizer)
        s->video_format->destroy_depacketizer(s->video_depacketizer);
    s->active = false;
}

media_pipeline_t *media_pipeline_create(const media_pipeline_config_t *cfg) {
    if (!cfg)
        return NULL;
    media_pipeline_t *p = (media_pipeline_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    if (rtc_mutex_init(&p->peer_mutex) != RTC_OK) {
        free(p);
        return NULL;
    }

    if (cfg->renderer)
        p->renderer = *cfg->renderer;
    snprintf(p->default_video_codec, sizeof(p->default_video_codec), "%s",
             cfg->default_video_codec ? cfg->default_video_codec : "VP8");
    if (cfg->default_audio_codec)
        snprintf(p->default_audio_codec, sizeof(p->default_audio_codec), "%s",
                 cfg->default_audio_codec);

    return p;
}

void media_pipeline_destroy(media_pipeline_t *p) {
    if (!p)
        return;
    for (int i = 0; i < p->send_count; i++)
        send_stream_destroy(&p->send[i]);
    for (int i = 0; i < p->recv_count; i++)
        recv_stream_destroy(&p->recv[i]);
    if (p->dbg.send_dump)
        video_dump_close(p->dbg.send_dump);
    if (p->dbg.recv_dump)
        video_dump_close(p->dbg.recv_dump);
    rtc_mutex_destroy(&p->peer_mutex);
    free(p);
}

int media_pipeline_add_send_peer(media_pipeline_t *p, const char *peer_id,
                                 rtc_rtp_sender_t *video_sender, rtc_rtp_sender_t *audio_sender) {
    if (!p || !peer_id || strlen(peer_id) >= MP_LABEL_SIZE)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&p->peer_mutex);

    /* Check for duplicate */
    for (int i = 0; i < p->send_peer_count; i++)
        if (p->send_peers[i].active && strcmp(p->send_peers[i].peer_id, peer_id) == 0) {
            rtc_mutex_unlock(&p->peer_mutex);
            return RTC_OK;
        }

    int slot = 0;
    while (slot < p->send_peer_count && p->send_peers[slot].active)
        slot++;
    if (slot == MP_MAX_SEND_PEERS) {
        rtc_mutex_unlock(&p->peer_mutex);
        return RTC_ERR_NOMEM;
    }
    if (slot == p->send_peer_count)
        p->send_peer_count++;
    mp_send_peer_t *sp = &p->send_peers[slot];
    memset(sp, 0, sizeof(*sp));
    snprintf(sp->peer_id, MP_LABEL_SIZE, "%s", peer_id);
    sp->video_sender = video_sender;
    sp->audio_sender = audio_sender;
    sp->active = true;
    atomic_init(&sp->bitrate_bps, 0);
    rtc_mutex_unlock(&p->peer_mutex);

    for (int i = 0; i < p->send_count; i++)
        if (p->send[i].active && p->send[i].is_video)
            video_encoder_request_keyframe(&p->send[i].video_enc);

    RTC_LOG_INFO("Pipeline: added send peer \"%s\"", peer_id);
    return RTC_OK;
}

void media_pipeline_remove_send_peer(media_pipeline_t *p, const char *peer_id) {
    if (!p || !peer_id)
        return;
    rtc_mutex_lock(&p->peer_mutex);
    for (int i = 0; i < p->send_peer_count; i++) {
        if (p->send_peers[i].active && strcmp(p->send_peers[i].peer_id, peer_id) == 0) {
            p->send_peers[i].active = false;
            p->send_peers[i].video_sender = NULL;
            p->send_peers[i].audio_sender = NULL;
            rtc_mutex_unlock(&p->peer_mutex);
            RTC_LOG_INFO("Pipeline: removed send peer \"%s\"", peer_id);
            return;
        }
    }
    rtc_mutex_unlock(&p->peer_mutex);
}

int media_pipeline_set_peer_bitrate(media_pipeline_t *p, const char *peer_id,
                                    uint32_t bitrate_bps) {
    if (!p || !peer_id || bitrate_bps > INT_MAX)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&p->peer_mutex);
    for (int i = 0; i < p->send_peer_count; i++) {
        if (p->send_peers[i].active && strcmp(p->send_peers[i].peer_id, peer_id) == 0) {
            atomic_store(&p->send_peers[i].bitrate_bps, bitrate_bps);
            rtc_mutex_unlock(&p->peer_mutex);
            return RTC_OK;
        }
    }
    rtc_mutex_unlock(&p->peer_mutex);
    return RTC_ERR_INVALID;
}

media_send_stream_t *media_pipeline_add_send_stream(media_pipeline_t *p, const char *label,
                                                    bool is_video, const char *codec, int width,
                                                    int height, int fps, int bitrate) {
    if (!p || !label || !codec || p->send_count >= MP_MAX_SEND_STREAMS)
        return NULL;
    const video_payload_format_t *format = is_video ? video_payload_format_find(codec) : NULL;
    if (is_video && (!format || (fps > 0 && (uint32_t)fps > format->clock_rate) ||
                     (p->dbg.send_dump && p->dbg.send_format != format))) {
        RTC_LOG_ERR("Pipeline: invalid video send format, frame rate or dump configuration");
        return NULL;
    }

    media_send_stream_t *s = &p->send[p->send_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, MP_LABEL_SIZE, "%s", label);
    s->is_video = is_video;
    s->video_format = format;
    s->active = true;
    s->fps = (uint32_t)(fps > 0 ? fps : 30);
    video_send_stats_init(&s->send_stats);

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

media_recv_stream_t *media_pipeline_add_recv_stream(media_pipeline_t *p, const char *peer_id,
                                                    const char *label, uint32_t ssrc, bool is_video,
                                                    const char *codec) {
    if (!p || !peer_id || !label || !codec)
        return NULL;

    /* Check for duplicate SSRC — return existing handle */
    media_recv_stream_t *existing = find_recv_by_ssrc(p, ssrc);
    if (existing)
        return existing;
    const video_payload_format_t *format = is_video ? video_payload_format_find(codec) : NULL;
    if (is_video && (!format || (p->dbg.recv_dump && p->dbg.recv_format != format))) {
        RTC_LOG_ERR("Pipeline: invalid video receive format or dump configuration");
        return NULL;
    }

    int slot = 0;
    while (slot < p->recv_count && p->recv[slot].active)
        slot++;
    if (slot == MP_MAX_RECV_STREAMS)
        return NULL;
    media_recv_stream_t *s = &p->recv[slot];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, MP_LABEL_SIZE, "%s", label);
    snprintf(s->peer_id, MP_LABEL_SIZE, "%s", peer_id);
    s->ssrc = ssrc;
    s->is_video = is_video;
    s->video_format = format;
    s->active = true;
    s->waiting_keyframe = is_video;
    video_recv_stats_init(&s->recv_stats);

    if (is_video) {
        if (video_decoder_create(&s->video_dec, codec) != RTC_OK) {
            s->active = false;
            return NULL;
        }
        s->has_decoder = true;
        s->video_depacketizer = format->create_depacketizer();
        if (!s->video_depacketizer) {
            recv_stream_destroy(s);
            return NULL;
        }
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

    if (!s->jb) {
        recv_stream_destroy(s);
        return NULL;
    }
    if (slot == p->recv_count)
        p->recv_count++;
    RTC_LOG_INFO("Pipeline: added recv stream \"%s\" from %s (ssrc=0x%08X, video=%d)", label,
                 peer_id, ssrc, is_video);
    return s;
}

void media_pipeline_remove_recv_stream(media_pipeline_t *p, uint32_t ssrc) {
    if (!p)
        return;
    for (int i = 0; i < p->recv_count; i++) {
        if (p->recv[i].active && p->recv[i].ssrc == ssrc) {
            recv_stream_destroy(&p->recv[i]);
            return;
        }
    }
}

void media_pipeline_remove_peer(media_pipeline_t *p, const char *peer_id) {
    if (!p || !peer_id)
        return;
    for (int i = 0; i < p->recv_count; i++) {
        if (p->recv[i].active && strcmp(p->recv[i].peer_id, peer_id) == 0)
            recv_stream_destroy(&p->recv[i]);
    }
}

static bool has_send_targets(media_pipeline_t *p, bool video) {
    for (int i = 0; i < p->send_peer_count; i++)
        if (p->send_peers[i].active &&
            (video ? p->send_peers[i].video_sender != NULL : p->send_peers[i].audio_sender != NULL))
            return true;
    return false;
}

static int push_video_to_stream(media_pipeline_t *p, media_send_stream_t *s,
                                const video_frame_t *frame) {
    if (!s->active || !s->has_encoder || !has_send_targets(p, true))
        return RTC_ERR_INVALID;

    /* Aggregate rate control across all peers: use minimum target bitrate
     * (weakest-link strategy) and OR keyframe requests (any peer requesting
     * a keyframe triggers one for the shared encoder). */
    {
        int min_bitrate = 0;
        bool any_keyframe = false;
        for (int peer = 0; peer < p->send_peer_count; peer++) {
            mp_send_peer_t *sp = &p->send_peers[peer];
            if (!sp->active || !sp->video_sender)
                continue;
            int br = rtc_rtp_sender_get_target_bitrate(sp->video_sender);
            uint32_t peer_bps = atomic_load(&sp->bitrate_bps);
            if (peer_bps && (br == 0 || peer_bps / 1000 < (uint32_t)br))
                br = (int)(peer_bps / 1000 ? peer_bps / 1000 : 1);
            if (br > 0) {
                if (min_bitrate == 0 || br < min_bitrate)
                    min_bitrate = br;
            }
            if (rtc_rtp_sender_should_keyframe(sp->video_sender))
                any_keyframe = true;
        }
        if (min_bitrate > 0) {
            int rc = video_encoder_set_bitrate(&s->video_enc, min_bitrate);
            if (rc != RTC_OK)
                return rc;
        }
        if (any_keyframe)
            video_encoder_request_keyframe(&s->video_enc);
    }

    /* Encode */
    video_packet_t pkt;
    int rc = video_encode(&s->video_enc, frame, &pkt);
    if (rc != RTC_OK) {
        s->send_stats.frames_dropped++;
        return rc;
    }

    /* Stats (always, zero cost) */
    s->send_stats.frames_encoded++;
    s->send_stats.bytes_encoded += (uint64_t)pkt.len;
    if (pkt.is_keyframe)
        s->send_stats.keyframes_encoded++;

    /* IVF dump (opt-in) */
    if (p->dbg.send_dump)
        video_dump_write_frame(p->dbg.send_dump, pkt.data, (size_t)pkt.len, pkt.timestamp);

    video_payload_t payloads[VIDEO_PAYLOAD_MAX_FRAME_PACKETS];
    int payload_count = 0;
    if (pkt.len <= 0 || (size_t)pkt.len > s->video_format->max_frame_size)
        rc = RTC_ERR_NOMEM;
    else
        rc = s->video_format->packetize(pkt.data, (size_t)pkt.len, pkt.is_keyframe, payloads,
                                        VIDEO_PAYLOAD_MAX_FRAME_PACKETS, &payload_count);
    if (rc != RTC_OK) {
        s->send_stats.frames_dropped++;
        video_encoder_request_keyframe(&s->video_enc);
        return rc;
    }

    /* Fan out to all registered send peers */
    s->timestamp_remainder += s->video_format->clock_rate;
    uint32_t samples = s->timestamp_remainder / s->fps;
    s->timestamp_remainder %= s->fps;
    int result = RTC_OK;
    for (int peer = 0; peer < p->send_peer_count; peer++) {
        mp_send_peer_t *sp = &p->send_peers[peer];
        if (!sp->active || !sp->video_sender)
            continue;
        sp->pending_samples += samples;
        uint32_t bitrate = atomic_load(&sp->bitrate_bps);
        if (bitrate) {
            uint64_t now = rtc_time_ms();
            uint64_t elapsed = sp->budget_time_ms ? now - sp->budget_time_ms : 1000;
            if (elapsed > 1000)
                elapsed = 1000;
            sp->budget_time_ms = now;
            sp->budget_bytes += (int64_t)((uint64_t)bitrate * elapsed / 8000);
            int64_t burst = (int64_t)bitrate / 8;
            if (sp->budget_bytes > burst)
                sp->budget_bytes = burst;
            if (sp->budget_bytes < 0) {
                s->send_stats.frames_dropped++;
                video_encoder_request_keyframe(&s->video_enc);
                result = RTC_ERR_NOMEM;
                continue;
            }
            sp->budget_bytes -= pkt.len;
        }
        uint32_t send_samples = sp->pending_samples;
        sp->pending_samples = 0;
        for (int i = 0; i < payload_count; i++) {
            /* The sender advances its timestamp AFTER sending this packet. */
            rc = rtc_rtp_sender_send(sp->video_sender, payloads[i].data, payloads[i].len,
                                     payloads[i].marker ? send_samples : 0, payloads[i].marker);
            if (rc != RTC_OK) {
                result = rc;
                (void)rtc_rtp_sender_discard_frame(sp->video_sender,
                                                   i == payload_count - 1 ? 0 : send_samples);
                video_encoder_request_keyframe(&s->video_enc);
                break;
            }
            s->send_stats.packets_sent++;
            s->send_stats.bytes_sent += payloads[i].len;
        }
    }

    return result;
}

int media_pipeline_push_video(media_pipeline_t *p, media_send_stream_t *stream,
                              const video_frame_t *frame) {
    if (!p || !stream || !frame || !stream->is_video)
        return RTC_ERR_INVALID;
    return push_video_to_stream(p, stream, frame);
}

static int push_audio_to_stream(media_pipeline_t *p, media_send_stream_t *s,
                                const audio_frame_t *audio) {
    if (!s->active || !s->has_encoder || !has_send_targets(p, false))
        return RTC_ERR_INVALID;

    int out_len = (int)sizeof(s->audio_out);
    int rc = audio_encode(&s->audio_enc, audio, s->audio_out, &out_len);
    if (rc != RTC_OK)
        return rc;

    /* Fan out to all registered send peers */
    int result = RTC_OK;
    for (int peer = 0; peer < p->send_peer_count; peer++) {
        mp_send_peer_t *sp = &p->send_peers[peer];
        if (!sp->active || !sp->audio_sender)
            continue;
        rc = rtc_rtp_sender_send(sp->audio_sender, s->audio_out, (size_t)out_len, 960, false);
        if (rc != RTC_OK)
            result = rc;
    }

    return result;
}

int media_pipeline_push_audio(media_pipeline_t *p, media_send_stream_t *stream,
                              const audio_frame_t *audio) {
    if (!p || !stream || !audio || stream->is_video)
        return RTC_ERR_INVALID;
    return push_audio_to_stream(p, stream, audio);
}

static void request_stream_keyframe(media_recv_stream_t *s) {
    uint64_t now = rtc_time_ms();
    if (s->request_keyframe &&
        (!s->last_keyframe_request_ms || now - s->last_keyframe_request_ms >= 500)) {
        s->last_keyframe_request_ms = now;
        s->request_keyframe(s->feedback_user);
    }
}

static void recv_video_on_stream(media_pipeline_t *p, media_recv_stream_t *s, const uint8_t *data,
                                 int len, uint16_t seq, uint32_t timestamp, bool marker) {
    s->recv_stats.packets_received++;
    s->recv_stats.bytes_received += (uint64_t)len;

    /* RTP sequence gap tracking (always, zero cost) */
    if (!s->recv_stats.rtp_seq_initialized) {
        s->recv_stats.rtp_last_seq = seq;
        s->recv_stats.rtp_seq_initialized = true;
    } else {
        uint16_t expected = s->recv_stats.rtp_last_seq + 1;
        int16_t diff = (int16_t)(seq - s->recv_stats.rtp_last_seq);
        if (diff == 0) {
            s->recv_stats.packets_duplicate++;
        } else if (diff > 0) {
            if (diff > 1) {
                s->recv_stats.rtp_seq_gaps++;
                s->recv_stats.packets_missing += (uint64_t)(diff - 1);
                RTC_LOG_DBG("RTP gap: expected %u, got %u (missing %d)", expected, seq, diff - 1);
            }
            s->recv_stats.rtp_last_seq = seq;
        } else {
            s->recv_stats.packets_reordered++;
        }
    }

    /* Depacketize */
    video_payload_frame_t frame;
    int depack_rc = s->video_format->depacketize(s->video_depacketizer, data, (size_t)len, seq,
                                                 timestamp, marker, &frame);
    if (frame.dropped_frames) {
        s->recv_stats.depack_frames_dropped += frame.dropped_frames;
        s->waiting_keyframe = true;
        request_stream_keyframe(s);
    }
    if (depack_rc == RTC_OK) {
        const uint8_t *frame_data = frame.data;
        size_t frame_len = frame.len;
        bool is_key = frame.is_keyframe;
        if (frame_data && frame_len > 0) {
            if (s->waiting_keyframe && !is_key) {
                request_stream_keyframe(s);
                return;
            }
            s->recv_stats.frames_assembled++;
            s->recv_stats.bytes_decoded += frame_len;

            /* IVF dump (opt-in) */
            if (p->dbg.recv_dump)
                video_dump_write_frame(p->dbg.recv_dump, frame_data, frame_len, timestamp);

            video_frame_t decoded;
            int rc = video_decode(&s->video_dec, frame_data, (int)frame_len, &decoded);
            if (rc == RTC_OK) {
                s->waiting_keyframe = false;
                s->recv_stats.frames_decoded++;
                if (is_key)
                    s->recv_stats.keyframes_decoded++;

                /* Frame checksum (opt-in) */
                if (p->dbg.frame_checksum) {
                    uint32_t cksum = video_frame_checksum(&decoded);
                    RTC_LOG_DBG("decoded %dx%d key=%d %zu B cksum=0x%08X", decoded.width,
                                decoded.height, is_key, frame_len, cksum);
                } else {
                    RTC_LOG_DBG("Pipeline: decoded %dx%d frame (%zu bytes, key=%d)", decoded.width,
                                decoded.height, frame_len, is_key);
                }

                if (p->renderer.on_video_frame) {
                    p->renderer.on_video_frame(s->peer_id, s->label, &decoded, p->renderer.user);
                    s->recv_stats.frames_rendered++;
                }
            } else {
                s->waiting_keyframe = true;
                request_stream_keyframe(s);
                s->recv_stats.frames_decode_failed++;
                RTC_LOG_WARN("Video decode failed (rc=%d, %zu B, key=%d)", rc, frame_len, is_key);
            }
        }
    }
}

int media_pipeline_set_recv_feedback(media_pipeline_t *p, media_recv_stream_t *stream,
                                     void (*request_keyframe)(void *), void *user) {
    if (!p || !stream || !stream->active || !stream->is_video)
        return RTC_ERR_INVALID;
    stream->request_keyframe = request_keyframe;
    stream->feedback_user = user;
    return RTC_OK;
}

static void recv_audio_on_stream(media_pipeline_t *p, media_recv_stream_t *s, const uint8_t *data,
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
    if (!p || !stream || len < 0 || (!data && len) || (!len && !stream->is_video))
        return RTC_ERR_INVALID;

    media_recv_stream_t *s = stream;
    if (!s->active)
        return RTC_ERR_INVALID;

    if (s->is_video)
        recv_video_on_stream(p, s, data, len, seq, timestamp, marker);
    else
        recv_audio_on_stream(p, s, data, len, seq, timestamp);

    return RTC_OK;
}

static const video_payload_format_t *dump_format(media_pipeline_t *p, bool send) {
    const video_payload_format_t *format = NULL;
    int count = send ? p->send_count : p->recv_count;
    for (int i = 0; i < count; i++) {
        bool active = send ? p->send[i].active : p->recv[i].active;
        bool video = send ? p->send[i].is_video : p->recv[i].is_video;
        if (!active || !video)
            continue;
        const video_payload_format_t *candidate =
            send ? p->send[i].video_format : p->recv[i].video_format;
        if (format && format != candidate) {
            RTC_LOG_ERR("Pipeline: cannot combine video payload formats in one IVF dump");
            return NULL;
        }
        format = candidate;
    }
    if (!format)
        format = video_payload_format_find(p->default_video_codec);
    if (format && !format->ivf_fourcc) {
        RTC_LOG_ERR("Pipeline: selected video format does not support IVF dumps");
        return NULL;
    }
    return format;
}

int media_pipeline_set_debug(media_pipeline_t *p, const media_debug_config_t *cfg) {
    if (!p)
        return RTC_ERR_INVALID;

    video_dump_close(p->dbg.send_dump);
    video_dump_close(p->dbg.recv_dump);
    memset(&p->dbg, 0, sizeof(p->dbg));
    if (!cfg)
        return RTC_OK;

    video_dump_t *send_dump = NULL, *recv_dump = NULL;
    const video_payload_format_t *send_format = NULL, *recv_format = NULL;
    if (cfg->send_dump_path) {
        send_format = dump_format(p, true);
        if (!send_format)
            return RTC_ERR_INVALID;
        send_dump = video_dump_open(cfg->send_dump_path, send_format->ivf_fourcc, cfg->dump_width,
                                    cfg->dump_height, cfg->dump_fps);
        if (!send_dump)
            return RTC_ERR_GENERIC;
    }
    if (cfg->recv_dump_path) {
        recv_format = dump_format(p, false);
        if (!recv_format) {
            video_dump_close(send_dump);
            return RTC_ERR_INVALID;
        }
        recv_dump = video_dump_open(cfg->recv_dump_path, recv_format->ivf_fourcc, cfg->dump_width,
                                    cfg->dump_height, cfg->dump_fps);
        if (!recv_dump) {
            video_dump_close(send_dump);
            return RTC_ERR_GENERIC;
        }
    }
    p->dbg.send_dump = send_dump;
    p->dbg.recv_dump = recv_dump;
    p->dbg.send_format = send_format;
    p->dbg.recv_format = recv_format;

    p->dbg.frame_checksum = cfg->enable_frame_checksum;
    p->dbg.psnr_interval = cfg->psnr_sample_interval;

    return RTC_OK;
}

const video_send_stats_t *media_pipeline_get_send_stats(media_pipeline_t *p, int index) {
    if (!p || index < 0 || index >= p->send_count || !p->send[index].is_video)
        return NULL;
    return &p->send[index].send_stats;
}

const video_recv_stats_t *media_pipeline_get_recv_stats(media_pipeline_t *p, int index) {
    if (!p || index < 0 || index >= p->recv_count || !p->recv[index].is_video)
        return NULL;
    return &p->recv[index].recv_stats;
}
