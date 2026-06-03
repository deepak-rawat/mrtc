/*
 * rtc_track.c - Track / Transceiver implementation.
 *
 * Sender send path, receiver callbacks, transceiver accessors, plus the
 * per-transceiver lifecycle helpers called by rtc_peer.c (init/close/attach/
 * arm/activate/emit_sr/emit_rr/fill_sdp_media).  Struct definitions are in
 * rtc_peer_internal.h (shared with rtc_peer*.c).
 */
#include "rtc_peer_internal.h"
#include "rtc_rtp_ext.h"

#include <stdio.h>
#include <string.h>

/* ---- RTCRtpSender ---- */

int rtc_rtp_sender_send(rtc_rtp_sender_t *sender, const uint8_t *payload, size_t len,
                        uint32_t samples, bool marker) {
    if (!sender || !sender->active)
        return RTC_ERR_INVALID;
    if (!sender->srtp || !sender->transport)
        return RTC_ERR_INVALID;
    /* setParameters({active:false}) suspends transmission. Return OK so
     * callers can keep feeding frames without treating it as an error. */
    if (!sender->send_active)
        return RTC_OK;

    /* Build RTP packet */
    rtc_rtp_packet_t pkt;
    int rc;
#ifdef MRTC_ENABLE_TWCC
    uint16_t twcc_seq = 0;
    bool tagged = (sender->twcc && sender->twcc_ext_id != 0);
    if (tagged) {
        rtc_twcc_sender_t *twcc = (rtc_twcc_sender_t *)sender->twcc;
        twcc_seq = atomic_fetch_add(&twcc->next_seq, 1);
        rtc_rtp_ext_t exts[1];
        rtc_rtp_ext_make_transport_cc(&exts[0], sender->twcc_ext_id, twcc_seq);
        rc = rtc_rtp_session_send_with_ext(&sender->rtp_session, &pkt, exts, 1, payload, len,
                                           samples, marker);
    } else {
        rc = rtc_rtp_session_send(&sender->rtp_session, &pkt, payload, len, samples, marker);
    }
#else
    rc = rtc_rtp_session_send(&sender->rtp_session, &pkt, payload, len, samples, marker);
#endif
    if (rc != RTC_OK)
        return rc;

    /* Update RTCP sender stats */
    rtc_rtcp_stats_on_rtp_send(&sender->rtcp_stats, pkt.header.timestamp, len);

    /* SRTP protect. The sender's SRTP context is also touched by the
     * transport thread (RTCP SR/RR + TWCC feedback timers), so this call
     * relies on the per-context mutex inside rtc_srtp_protect. */
    size_t pkt_len = pkt.buf_len;
    rc = rtc_srtp_protect(sender->srtp, pkt.buf, &pkt_len, sizeof(pkt.buf));
    if (rc != RTC_OK)
        return rc;

    /* Record send-time + wire size in TWCC sender ring */
#ifdef MRTC_ENABLE_TWCC
    if (tagged) {
        rtc_twcc_sender_t *twcc = (rtc_twcc_sender_t *)sender->twcc;
        rtc_twcc_sent_pkt_t *e = &twcc->ring[twcc_seq & (RTC_TWCC_SENDER_RING - 1)];
        e->seq = twcc_seq;
        e->size = (uint16_t)pkt_len;
        e->send_time_us = rtc_time_us();
        e->used = true;
    }
#endif

    /* Store post-SRTP packet in NACK buffer for retransmission.
     * Stash the TWCC seq (if any) so retransmits can invalidate the
     * matching slot in the TWCC sender ring; otherwise BWE would pair
     * the retransmit's arrival time with the original's send time. */
    if (sender->nack_buf) {
#ifdef MRTC_ENABLE_TWCC
        rtc_nack_buf_store(sender->nack_buf, pkt.buf, pkt_len, pkt.header.seq, tagged, twcc_seq);
#else
        rtc_nack_buf_store(sender->nack_buf, pkt.buf, pkt_len, pkt.header.seq, false, 0);
#endif
    }

    /* Send via transport (sendto is thread-safe on UDP sockets) */
    rtc_transport_t *t = (rtc_transport_t *)sender->transport;
    return rtc_transport_send_to_remote(t, pkt.buf, pkt_len);
}

const rtc_codec_t *rtc_rtp_sender_get_codec(const rtc_rtp_sender_t *sender) {
    return sender ? &sender->codec : NULL;
}

rtc_kind_t rtc_rtp_sender_kind(const rtc_rtp_sender_t *sender) {
    return sender ? sender->kind : RTC_KIND_AUDIO;
}

int rtc_rtp_sender_get_target_bitrate(const rtc_rtp_sender_t *sender) {
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (!sender || !sender->rate_ctrl)
        return sender && sender->max_bitrate_bps ? (int)(sender->max_bitrate_bps / 1000) : 0;
    int kbps = rtc_rate_control_get_bitrate(sender->rate_ctrl);
    if (sender->max_bitrate_bps) {
        int cap_kbps = (int)(sender->max_bitrate_bps / 1000);
        if (cap_kbps > 0 && kbps > cap_kbps)
            kbps = cap_kbps;
    }
    return kbps;
#else
    if (sender && sender->max_bitrate_bps)
        return (int)(sender->max_bitrate_bps / 1000);
    (void)sender;
    return 0;
#endif
}

bool rtc_rtp_sender_should_keyframe(rtc_rtp_sender_t *sender) {
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (!sender || !sender->rate_ctrl)
        return false;
    return rtc_rate_control_should_keyframe(sender->rate_ctrl);
#else
    (void)sender;
    return false;
#endif
}

/* ---- Sender feedback callbacks ---- */

void rtc_rtp_sender_on_nack(rtc_rtp_sender_t *sender, rtc_on_nack_fn fn, void *user) {
    if (!sender)
        return;
    sender->on_nack = fn;
    sender->on_nack_user = user;
}

void rtc_rtp_sender_on_pli(rtc_rtp_sender_t *sender, rtc_on_pli_fn fn, void *user) {
    if (!sender)
        return;
    sender->on_pli = fn;
    sender->on_pli_user = user;
}

int rtc_rtp_sender_get_parameters(const rtc_rtp_sender_t *sender, rtc_rtp_send_params_t *params) {
    if (!sender || !params)
        return RTC_ERR_INVALID;
    memset(params, 0, sizeof(*params));
    params->encoding_count = 1;
    params->encodings[0].active = sender->send_active;
    params->encodings[0].max_bitrate_bps = sender->max_bitrate_bps;
    return RTC_OK;
}

int rtc_rtp_sender_set_parameters(rtc_rtp_sender_t *sender, const rtc_rtp_send_params_t *params) {
    if (!sender || !params || params->encoding_count != 1)
        return RTC_ERR_INVALID;
    sender->send_active = params->encodings[0].active;
    sender->max_bitrate_bps = params->encodings[0].max_bitrate_bps;
    return RTC_OK;
}

/* ---- Internal handlers (called by peer connection on RTCP feedback) ---- */

/* Minimum interval between honored PLIs from the same remote (ms). Protects
 * the encoder against keyframe storms from a buggy or hostile peer. */
#define RTC_PLI_MIN_INTERVAL_MS 500

void rtc_rtp_sender_handle_nack(rtc_rtp_sender_t *sender, const uint16_t *lost_seqs, int count) {
    if (!sender || !lost_seqs || count <= 0)
        return;
    if (!sender->nack_buf || !sender->transport)
        goto notify;

    rtc_transport_t *t = (rtc_transport_t *)sender->transport;
    for (int i = 0; i < count; i++) {
        const uint8_t *pkt;
        size_t pkt_len;
        uint16_t twcc_seq = 0;
        if (!rtc_nack_buf_retransmit(sender->nack_buf, lost_seqs[i], &pkt, &pkt_len, &twcc_seq))
            continue; /* not in buffer or per-seq retransmit cap hit */
        rtc_transport_send_to_remote(t, pkt, pkt_len);
#ifdef MRTC_ENABLE_TWCC
        /* The retransmit carries the original TWCC seq. Drop it from the
         * sender ring so handle_rtcp_twcc skips this seq when feedback
         * arrives — otherwise BWE pairs the retransmit's arrival time
         * with the original's send time and underestimates bandwidth. */
        if (twcc_seq != 0 && sender->twcc) {
            rtc_twcc_sender_t *twcc = (rtc_twcc_sender_t *)sender->twcc;
            rtc_twcc_sender_invalidate(twcc, twcc_seq);
        }
#else
        (void)twcc_seq;
#endif
    }

notify:
    /* Notify application callback */
    if (sender->on_nack)
        sender->on_nack(lost_seqs, count, sender->on_nack_user);
}

void rtc_rtp_sender_handle_pli(rtc_rtp_sender_t *sender) {
    if (!sender)
        return;

    /* Rate-limit: a peer can legitimately request a keyframe, but at most
     * once per RTC_PLI_MIN_INTERVAL_MS. Otherwise the encoder would be
     * forced into back-to-back keyframes, blowing the bitrate budget. */
    uint64_t now = rtc_time_ms();
    if (now - sender->last_pli_handled_ms < RTC_PLI_MIN_INTERVAL_MS)
        return;
    sender->last_pli_handled_ms = now;

    /* Request keyframe via rate controller */
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (sender->rate_ctrl)
        atomic_store_explicit(&sender->rate_ctrl->keyframe_requested, true, memory_order_release);
#endif

    if (sender->on_pli)
        sender->on_pli(sender->on_pli_user);
}

/* ---- RTCRtpReceiver ---- */

void rtc_rtp_receiver_on_frame(rtc_rtp_receiver_t *receiver, rtc_on_frame_fn fn, void *user) {
    if (!receiver)
        return;
    receiver->on_frame = fn;
    receiver->on_frame_user = user;
}

rtc_kind_t rtc_rtp_receiver_kind(const rtc_rtp_receiver_t *receiver) {
    return receiver ? receiver->kind : RTC_KIND_AUDIO;
}

const rtc_codec_t *rtc_rtp_receiver_get_codec(const rtc_rtp_receiver_t *receiver) {
    return receiver ? &receiver->codec : NULL;
}

/* ---- RTCRtpTransceiver ---- */

rtc_direction_t rtc_rtp_transceiver_direction(const rtc_rtp_transceiver_t *t) {
    return t ? t->direction : RTC_DIR_INACTIVE;
}

void rtc_rtp_transceiver_set_direction(rtc_rtp_transceiver_t *t, rtc_direction_t dir) {
    if (t)
        t->direction = dir;
}

rtc_rtp_sender_t *rtc_rtp_transceiver_sender(rtc_rtp_transceiver_t *t) {
    return t ? &t->sender : NULL;
}

rtc_rtp_receiver_t *rtc_rtp_transceiver_receiver(rtc_rtp_transceiver_t *t) {
    return t ? &t->receiver : NULL;
}

const char *rtc_rtp_transceiver_mid(const rtc_rtp_transceiver_t *t) {
    return t ? t->mid : NULL;
}

/* ---- Per-transceiver lifecycle helpers (called by rtc_peer.c) ---- */

void rtc_rtp_transceiver_init_slot(struct rtc_rtp_transceiver *t, int mid_index, rtc_kind_t kind,
                                   const rtc_codec_t *codec) {
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->direction = RTC_DIR_SENDRECV;
    t->mid_index = mid_index;
    snprintf(t->mid, sizeof(t->mid), "%d", mid_index);

    /* Sender */
    t->sender.codec = *codec;
    t->sender.kind = kind;
    t->sender.active = true;
    t->sender.send_active = true;
    t->sender.max_bitrate_bps = 0;
    rtc_rtp_session_init(&t->sender.rtp_session, codec->payload_type, codec->clock_rate);
    rtc_rtcp_stats_init(&t->sender.rtcp_stats, t->sender.rtp_session.ssrc);

    /* Receiver (activated when remote description arrives). RTCP stats are
     * initialized with the sender SSRC so the rtcp_stats struct is valid;
     * the real receiver SSRC is patched in once it is learned from SDP. */
    t->receiver.codec = *codec;
    t->receiver.kind = kind;
    t->receiver.active = false;
    rtc_rtcp_stats_init(&t->receiver.rtcp_stats, t->sender.rtp_session.ssrc);
}

void rtc_rtp_transceiver_close_resources(struct rtc_rtp_transceiver *t) {
    struct rtc_rtp_sender *s = &t->sender;
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (s->rate_ctrl) {
        rtc_rate_control_destroy(s->rate_ctrl);
        s->rate_ctrl = NULL;
    }
#endif
    if (s->nack_buf) {
        rtc_nack_buf_destroy(s->nack_buf);
        s->nack_buf = NULL;
    }
}

void rtc_rtp_sender_attach(struct rtc_rtp_sender *s, rtc_srtp_ctx_t *srtp_send,
                           rtc_transport_t *transport) {
    if (!s || !s->active)
        return;
    s->srtp = srtp_send;
    s->transport = transport;
}

void rtc_rtp_sender_attach_twcc(struct rtc_rtp_sender *s, void *twcc_sender, uint8_t ext_id) {
#ifdef MRTC_ENABLE_TWCC
    if (!s || !s->active || ext_id == 0)
        return;
    s->twcc = twcc_sender;
    s->twcc_ext_id = ext_id;
#else
    (void)s;
    (void)twcc_sender;
    (void)ext_id;
#endif
}

void rtc_rtp_sender_arm_video(struct rtc_rtp_sender *s) {
    if (!s || !s->active || s->kind != RTC_KIND_VIDEO)
        return;
#ifdef MRTC_ENABLE_RATE_CONTROL
    rtc_rate_control_config_t rc_cfg = {
        .target_bitrate_kbps = 500,
        .min_bitrate_kbps = 100,
        .max_bitrate_kbps = 2500,
    };
    s->rate_ctrl = rtc_rate_control_create(&rc_cfg);
#endif
    s->nack_buf = rtc_nack_buf_create(NACK_BUF_DEFAULT_SIZE);
}

void rtc_rtp_receiver_activate(struct rtc_rtp_receiver *r) {
    if (r)
        r->active = true;
}

/* SR / RR build + SRTCP protect + send. The scratch buffer is sized to hold
 * the largest RTCP packet plus the 4-byte SRTCP index trailer plus the auth
 * tag (see rtc_srtp_protect_rtcp). */
void rtc_rtp_sender_emit_sr(struct rtc_rtp_sender *s, rtc_srtp_ctx_t *srtp_send,
                            rtc_transport_t *transport) {
    if (!s || !s->active || s->rtcp_stats.packets_sent == 0)
        return;
    rtc_rtcp_packet_t pkt;
    if (rtc_rtcp_build_sr(&pkt, &s->rtcp_stats) != RTC_OK)
        return;
    uint8_t buf[RTCP_MAX_PACKET + 4 + SRTP_AUTH_TAG_LEN];
    if (pkt.buf_len > sizeof(buf))
        return;
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;
    if (rtc_srtp_protect_rtcp(srtp_send, buf, &len, sizeof(buf)) != RTC_OK)
        return;
    rtc_transport_send_to_remote(transport, buf, len);
    s->rtcp_stats.last_report_time = rtc_time_ms();
}

void rtc_rtp_receiver_emit_rr(struct rtc_rtp_receiver *r, rtc_srtp_ctx_t *srtp_send,
                              rtc_transport_t *transport) {
    if (!r || !r->active || r->rtcp_stats.packets_received == 0)
        return;
    rtc_rtcp_packet_t pkt;
    if (rtc_rtcp_build_rr(&pkt, &r->rtcp_stats) != RTC_OK)
        return;
    uint8_t buf[RTCP_MAX_PACKET + 4 + SRTP_AUTH_TAG_LEN];
    if (pkt.buf_len > sizeof(buf))
        return;
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;
    if (rtc_srtp_protect_rtcp(srtp_send, buf, &len, sizeof(buf)) != RTC_OK)
        return;
    rtc_transport_send_to_remote(transport, buf, len);
    r->rtcp_stats.last_report_time = rtc_time_ms();
}

void rtc_rtp_transceiver_fill_sdp_media(const struct rtc_rtp_transceiver *t, rtc_sdp_media_t *m) {
    memset(m, 0, sizeof(*m));

    const char *mime = t->sender.codec.mime_type;
    if (strncmp(mime, "audio/", 6) == 0) {
        m->media_type = RTC_MEDIA_AUDIO;
        size_t clen = strlen(mime + 6);
        if (clen >= sizeof(m->codec_name))
            clen = sizeof(m->codec_name) - 1;
        memcpy(m->codec_name, mime + 6, clen);
        m->codec_name[clen] = '\0';
    } else if (strncmp(mime, "video/", 6) == 0) {
        m->media_type = RTC_MEDIA_VIDEO;
        size_t clen = strlen(mime + 6);
        if (clen >= sizeof(m->codec_name))
            clen = sizeof(m->codec_name) - 1;
        memcpy(m->codec_name, mime + 6, clen);
        m->codec_name[clen] = '\0';
    } else {
        m->media_type = RTC_MEDIA_APPLICATION;
    }

    m->payload_type = t->sender.codec.payload_type;
    m->clockrate = (int)t->sender.codec.clock_rate;
    m->channels = t->sender.codec.channels;
    m->mid_index = t->mid_index;
    m->ssrc = t->sender.rtp_session.ssrc;

#ifdef MRTC_ENABLE_TWCC
    if (m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) {
        rtc_sdp_media_add_extmap(m, 5, RTC_EXT_URI_TRANSPORT_CC);
    }
#endif
}
