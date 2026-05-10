/*
 * media_pipeline.c - Multi-stream media pipeline.
 *
 * Each send/recv stream has its own encoder/decoder, packetizer,
 * and jitter buffer. Streams are identified by label (send) or SSRC (recv).
 */
#include "media/media_pipeline.h"
#include "media/video_stats.h"
#include "video_dump.h"
#include "video_debug.h"
#include "vp8_packetizer.h"
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

    /* Debug stats */
    video_send_stats_t send_stats;
    video_recv_stats_t recv_stats;
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

    mp_send_peer_t send_peers[MP_MAX_SEND_PEERS];
    int send_peer_count;
    media_renderer_t renderer;

    char default_video_codec[16];
    char default_audio_codec[16];

    /* Debug state */
    struct {
        video_dump_t *send_dump;
        video_dump_t *recv_dump;
        bool frame_checksum;
        int psnr_interval;
    } dbg;
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

    return p;
}

void media_pipeline_destroy(media_pipeline_t *p) {
    if (!p)
        return;
    for (int i = 0; i < p->send_count; i++)
        stream_destroy(&p->send[i]);
    for (int i = 0; i < p->recv_count; i++)
        stream_destroy(&p->recv[i]);
    if (p->dbg.send_dump)
        video_dump_close(p->dbg.send_dump);
    if (p->dbg.recv_dump)
        video_dump_close(p->dbg.recv_dump);
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
    video_recv_stats_init(&s->recv_stats);

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

    /* TODO: Rate control only reads first peer's feedback. With multiple
     * peers at different network conditions (e.g. one on LTE, one on fiber),
     * we ignore all but the first peer's bitrate/keyframe signals. Fix:
     * aggregate across all peers — use the minimum target bitrate (encode
     * for the weakest link) and OR together keyframe requests (any peer
     * requesting a keyframe should trigger one). */
    for (int peer = 0; peer < p->send_peer_count; peer++) {
        mp_send_peer_t *sp = &p->send_peers[peer];
        if (!sp->active || !sp->video_sender)
            continue;
        int br = rtc_rtp_sender_get_target_bitrate(sp->video_sender);
        if (br > 0)
            video_encoder_set_bitrate(&s->video_enc, br);
        if (rtc_rtp_sender_should_keyframe(sp->video_sender))
            video_encoder_request_keyframe(&s->video_enc);
        break; /* use first peer's rate control for encoder config */
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
        for (int i = 0; i < payload_count; i++) {
            rtc_rtp_sender_send(sp->video_sender, payloads[i].data, payloads[i].len,
                                i == 0 ? samples : 0, payloads[i].marker);
            s->send_stats.packets_sent++;
            s->send_stats.bytes_sent += payloads[i].len;
        }
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
    s->recv_stats.packets_received++;
    s->recv_stats.bytes_received += (uint64_t)len;

    /* RTP sequence gap tracking (always, zero cost) */
    if (!s->recv_stats.rtp_seq_initialized) {
        s->recv_stats.rtp_last_seq = seq;
        s->recv_stats.rtp_seq_initialized = true;
    } else {
        uint16_t expected = s->recv_stats.rtp_last_seq + 1;
        int16_t diff = (int16_t)(seq - expected);
        if (diff > 0 && diff < 1000) {
            s->recv_stats.rtp_seq_gaps++;
            s->recv_stats.packets_missing += (uint64_t)diff;
            RTC_LOG_WARN("RTP gap: expected %u, got %u (missing %d)", expected, seq, diff);
        } else if (diff < 0 && diff > -1000) {
            s->recv_stats.packets_reordered++;
        } else if (seq == s->recv_stats.rtp_last_seq) {
            s->recv_stats.packets_duplicate++;
        }
        s->recv_stats.rtp_last_seq = seq;
    }

    /* Depacketize */
    bool was_collecting = s->vp8_depack.collecting;
    const uint8_t *frame_data;
    size_t frame_len;
    bool is_key;

    if (rtc_vp8_depacketize(&s->vp8_depack, data, (size_t)len, timestamp, marker, &frame_data,
                            &frame_len, &is_key) == RTC_OK) {
        if (was_collecting && !s->vp8_depack.collecting && (!frame_data || frame_len == 0))
            s->recv_stats.depack_frames_dropped++;

        if (frame_data && frame_len > 0) {
            s->recv_stats.frames_assembled++;
            s->recv_stats.bytes_decoded += frame_len;

            /* IVF dump (opt-in) */
            if (p->dbg.recv_dump)
                video_dump_write_frame(p->dbg.recv_dump, frame_data, frame_len, timestamp);

            video_frame_t decoded;
            int rc = video_decode(&s->video_dec, frame_data, (int)frame_len, &decoded);
            if (rc == RTC_OK) {
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
                s->recv_stats.frames_decode_failed++;
                RTC_LOG_WARN("VP8 decode failed (rc=%d, %zu B, key=%d)", rc, frame_len, is_key);
            }
        }
    } else {
        if (was_collecting && !s->vp8_depack.collecting)
            s->recv_stats.depack_frames_dropped++;
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

/* ---- Debug config ---- */

int media_pipeline_set_debug(media_pipeline_t *p, const media_debug_config_t *cfg) {
    if (!p)
        return RTC_ERR_INVALID;

    /* Close existing dumps */
    if (p->dbg.send_dump) {
        video_dump_close(p->dbg.send_dump);
        p->dbg.send_dump = NULL;
    }
    if (p->dbg.recv_dump) {
        video_dump_close(p->dbg.recv_dump);
        p->dbg.recv_dump = NULL;
    }
    p->dbg.frame_checksum = false;
    p->dbg.psnr_interval = 0;

    if (!cfg)
        return RTC_OK; /* disable everything */

    if (cfg->send_dump_path)
        p->dbg.send_dump = video_dump_open(cfg->send_dump_path, "VP80", cfg->dump_width,
                                           cfg->dump_height, cfg->dump_fps);
    if (cfg->recv_dump_path)
        p->dbg.recv_dump = video_dump_open(cfg->recv_dump_path, "VP80", cfg->dump_width,
                                           cfg->dump_height, cfg->dump_fps);
    p->dbg.frame_checksum = cfg->enable_frame_checksum;
    p->dbg.psnr_interval = cfg->psnr_sample_interval;

    return RTC_OK;
}

/* ---- Stats access ---- */

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
