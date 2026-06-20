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
    bool send_active;

    rtc_rtp_send_stream_nack_fn on_nack;
    void *on_nack_user;
    rtc_rtp_send_stream_pli_fn on_pli;
    void *on_pli_user;

    uint64_t last_pli_handled_ms;

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
    uint32_t ssrc;
    rtc_rtcp_stats_t rtcp_stats;
    bool active;
};

rtc_rtp_send_stream_t *rtc_rtp_send_stream_create(const rtc_rtp_send_stream_config_t *cfg) {
    if (!cfg)
        return NULL;
    rtc_rtp_send_stream_t *stream = (rtc_rtp_send_stream_t *)calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;
    stream->active = true;
    stream->send_active = true;
    if (rtc_rtp_session_init(&stream->rtp_session, cfg->payload_type, cfg->clock_rate) != RTC_OK ||
        rtc_rtcp_stats_init(&stream->rtcp_stats, stream->rtp_session.ssrc) != RTC_OK) {
        free(stream);
        return NULL;
    }
    return stream;
}

void rtc_rtp_send_stream_destroy(rtc_rtp_send_stream_t *stream) {
    if (!stream)
        return;
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (stream->rate_ctrl)
        rtc_rate_control_destroy(stream->rate_ctrl);
#endif
    if (stream->nack_buf)
        rtc_nack_buf_destroy(stream->nack_buf);
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
    if (rtc_rtcp_stats_init(&stream->rtcp_stats, cfg->local_ssrc) != RTC_OK) {
        free(stream);
        return NULL;
    }
    return stream;
}

void rtc_rtp_recv_stream_destroy(rtc_rtp_recv_stream_t *stream) {
    free(stream);
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
    return stream ? &stream->rtcp_stats : NULL;
}

void rtc_rtp_send_stream_attach_transport(rtc_rtp_send_stream_t *stream,
                                          rtc_transport_t *transport) {
    if (!stream || !stream->active)
        return;
    stream->transport = transport;
}

void rtc_rtp_send_stream_attach_twcc(rtc_rtp_send_stream_t *stream, uint8_t ext_id) {
#ifdef MRTC_ENABLE_TWCC
    if (!stream || !stream->active || ext_id == 0 || !stream->transport)
        return;
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
#ifdef MRTC_ENABLE_RATE_CONTROL
    rtc_rate_control_config_t rc_cfg = {
        .target_bitrate_kbps = 500,
        .min_bitrate_kbps = 100,
        .max_bitrate_kbps = 2500,
    };
    stream->rate_ctrl = rtc_rate_control_create(&rc_cfg);
#endif
    stream->nack_buf = rtc_nack_buf_create(NACK_BUF_DEFAULT_SIZE);
}

void rtc_rtp_send_stream_set_send_active(rtc_rtp_send_stream_t *stream, bool active) {
    if (stream)
        stream->send_active = active;
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

int rtc_rtp_send_stream_send(rtc_rtp_send_stream_t *stream, const uint8_t *payload, size_t len,
                             uint32_t samples, bool marker) {
    if (!stream || !stream->active)
        return RTC_ERR_INVALID;
    if (!stream->transport)
        return RTC_ERR_INVALID;
    if (!stream->send_active)
        return RTC_OK;

    rtc_rtp_packet_t pkt;
    int rc;
#ifdef MRTC_ENABLE_TWCC
    uint16_t twcc_seq = 0;
    bool tagged = (stream->twcc && stream->twcc_ext_id != 0);
    if (tagged) {
        twcc_seq = atomic_fetch_add(&stream->twcc->next_seq, 1);
        rtc_rtp_ext_t exts[1];
        rtc_rtp_ext_make_transport_cc(&exts[0], stream->twcc_ext_id, twcc_seq);
        rc = rtc_rtp_session_send_with_ext(&stream->rtp_session, &pkt, exts, 1, payload, len,
                                           samples, marker);
    } else {
        rc = rtc_rtp_session_send(&stream->rtp_session, &pkt, payload, len, samples, marker);
    }
#else
    rc = rtc_rtp_session_send(&stream->rtp_session, &pkt, payload, len, samples, marker);
#endif
    if (rc != RTC_OK)
        return rc;

    rtc_rtcp_stats_on_rtp_send(&stream->rtcp_stats, pkt.header.timestamp, len);

    size_t pkt_len = pkt.buf_len;
    rc = rtc_transport_send_rtp(stream->transport, pkt.buf, &pkt_len, sizeof(pkt.buf));
    if (rc != RTC_OK)
        return rc;

#ifdef MRTC_ENABLE_TWCC
    if (tagged) {
        rtc_twcc_sent_pkt_t *entry = &stream->twcc->ring[twcc_seq & (RTC_TWCC_SENDER_RING - 1)];
        entry->seq = twcc_seq;
        entry->size = (uint16_t)pkt_len;
        entry->send_time_us = rtc_time_us();
        entry->used = true;
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

int rtc_rtp_send_stream_get_target_bitrate(const rtc_rtp_send_stream_t *stream) {
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (!stream || !stream->rate_ctrl)
        return stream && stream->max_bitrate_bps ? (int)(stream->max_bitrate_bps / 1000) : 0;
    int kbps = rtc_rate_control_get_bitrate(stream->rate_ctrl);
    if (stream->max_bitrate_bps) {
        int cap_kbps = (int)(stream->max_bitrate_bps / 1000);
        if (cap_kbps > 0 && kbps > cap_kbps)
            kbps = cap_kbps;
    }
    return kbps;
#else
    if (stream && stream->max_bitrate_bps)
        return (int)(stream->max_bitrate_bps / 1000);
    (void)stream;
    return 0;
#endif
}

bool rtc_rtp_send_stream_should_keyframe(rtc_rtp_send_stream_t *stream) {
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
        rtc_transport_send_raw(stream->transport, pkt, pkt_len);
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

#ifdef MRTC_ENABLE_RATE_CONTROL
    if (stream->rate_ctrl)
        atomic_store_explicit(&stream->rate_ctrl->keyframe_requested, true, memory_order_release);
#endif

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
static void emit_rtcp_report(rtc_rtcp_stats_t *stats, rtc_transport_t *transport,
                             int (*build)(rtc_rtcp_packet_t *, const rtc_rtcp_stats_t *)) {
    rtc_rtcp_packet_t pkt;
    if (build(&pkt, stats) != RTC_OK)
        return;
    uint8_t buf[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    if (pkt.buf_len > sizeof(buf))
        return;
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;
    if (rtc_transport_send_rtcp(transport, buf, &len, sizeof(buf)) != RTC_OK)
        return;
    stats->last_report_time = rtc_time_ms();
}

void rtc_rtp_send_stream_emit_sr(rtc_rtp_send_stream_t *stream, rtc_transport_t *transport) {
    if (!stream || !stream->active || !transport || stream->rtcp_stats.packets_sent == 0)
        return;
    emit_rtcp_report(&stream->rtcp_stats, transport, rtc_rtcp_build_sr);
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
    if (stream)
        stream->ssrc = ssrc;
}

uint32_t rtc_rtp_recv_stream_ssrc(const rtc_rtp_recv_stream_t *stream) {
    return stream ? stream->ssrc : 0;
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

void rtc_rtp_recv_stream_on_packet(rtc_rtp_recv_stream_t *stream, const rtc_rtp_packet_t *pkt) {
    if (!stream || !pkt || !stream->active || !stream->on_frame)
        return;
    rtc_rtcp_stats_on_rtp_recv(&stream->rtcp_stats, pkt->header.seq, pkt->header.timestamp,
                               pkt->header.ssrc, stream->clock_rate);
    stream->on_frame(pkt->payload, pkt->payload_len, pkt->header.seq, pkt->header.timestamp,
                     pkt->header.ssrc, pkt->header.marker, stream->on_frame_user);
}

void rtc_rtp_recv_stream_on_sr(rtc_rtp_recv_stream_t *stream, const rtc_rtcp_sr_t *sr) {
    if (stream)
        rtc_rtcp_stats_on_sr_recv(&stream->rtcp_stats, sr);
}

void rtc_rtp_recv_stream_emit_rr(rtc_rtp_recv_stream_t *stream, rtc_transport_t *transport) {
    if (!stream || !stream->active || !transport || stream->rtcp_stats.packets_received == 0)
        return;
    emit_rtcp_report(&stream->rtcp_stats, transport, rtc_rtcp_build_rr);
}
