/*
 * RTP send/receive streams.
 */
#include "rtc/rtc_rtp_stream.h"

#include "rtc_nack_buf.h"
#include "rtc_rate_control.h"
#include "rtc/rtc_rtp_ext.h"
#include "rtc_transport_internal.h"
#include "rtc_twcc_sender.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RTC_PLI_MIN_INTERVAL_MS 500

struct rtc_rtp_send_stream {
    rtc_rtp_session_t rtp_session;
    rtc_transport_t *transport;
    rtc_rtcp_stats_t rtcp_stats;
#ifdef MRTC_ENABLE_RATE_CONTROL
    rtc_rate_controller_t *rate_ctrl;
#endif
    rtc_nack_buf_t *nack_buf;
    bool active;

    uint32_t max_bitrate_bps;
    bool remb_enabled;
    _Atomic uint32_t remote_limit_bps;
    _Atomic uint64_t remote_limit_ms;
    bool send_active;
    bool video;
    bool started;
    uint64_t generation;
    uint8_t mid_ext_id;
    char mid[17];
    rtc_rtp_recv_stream_source_fn on_source;
    void *source_user;

    rtc_rtp_send_stream_nack_fn on_nack;
    void *on_nack_user;
    rtc_rtp_send_stream_pli_fn on_pli;
    void *on_pli_user;

    uint64_t last_pli_handled_ms;
    atomic_bool keyframe_requested;

#ifdef MRTC_ENABLE_TWCC
    rtc_twcc_sender_t *twcc;
    uint8_t twcc_ext_id;
#endif
};

struct rtc_rtp_recv_stream {
    uint8_t payload_type;
    uint32_t clock_rate;
    rtc_rtp_recv_stream_frame_fn on_frame;
    void *on_frame_user;
    rtc_rtp_recv_stream_source_fn on_source_change;
    void *source_user;
    uint64_t last_activity_ms;
    uint64_t source_generation;
    uint32_t ssrc;
    char mid[RTC_RTP_MID_MAX];
    bool has_mid;
    rtc_rtcp_stats_t rtcp_stats;
    bool active;
    rtc_transport_t *transport;
    rtc_rtp_recv_stream_feedback_fn on_feedback;
    void *feedback_user;
    bool seq_initialized;
    uint16_t highest_seq;
    struct {
        uint16_t seq;
        uint8_t attempts;
        uint64_t first_ms, last_ms;
        bool used;
    } missing[64];
    bool recovery;
    bool frame_open;
    bool frame_timestamp_valid;
    bool frame_marker_seen;
    uint32_t frame_timestamp;
    uint64_t last_packet_ms;
    uint64_t last_pli_ms;
    uint8_t feedback;
    uint8_t fir_sequence;
};

rtc_rtp_send_stream_t *rtc_rtp_send_stream_create(const rtc_rtp_send_stream_config_t *cfg) {
    if (!cfg)
        return NULL;
    rtc_rtp_send_stream_t *stream = (rtc_rtp_send_stream_t *)calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;
    stream->active = true;
    stream->send_active = true;
    atomic_init(&stream->keyframe_requested, false);
    atomic_init(&stream->remote_limit_bps, UINT32_MAX);
    if (rtc_rtp_session_init(&stream->rtp_session, cfg->payload_type, cfg->clock_rate) != RTC_OK ||
        rtc_rtcp_stats_init(&stream->rtcp_stats, stream->rtp_session.ssrc) != RTC_OK) {
        free(stream);
        return NULL;
    }
    stream->rtcp_stats.send_clock_rate = cfg->clock_rate;
    if (cfg->ssrc) {
        stream->rtp_session.ssrc = cfg->ssrc;
        stream->rtcp_stats.ssrc = cfg->ssrc;
    }
    stream->generation = 1;
    return stream;
}

void rtc_rtp_send_stream_destroy(rtc_rtp_send_stream_t *stream) {
    if (!stream)
        return;
    rtc_transport_unregister_source(stream->transport, stream);
    rtc_transport_forget_rtp(stream->transport, stream->rtp_session.ssrc);
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (stream->rate_ctrl)
        rtc_rate_control_destroy(stream->rate_ctrl);
#endif
    if (stream->nack_buf)
        rtc_nack_buf_destroy(stream->nack_buf);
    rtc_transport_release(stream->transport);
    free(stream);
}

rtc_rtp_recv_stream_t *rtc_rtp_recv_stream_create(const rtc_rtp_recv_stream_config_t *cfg) {
    if (!cfg)
        return NULL;
    rtc_rtp_recv_stream_t *stream = (rtc_rtp_recv_stream_t *)calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;
    stream->payload_type = cfg->payload_type;
    stream->clock_rate = cfg->clock_rate;
    stream->feedback = RTC_SDP_FB_NACK | RTC_SDP_FB_PLI | RTC_SDP_FB_FIR;
    if (rtc_rtcp_stats_init(&stream->rtcp_stats, cfg->local_ssrc) != RTC_OK) {
        free(stream);
        return NULL;
    }
    return stream;
}

void rtc_rtp_recv_stream_destroy(rtc_rtp_recv_stream_t *stream) {
    if (!stream)
        return;
    rtc_transport_unregister_source(stream->transport, stream);
    rtc_transport_release(stream->transport);
    free(stream);
}

int rtc_rtp_send_stream_set_payload_type(rtc_rtp_send_stream_t *stream, uint8_t payload_type) {
    if (!stream || payload_type > 127 ||
        (stream->rtcp_stats.packets_sent && stream->rtp_session.payload_type != payload_type))
        return RTC_ERR_INVALID;
    stream->rtp_session.payload_type = payload_type;
    return RTC_OK;
}

int rtc_rtp_recv_stream_set_payload_type(rtc_rtp_recv_stream_t *stream, uint8_t payload_type) {
    if (!stream || payload_type > 127 ||
        (stream->rtcp_stats.packets_received && stream->payload_type != payload_type))
        return RTC_ERR_INVALID;
    stream->payload_type = payload_type;
    return RTC_OK;
}

void rtc_rtp_send_stream_set_active(rtc_rtp_send_stream_t *stream, bool active) {
    if (stream)
        stream->active = active;
}

bool rtc_rtp_send_stream_is_active(const rtc_rtp_send_stream_t *stream) {
    return stream && stream->active;
}

uint32_t rtc_rtp_send_stream_ssrc(const rtc_rtp_send_stream_t *stream) {
    return stream ? stream->rtp_session.ssrc : 0;
}

const rtc_rtcp_stats_t *rtc_rtp_send_stream_stats(const rtc_rtp_send_stream_t *stream) {
    if (stream)
        (void)rtc_transport_sync_rtp_stats(stream->transport,
                                           &((rtc_rtp_send_stream_t *)stream)->rtcp_stats, NULL);
    return stream ? &stream->rtcp_stats : NULL;
}

uint64_t rtc_rtp_send_stream_generation(const rtc_rtp_send_stream_t *stream) {
    return stream ? stream->generation : 0;
}

void rtc_rtp_send_stream_on_source(rtc_rtp_send_stream_t *stream, rtc_rtp_recv_stream_source_fn fn,
                                   void *user) {
    if (stream) {
        stream->on_source = fn;
        stream->source_user = user;
    }
}

void rtc_rtp_recv_stream_set_local_ssrc(rtc_rtp_recv_stream_t *stream, uint32_t ssrc) {
    if (stream) {
        rtc_rtcp_stats_restart_sender(&stream->rtcp_stats, ssrc);
    }
}

int rtc_rtp_send_stream_set_mid(rtc_rtp_send_stream_t *stream, uint8_t ext_id, const char *mid) {
    if (!stream || ext_id > 14 || (ext_id && (!mid || !mid[0] || strlen(mid) > 16)))
        return RTC_ERR_INVALID;
    if (stream->started &&
        (ext_id != stream->mid_ext_id || (ext_id && strcmp(stream->mid, mid) != 0)))
        return RTC_ERR_INVALID;
#ifdef MRTC_ENABLE_TWCC
    if (ext_id && ext_id == stream->twcc_ext_id)
        return RTC_ERR_INVALID;
#endif
    stream->mid_ext_id = ext_id;
    if (ext_id)
        memcpy(stream->mid, mid, strlen(mid) + 1);
    return RTC_OK;
}

static int send_stream_rotate(uint32_t previous, uint32_t current, void *user) {
    rtc_rtp_send_stream_t *stream = user;
    rtc_nack_buf_t *replacement =
        stream->nack_buf ? rtc_nack_buf_create(NACK_BUF_DEFAULT_SIZE) : NULL;
    if (stream->nack_buf && !replacement)
        return RTC_ERR_NOMEM;
    uint16_t seq;
    if (rtc_random_bytes((uint8_t *)&seq, sizeof(seq)) != RTC_OK) {
        rtc_nack_buf_destroy(replacement);
        return RTC_ERR_GENERIC;
    }
    rtc_nack_buf_destroy(stream->nack_buf);
    stream->nack_buf = replacement;
    stream->rtp_session.ssrc = current;
    stream->rtp_session.seq = seq;
    rtc_rtcp_stats_restart_sender(&stream->rtcp_stats, current);
    stream->generation++;
    atomic_store(&stream->keyframe_requested, true);
    if (stream->on_source)
        stream->on_source(previous, current, stream->source_user);
    return RTC_OK;
}

void rtc_rtp_send_stream_attach_transport(rtc_rtp_send_stream_t *stream,
                                          rtc_transport_t *transport) {
    if (!stream)
        return;
    if (stream->transport == transport)
        return;
    rtc_transport_unregister_source(stream->transport, stream);
    rtc_transport_retain(transport);
    rtc_transport_release(stream->transport);
    stream->transport = transport;
    if (transport &&
        rtc_transport_register_source(transport, stream->rtp_session.ssrc, send_stream_rotate,
                                      stream, stream->rtcp_stats.cname) != RTC_OK) {
        stream->active = false;
        RTC_LOG_ERR("Local RTP source registration failed");
        rtc_transport_close(transport);
    }
}

void rtc_rtp_send_stream_attach_twcc(rtc_rtp_send_stream_t *stream, uint8_t ext_id) {
#ifdef MRTC_ENABLE_TWCC
    if (!stream || !stream->transport)
        return;
    if (!ext_id) {
        stream->twcc = NULL;
        stream->twcc_ext_id = 0;
        return;
    }
    if (ext_id > 14 || ext_id == stream->mid_ext_id) {
        rtc_transport_close(stream->transport);
        return;
    }
    stream->twcc = rtc_transport_twcc_sender(stream->transport);
    stream->twcc_ext_id = ext_id;
#else
    (void)stream;
    (void)ext_id;
#endif
}

void rtc_rtp_send_stream_arm_video(rtc_rtp_send_stream_t *stream) {
    if (!stream || !stream->active)
        return;
    stream->video = true;
#ifdef MRTC_ENABLE_RATE_CONTROL
    rtc_rate_control_config_t rc_cfg = {
        .target_bitrate_kbps = 500,
        .min_bitrate_kbps = 100,
        .max_bitrate_kbps = 2500,
    };
    if (!stream->rate_ctrl)
        stream->rate_ctrl = rtc_rate_control_create(&rc_cfg);
    if (!stream->rate_ctrl)
        RTC_LOG_WARN("Video rate-controller allocation failed");
#endif
    if (!stream->nack_buf)
        stream->nack_buf = rtc_nack_buf_create(NACK_BUF_DEFAULT_SIZE);
    if (!stream->nack_buf)
        RTC_LOG_WARN("Video retransmission-buffer allocation failed");
}

void rtc_rtp_send_stream_set_send_active(rtc_rtp_send_stream_t *stream, bool active) {
    if (stream) {
        if (!active)
            rtc_transport_discard_rtp(stream->transport, stream->rtp_session.ssrc);
        stream->send_active = active;
    }
}

bool rtc_rtp_send_stream_send_active(const rtc_rtp_send_stream_t *stream) {
    return stream && stream->send_active;
}

void rtc_rtp_send_stream_set_max_bitrate(rtc_rtp_send_stream_t *stream, uint32_t bitrate_bps) {
    if (stream)
        stream->max_bitrate_bps = bitrate_bps;
}

uint32_t rtc_rtp_send_stream_max_bitrate(const rtc_rtp_send_stream_t *stream) {
    return stream ? stream->max_bitrate_bps : 0;
}

static int send_stream_send(rtc_rtp_send_stream_t *stream, const uint8_t *payload, size_t len,
                            uint32_t samples, bool marker) {
    if (!stream || !stream->active)
        return RTC_ERR_INVALID;
    if (!stream->transport)
        return RTC_ERR_INVALID;
    if (!stream->send_active)
        return RTC_OK;
    if (atomic_load(&stream->remote_limit_bps) == 0 &&
        rtc_time_ms() - atomic_load(&stream->remote_limit_ms) < 10000)
        return RTC_OK;

    rtc_rtp_packet_t pkt;
    int rc;
    rtc_rtp_ext_t exts[2];
    size_t ext_count = 0;
    if (stream->mid_ext_id)
        rtc_rtp_ext_make_string(&exts[ext_count++], stream->mid_ext_id, stream->mid);
#ifdef MRTC_ENABLE_TWCC
    uint16_t twcc_seq = 0;
    bool tagged = (stream->twcc && stream->twcc_ext_id != 0);
    if (tagged) {
        twcc_seq = atomic_fetch_add(&stream->twcc->next_seq, 1);
        rtc_rtp_ext_make_transport_cc(&exts[ext_count++], stream->twcc_ext_id, twcc_seq);
    }
#endif
    rc = rtc_rtp_session_send_with_ext(&stream->rtp_session, &pkt, exts, ext_count, payload, len,
                                       samples, marker);
    if (rc != RTC_OK)
        return rc;

    stream->started = true;
    size_t pkt_len = pkt.buf_len;
    int target = rtc_rtp_send_stream_get_target_bitrate(stream);
    rc = rtc_transport_send_rtp_media(stream->transport, pkt.buf, &pkt_len, sizeof(pkt.buf),
                                      stream->video, target > 0 ? (uint32_t)target * 1000 : 0);
    if (rc != RTC_OK)
        return rc;
    if (!rtc_transport_sync_rtp_stats(stream->transport, &stream->rtcp_stats, NULL))
        rtc_rtcp_stats_on_rtp_send(&stream->rtcp_stats, pkt.header.timestamp, len);

#ifdef MRTC_ENABLE_TWCC
    if (tagged) {
        rtc_twcc_sent_pkt_t *entry = &stream->twcc->ring[twcc_seq & (RTC_TWCC_SENDER_RING - 1)];
        entry->seq = twcc_seq;
        entry->size = (uint16_t)pkt_len;
        entry->send_time_us = rtc_time_us();
        entry->used = !(stream->video && rtc_transport_rtp_pacing_enabled(stream->transport));
    }
#endif

    if (stream->nack_buf) {
#ifdef MRTC_ENABLE_TWCC
        rtc_nack_buf_store(stream->nack_buf, pkt.buf, pkt_len, pkt.header.seq, tagged, twcc_seq);
#else
        rtc_nack_buf_store(stream->nack_buf, pkt.buf, pkt_len, pkt.header.seq, false, 0);
#endif
    }

    return RTC_OK;
}

typedef struct {
    rtc_rtp_send_stream_t *stream;
    const uint8_t *payload;
    size_t len;
    uint32_t samples;
    bool marker;
    int result;
} stream_send_args_t;

static void stream_send_on_worker(void *user) {
    stream_send_args_t *args = user;
    args->result =
        send_stream_send(args->stream, args->payload, args->len, args->samples, args->marker);
}

static void stream_discard_frame_on_worker(void *user) {
    stream_send_args_t *args = user;
    rtc_transport_discard_rtp(args->stream->transport, args->stream->rtp_session.ssrc);
    args->stream->rtp_session.timestamp += args->samples;
}

int rtc_rtp_send_stream_discard_frame(rtc_rtp_send_stream_t *stream, uint32_t samples) {
    if (!stream || !stream->transport)
        return RTC_ERR_INVALID;
    stream_send_args_t args = {.stream = stream, .samples = samples};
    return rtc_transport_invoke(stream->transport, stream_discard_frame_on_worker, &args);
}

int rtc_rtp_send_stream_send(rtc_rtp_send_stream_t *stream, const uint8_t *payload, size_t len,
                             uint32_t samples, bool marker) {
    if (!stream || !stream->transport)
        return RTC_ERR_INVALID;
    stream_send_args_t args = {
        .stream = stream,
        .payload = payload,
        .len = len,
        .samples = samples,
        .marker = marker,
        .result = RTC_ERR_INVALID,
    };
    int rc = rtc_transport_invoke(stream->transport, stream_send_on_worker, &args);
    return rc == RTC_OK ? args.result : rc;
}

int rtc_rtp_send_stream_get_target_bitrate(const rtc_rtp_send_stream_t *stream) {
    if (!stream)
        return 0;
    int kbps = 0;
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (stream->rate_ctrl)
        kbps = rtc_rate_control_get_bitrate(stream->rate_ctrl);
#endif
    if (stream->max_bitrate_bps) {
        int cap_kbps = (int)(stream->max_bitrate_bps / 1000);
        if (cap_kbps > 0 && (!kbps || kbps > cap_kbps))
            kbps = cap_kbps;
    }
    uint32_t cap = atomic_load(&stream->remote_limit_bps);
    if (cap != UINT32_MAX && rtc_time_ms() - atomic_load(&stream->remote_limit_ms) < 10000) {
        int remote = (int)(cap / 1000);
        if (cap && remote == 0)
            remote = 1;
        if (!kbps || remote < kbps)
            kbps = remote;
    }
    return kbps;
}

void rtc_rtp_send_stream_set_remote_feedback(rtc_rtp_send_stream_t *stream, uint8_t feedback) {
    if (stream) {
        stream->remb_enabled = (feedback & RTC_SDP_FB_REMB) != 0;
        if (!stream->remb_enabled)
            atomic_store(&stream->remote_limit_bps, UINT32_MAX);
    }
}

void rtc_rtp_send_stream_on_remb(rtc_rtp_send_stream_t *stream, uint32_t bitrate_bps) {
    if (stream && stream->remb_enabled) {
        atomic_store(&stream->remote_limit_bps, bitrate_bps);
        atomic_store(&stream->remote_limit_ms, rtc_time_ms());
    }
}

bool rtc_rtp_send_stream_should_keyframe(rtc_rtp_send_stream_t *stream) {
    if (stream && atomic_exchange(&stream->keyframe_requested, false))
        return true;
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (!stream || !stream->rate_ctrl)
        return false;
    return rtc_rate_control_should_keyframe(stream->rate_ctrl);
#else
    (void)stream;
    return false;
#endif
}

void rtc_rtp_send_stream_on_nack(rtc_rtp_send_stream_t *stream, rtc_rtp_send_stream_nack_fn fn,
                                 void *user) {
    if (!stream)
        return;
    stream->on_nack = fn;
    stream->on_nack_user = user;
}

void rtc_rtp_send_stream_on_pli(rtc_rtp_send_stream_t *stream, rtc_rtp_send_stream_pli_fn fn,
                                void *user) {
    if (!stream)
        return;
    stream->on_pli = fn;
    stream->on_pli_user = user;
}

void rtc_rtp_send_stream_handle_nack(rtc_rtp_send_stream_t *stream, const uint16_t *lost_seqs,
                                     int count) {
    if (!stream || !lost_seqs || count <= 0)
        return;
    if (!stream->nack_buf || !stream->transport)
        goto notify;

    for (int i = 0; i < count; i++) {
        const uint8_t *pkt;
        size_t pkt_len;
        uint16_t twcc_seq = 0;
        if (!rtc_nack_buf_retransmit(stream->nack_buf, lost_seqs[i], &pkt, &pkt_len, &twcc_seq))
            continue;
        int rc = rtc_transport_retransmit_rtp(stream->transport, pkt, pkt_len);
        if (rc != RTC_OK) {
            if (rc != RTC_ERR_TIMEOUT)
                RTC_LOG_WARN("RTP retransmission failed: %d", rc);
            continue;
        }
#ifdef MRTC_ENABLE_TWCC
        if (twcc_seq != 0 && stream->twcc)
            rtc_twcc_sender_invalidate(stream->twcc, twcc_seq);
#else
        (void)twcc_seq;
#endif
    }

notify:
    if (stream->on_nack)
        stream->on_nack(lost_seqs, count, stream->on_nack_user);
}

void rtc_rtp_send_stream_handle_pli(rtc_rtp_send_stream_t *stream) {
    if (!stream)
        return;

    uint64_t now = rtc_time_ms();
    if (now - stream->last_pli_handled_ms < RTC_PLI_MIN_INTERVAL_MS)
        return;
    stream->last_pli_handled_ms = now;
    atomic_store(&stream->keyframe_requested, true);

    if (stream->on_pli)
        stream->on_pli(stream->on_pli_user);
}

bool rtc_rtp_send_stream_on_rr(rtc_rtp_send_stream_t *stream, const rtc_rtcp_rr_block_t *rr,
                               int rtt_ms) {
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (!stream || !stream->rate_ctrl || !rr)
        return false;
    rtc_rate_control_on_rtcp_rr(stream->rate_ctrl, rr->fraction_lost, rtt_ms, (int)rr->jitter);
    return true;
#else
    (void)stream;
    (void)rr;
    (void)rtt_ms;
    return false;
#endif
}

/* Build an SR/RR for `stats` with `build` and send it SRTCP-protected over
 * `transport`, stamping the report time on success. Shared by the send-stream
 * SR path and the recv-stream RR path. */
static void emit_rtcp_report(rtc_rtcp_stats_t *stats, rtc_transport_t *transport, bool sender) {
    int rc = rtc_transport_send_report(transport, stats, sender);
    if (rc != RTC_OK && rc != RTC_ERR_TIMEOUT)
        RTC_LOG_WARN("RTCP report send failed: %d", rc);
}

void rtc_rtp_send_stream_emit_sr(rtc_rtp_send_stream_t *stream, rtc_transport_t *transport) {
    rtc_rtp_send_stream_emit_report(stream, NULL, transport);
}

void rtc_rtp_send_stream_emit_report(rtc_rtp_send_stream_t *stream, rtc_rtp_recv_stream_t *receiver,
                                     rtc_transport_t *transport) {
    if (stream)
        (void)rtc_transport_sync_rtp_stats(transport, &stream->rtcp_stats, NULL);
    if (!stream || !stream->active || !transport || stream->rtcp_stats.packets_sent == 0)
        return;
    if (!receiver) {
        rtc_rtcp_stats_reset_source(&stream->rtcp_stats);
        emit_rtcp_report(&stream->rtcp_stats, transport, true);
        return;
    }
    if (receiver->rtcp_stats.ssrc != stream->rtcp_stats.ssrc) {
        RTC_LOG_ERR("Cannot combine reports for different local SSRCs");
        return;
    }
    rtc_rtcp_stats_t *report = &receiver->rtcp_stats;
    report->packets_sent = stream->rtcp_stats.packets_sent;
    report->octets_sent = stream->rtcp_stats.octets_sent;
    report->rtp_timestamp = stream->rtcp_stats.rtp_timestamp;
    report->send_clock_rate = stream->rtcp_stats.send_clock_rate;
    report->send_time_ms = stream->rtcp_stats.send_time_ms;
    report->send_clock_valid = stream->rtcp_stats.send_clock_valid;
    uint32_t remote_ssrc = report->remote_ssrc;
    bool first_packet = report->first_packet;
    if (!receiver->active) {
        report->remote_ssrc = 0;
        report->first_packet = true;
    }
    emit_rtcp_report(report, transport, true);
    report->remote_ssrc = remote_ssrc;
    report->first_packet = first_packet;
    stream->rtcp_stats = *report;
}

void rtc_rtp_recv_stream_set_active(rtc_rtp_recv_stream_t *stream, bool active) {
    if (stream)
        stream->active = active;
}

bool rtc_rtp_recv_stream_is_active(const rtc_rtp_recv_stream_t *stream) {
    return stream && stream->active;
}

bool rtc_rtp_recv_stream_can_receive(const rtc_rtp_recv_stream_t *stream, uint8_t payload_type) {
    return stream && stream->active && stream->on_frame && stream->payload_type == payload_type;
}

void rtc_rtp_recv_stream_set_ssrc(rtc_rtp_recv_stream_t *stream, uint32_t ssrc) {
    if (stream && stream->ssrc != ssrc) {
        uint32_t previous = stream->ssrc;
        rtc_rtcp_stats_reset_source(&stream->rtcp_stats);
        stream->ssrc = ssrc;
        if (ssrc)
            stream->source_generation++;
        stream->last_activity_ms = ssrc ? rtc_time_ms() : 0;
        stream->seq_initialized = false;
        stream->recovery = false;
        stream->frame_open = false;
        stream->frame_timestamp_valid = false;
        stream->frame_marker_seen = false;
        stream->last_packet_ms = 0;
        stream->last_pli_ms = 0;
        memset(stream->missing, 0, sizeof(stream->missing));
        if (stream->on_source_change)
            stream->on_source_change(previous, ssrc, stream->source_user);
    }
}

void rtc_rtp_recv_stream_on_source_change(rtc_rtp_recv_stream_t *stream,
                                          rtc_rtp_recv_stream_source_fn fn, void *user) {
    if (stream) {
        stream->on_source_change = fn;
        stream->source_user = user;
    }
}

void rtc_rtp_recv_stream_on_rtcp_activity(rtc_rtp_recv_stream_t *stream, uint64_t now_ms) {
    if (stream && stream->ssrc)
        stream->last_activity_ms = now_ms;
}

void rtc_rtp_recv_stream_set_mid(rtc_rtp_recv_stream_t *stream, const char *mid) {
    if (!stream)
        return;
    if (!mid || mid[0] == '\0') {
        stream->has_mid = false;
        stream->mid[0] = '\0';
        return;
    }
    size_t n = strlen(mid);
    if (n >= sizeof(stream->mid))
        n = sizeof(stream->mid) - 1;
    memcpy(stream->mid, mid, n);
    stream->mid[n] = '\0';
    stream->has_mid = true;
}

const char *rtc_rtp_recv_stream_mid(const rtc_rtp_recv_stream_t *stream) {
    return (stream && stream->has_mid) ? stream->mid : NULL;
}

bool rtc_rtp_recv_stream_mid_matches(const rtc_rtp_recv_stream_t *stream, const char *mid) {
    return stream && stream->has_mid && mid && strcmp(stream->mid, mid) == 0;
}

uint32_t rtc_rtp_recv_stream_ssrc(const rtc_rtp_recv_stream_t *stream) {
    return stream ? stream->ssrc : 0;
}

uint64_t rtc_rtp_recv_stream_source_generation(const rtc_rtp_recv_stream_t *stream) {
    return stream ? stream->source_generation : 0;
}

const rtc_rtcp_stats_t *rtc_rtp_recv_stream_stats(const rtc_rtp_recv_stream_t *stream) {
    return stream ? &stream->rtcp_stats : NULL;
}

void rtc_rtp_recv_stream_on_frame(rtc_rtp_recv_stream_t *stream, rtc_rtp_recv_stream_frame_fn fn,
                                  void *user) {
    if (!stream)
        return;
    stream->on_frame = fn;
    stream->on_frame_user = user;
}

static bool recv_vp8_keyframe_start(const uint8_t *payload, size_t len) {
    if (len < 2 || (payload[0] & 0x1f) != 0x10)
        return false;
    size_t offset = 1;
    if (payload[0] & 0x80) {
        uint8_t ext = payload[offset++];
        if (ext & 0x80) {
            if (offset >= len)
                return false;
            offset += (payload[offset] & 0x80) ? 2 : 1;
        }
        if (ext & 0x40)
            offset++;
        if (ext & 0x30)
            offset++;
    }
    return offset < len && !(payload[offset] & 1);
}

void rtc_rtp_recv_stream_on_packet(rtc_rtp_recv_stream_t *stream, const rtc_rtp_packet_t *pkt) {
    if (!stream || !pkt || !stream->active || !stream->on_frame)
        return;
    if (pkt->header.payload_type != stream->payload_type) {
        RTC_LOG_DBG("Ignoring RTP payload type %u on receiver configured for %u",
                    pkt->header.payload_type, stream->payload_type);
        return;
    }
    rtc_rtp_recv_stream_set_ssrc(stream, pkt->header.ssrc);
    uint64_t now = rtc_time_ms();
    stream->last_activity_ms = now;
    uint16_t seq = pkt->header.seq;
    if (stream->seq_initialized) {
        int16_t gap = (int16_t)(seq - stream->highest_seq);
        if (gap > 0) {
            if (gap > 64) {
                memset(stream->missing, 0, sizeof(stream->missing));
                stream->recovery = stream->clock_rate == 90000;
            } else {
                for (int i = 1; i < gap; i++) {
                    uint16_t lost = (uint16_t)(stream->highest_seq + i);
                    size_t slot = lost % 64;
                    if (stream->missing[slot].used)
                        stream->recovery = stream->clock_rate == 90000;
                    stream->missing[slot].used = true;
                    stream->missing[slot].seq = lost;
                    stream->missing[slot].attempts = 0;
                    stream->missing[slot].first_ms = now;
                    stream->missing[slot].last_ms = 0;
                }
            }
            stream->highest_seq = seq;
        }
    } else {
        stream->seq_initialized = true;
        stream->highest_seq = seq;
    }
    if (stream->missing[seq % 64].used && stream->missing[seq % 64].seq == seq)
        stream->missing[seq % 64].used = false;
    stream->last_packet_ms = now;
    if (stream->clock_rate == 90000 && pkt->payload_len) {
        if (!stream->frame_timestamp_valid ||
            (int32_t)(pkt->header.timestamp - stream->frame_timestamp) > 0) {
            stream->frame_timestamp_valid = true;
            stream->frame_timestamp = pkt->header.timestamp;
            stream->frame_marker_seen = false;
        }
        if (pkt->header.timestamp == stream->frame_timestamp) {
            stream->frame_marker_seen |= pkt->header.marker;
            stream->frame_open = !stream->frame_marker_seen;
        }
        if (pkt->header.timestamp == stream->frame_timestamp &&
            recv_vp8_keyframe_start(pkt->payload, pkt->payload_len)) {
            stream->recovery = false;
            for (size_t i = 0; i < 64; i++)
                if (stream->missing[i].used && (int16_t)(stream->missing[i].seq - seq) < 0)
                    stream->missing[i].used = false;
        }
    }
    rtc_rtp_recv_stream_tick(stream, now);
    rtc_rtcp_stats_on_rtp_recv(&stream->rtcp_stats, pkt->header.seq, pkt->header.timestamp,
                               pkt->header.ssrc, stream->clock_rate);
    stream->on_frame(pkt->payload, pkt->payload_len, pkt->header.seq, pkt->header.timestamp,
                     pkt->header.ssrc, pkt->header.marker, stream->on_frame_user);
}

void rtc_rtp_recv_stream_attach_transport(rtc_rtp_recv_stream_t *stream,
                                          rtc_transport_t *transport) {
    if (!stream || stream->transport == transport)
        return;
    rtc_transport_unregister_source(stream->transport, stream);
    rtc_transport_retain(transport);
    rtc_transport_release(stream->transport);
    stream->transport = transport;
}

void rtc_rtp_recv_stream_on_feedback(rtc_rtp_recv_stream_t *stream,
                                     rtc_rtp_recv_stream_feedback_fn fn, void *user) {
    if (stream) {
        stream->on_feedback = fn;
        stream->feedback_user = user;
    }
}

void rtc_rtp_recv_stream_set_feedback(rtc_rtp_recv_stream_t *stream, uint8_t feedback) {
    if (stream)
        stream->feedback = feedback & (RTC_SDP_FB_NACK | RTC_SDP_FB_PLI | RTC_SDP_FB_FIR);
}

static int recv_send_feedback(rtc_rtp_recv_stream_t *stream, uint8_t *buf, size_t len, size_t cap) {
    int rc = stream->on_feedback ? stream->on_feedback(buf, len, stream->feedback_user)
             : stream->transport ? rtc_transport_send_rtcp(stream->transport, buf, &len, cap)
                                 : RTC_ERR_INVALID;
    if (rc != RTC_OK)
        RTC_LOG_WARN("Receiver RTCP feedback failed: %d", rc);
    return rc;
}

static int recv_stream_request_keyframe(rtc_rtp_recv_stream_t *stream, uint64_t now) {
    if (!stream || !stream->active)
        return RTC_ERR_INVALID;
    if (!(stream->feedback & (RTC_SDP_FB_PLI | RTC_SDP_FB_FIR))) {
        RTC_LOG_WARN("Keyframe feedback was not negotiated");
        return RTC_ERR_INVALID;
    }
    stream->recovery = true;
    if (stream->last_pli_ms &&
        (now < stream->last_pli_ms || now - stream->last_pli_ms < RTC_PLI_MIN_INTERVAL_MS))
        return RTC_OK;
    stream->last_pli_ms = now;
    uint8_t buf[64 + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    size_t len;
    int rc = stream->feedback & RTC_SDP_FB_PLI
                 ? rtc_rtcp_build_pli(buf, sizeof(buf), &len, stream->rtcp_stats.ssrc, stream->ssrc)
                 : rtc_rtcp_build_fir(buf, sizeof(buf), &len, stream->rtcp_stats.ssrc, stream->ssrc,
                                      ++stream->fir_sequence);
    if (rc == RTC_OK)
        rc = recv_send_feedback(stream, buf, len, sizeof(buf));
    return rc;
}

typedef struct {
    rtc_rtp_recv_stream_t *stream;
    int result;
} stream_pli_args_t;

static void stream_pli_on_worker(void *user) {
    stream_pli_args_t *args = user;
    args->result = recv_stream_request_keyframe(args->stream, rtc_time_ms());
}

int rtc_rtp_recv_stream_request_keyframe(rtc_rtp_recv_stream_t *stream) {
    if (!stream)
        return RTC_ERR_INVALID;
    if (!stream->transport)
        return recv_stream_request_keyframe(stream, rtc_time_ms());
    stream_pli_args_t args = {.stream = stream, .result = RTC_ERR_INVALID};
    int rc = rtc_transport_invoke(stream->transport, stream_pli_on_worker, &args);
    return rc == RTC_OK ? args.result : rc;
}

void rtc_rtp_recv_stream_tick(rtc_rtp_recv_stream_t *stream, uint64_t now_ms) {
    if (!stream)
        return;
    if (stream->ssrc && stream->last_activity_ms && now_ms >= stream->last_activity_ms &&
        now_ms - stream->last_activity_ms >= rtc_rtcp_source_timeout_ms(&stream->rtcp_stats)) {
        uint32_t expired = stream->ssrc;
        if (stream->transport)
            rtc_transport_unbind_rtp(stream->transport, expired);
        rtc_rtp_recv_stream_set_ssrc(stream, 0);
        RTC_LOG_DBG("RTP receive source 0x%08x expired", expired);
        return;
    }
    if (!stream->active)
        return;
    uint16_t lost[64];
    int count = 0;
    for (size_t i = 0; i < 64; i++) {
        if (!stream->missing[i].used)
            continue;
        if (now_ms - stream->missing[i].first_ms >= 200) {
            stream->missing[i].used = false;
            stream->recovery = stream->clock_rate == 90000;
        } else if (stream->missing[i].attempts < 3 &&
                   (!stream->missing[i].last_ms || now_ms - stream->missing[i].last_ms >= 40)) {
            lost[count++] = stream->missing[i].seq;
            stream->missing[i].attempts++;
            stream->missing[i].last_ms = now_ms;
        }
    }
    if (count && (stream->feedback & RTC_SDP_FB_NACK)) {
        uint8_t buf[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
        size_t len;
        if (rtc_rtcp_build_nack(buf, sizeof(buf), &len, stream->rtcp_stats.ssrc, stream->ssrc, lost,
                                count) == RTC_OK)
            (void)recv_send_feedback(stream, buf, len, sizeof(buf));
    }
    if (stream->frame_open && now_ms - stream->last_packet_ms >= 200)
        stream->recovery = true;
    if (stream->recovery && (stream->feedback & (RTC_SDP_FB_PLI | RTC_SDP_FB_FIR)))
        (void)recv_stream_request_keyframe(stream, now_ms);
}

void rtc_rtp_recv_stream_on_sr(rtc_rtp_recv_stream_t *stream, const rtc_rtcp_sr_t *sr) {
    if (stream) {
        rtc_rtp_recv_stream_on_rtcp_activity(stream, rtc_time_ms());
        rtc_rtcp_stats_on_sr_recv(&stream->rtcp_stats, sr);
    }
}

void rtc_rtp_recv_stream_emit_rr(rtc_rtp_recv_stream_t *stream, rtc_transport_t *transport) {
    if (!stream || !stream->active || !transport || stream->rtcp_stats.packets_received == 0)
        return;
    emit_rtcp_report(&stream->rtcp_stats, transport, stream->rtcp_stats.packets_sent != 0);
}
