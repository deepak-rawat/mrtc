/*
 * rtc_track.c - Track / Transceiver implementation.
 *
 * Sender send path, receiver callbacks, transceiver accessors.
 * Struct definitions are in rtc_peer_internal.h (shared with rtc_peer*.c).
 */
#include "rtc_peer_internal.h"

#include <string.h>

/* ---- RTCRtpSender ---- */

int rtc_rtp_sender_send(rtc_rtp_sender_t *sender, const uint8_t *payload, size_t len,
                        uint32_t samples, bool marker) {
    if (!sender || !sender->active)
        return RTC_ERR_INVALID;
    if (!sender->srtp || !sender->transport)
        return RTC_ERR_INVALID;

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

    /* SRTP protect (sender SRTP ctx only touched from main thread) */
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

    /* Store post-SRTP packet in NACK buffer for retransmission */
    if (sender->nack_buf)
        rtc_nack_buf_store(sender->nack_buf, pkt.buf, pkt_len, pkt.header.seq);

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
        return 0;
    return rtc_rate_control_get_bitrate(sender->rate_ctrl);
#else
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

/* ---- Internal handlers (called by peer connection on RTCP feedback) ---- */

void rtc_rtp_sender_handle_nack(rtc_rtp_sender_t *sender, const uint16_t *lost_seqs, int count) {
    if (!sender || !lost_seqs || count <= 0)
        return;

    /* Retransmit from NACK buffer */
    if (sender->nack_buf && sender->transport) {
        rtc_transport_t *t = (rtc_transport_t *)sender->transport;
        for (int i = 0; i < count; i++) {
            const uint8_t *pkt;
            size_t pkt_len;
            if (rtc_nack_buf_get(sender->nack_buf, lost_seqs[i], &pkt, &pkt_len))
                rtc_transport_send_to_remote(t, pkt, pkt_len);
        }
    }

    /* Notify application callback */
    if (sender->on_nack)
        sender->on_nack(lost_seqs, count, sender->on_nack_user);
}

void rtc_rtp_sender_handle_pli(rtc_rtp_sender_t *sender) {
    if (!sender)
        return;

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
