/*
 * rtc_track.c - Track / Transceiver implementation.
 *
 * Internal struct definitions for rtc_rtp_sender, rtc_rtp_receiver,
 * and rtc_rtp_transceiver. These are opaque to callers.
 */
#include "rtc/rtc_track.h"
#include "rtc_rtp.h"
#include "rtc_rtcp.h"
#include "rtc_rate_control.h"
#include "rtc_transport.h"
#include "rtc_srtp.h"

#include <string.h>

/* ---- Internal struct definitions ---- */

struct rtc_rtp_sender {
    rtc_codec_t codec;
    rtc_kind_t kind;
    rtc_rtp_session_t rtp_session;
    rtc_srtp_ctx_t *srtp; /* borrowed from peer, set after CONNECTED */
    void *transport;      /* borrowed rtc_transport_t* for sendto */
    rtc_rtcp_stats_t rtcp_stats;
    rtc_rate_controller_t *rate_ctrl; /* borrowed from peer, set after CONNECTED */
    bool active;
};

struct rtc_rtp_receiver {
    rtc_codec_t codec;
    rtc_kind_t kind;
    rtc_on_frame_fn on_frame;
    void *on_frame_user;
    uint32_t ssrc; /* remote SSRC to match */
    rtc_rtcp_stats_t rtcp_stats;
    bool active;
};

struct rtc_rtp_transceiver {
    struct rtc_rtp_sender sender;
    struct rtc_rtp_receiver receiver;
    rtc_direction_t direction;
    char mid[32];
    int mid_index;
    bool used;
};

/* ---- RTCRtpSender ---- */

int rtc_rtp_sender_send(rtc_rtp_sender_t *sender, const uint8_t *payload, size_t len,
                        uint32_t samples, bool marker) {
    if (!sender || !sender->active)
        return RTC_ERR_INVALID;
    if (!sender->srtp || !sender->transport)
        return RTC_ERR_INVALID;

    /* Build RTP packet */
    rtc_rtp_packet_t pkt;
    int rc = rtc_rtp_session_send(&sender->rtp_session, &pkt, payload, len, samples, marker);
    if (rc != RTC_OK)
        return rc;

    /* Update RTCP sender stats */
    rtc_rtcp_stats_on_rtp_send(&sender->rtcp_stats, pkt.header.timestamp, len);

    /* SRTP protect (sender SRTP ctx only touched from main thread) */
    size_t pkt_len = pkt.buf_len;
    rc = rtc_srtp_protect(sender->srtp, pkt.buf, &pkt_len, sizeof(pkt.buf));
    if (rc != RTC_OK)
        return rc;

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
    if (!sender || !sender->rate_ctrl)
        return 0;
    return rtc_rate_control_get_bitrate(sender->rate_ctrl);
}

bool rtc_rtp_sender_should_keyframe(rtc_rtp_sender_t *sender) {
    if (!sender || !sender->rate_ctrl)
        return false;
    return rtc_rate_control_should_keyframe(sender->rate_ctrl);
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
