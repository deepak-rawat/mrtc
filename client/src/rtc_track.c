/*
 * Track / Transceiver implementation.
 *
 * Sender send path, receiver callbacks, transceiver accessors, plus the
 * per-transceiver lifecycle helpers called by rtc_peer.c (init/close/attach/
 * arm/activate/emit_sr/emit_rr/fill_sdp_media).  Struct definitions are in
 * rtc_peer_internal.h (shared with rtc_peer*.c).
 */
#include "rtc_peer_internal.h"

#include <stdio.h>
#include <string.h>

int rtc_rtp_sender_send(rtc_rtp_sender_t *sender, const uint8_t *payload, size_t len,
                        uint32_t samples, bool marker) {
    return sender ? rtc_rtp_send_stream_send(sender->stream, payload, len, samples, marker)
                  : RTC_ERR_INVALID;
}

const rtc_codec_t *rtc_rtp_sender_get_codec(const rtc_rtp_sender_t *sender) {
    return sender ? &sender->codec : NULL;
}

rtc_kind_t rtc_rtp_sender_kind(const rtc_rtp_sender_t *sender) {
    return sender ? sender->kind : RTC_KIND_AUDIO;
}

int rtc_rtp_sender_get_target_bitrate(const rtc_rtp_sender_t *sender) {
    return sender ? rtc_rtp_send_stream_get_target_bitrate(sender->stream) : 0;
}

bool rtc_rtp_sender_should_keyframe(rtc_rtp_sender_t *sender) {
    return sender ? rtc_rtp_send_stream_should_keyframe(sender->stream) : false;
}

void rtc_rtp_sender_on_nack(rtc_rtp_sender_t *sender, rtc_on_nack_fn fn, void *user) {
    if (sender)
        rtc_rtp_send_stream_on_nack(sender->stream, fn, user);
}

void rtc_rtp_sender_on_pli(rtc_rtp_sender_t *sender, rtc_on_pli_fn fn, void *user) {
    if (sender)
        rtc_rtp_send_stream_on_pli(sender->stream, fn, user);
}

int rtc_rtp_sender_get_parameters(const rtc_rtp_sender_t *sender, rtc_rtp_send_params_t *params) {
    if (!sender || !params)
        return RTC_ERR_INVALID;
    memset(params, 0, sizeof(*params));
    params->encoding_count = 1;
    params->encodings[0].active = rtc_rtp_send_stream_send_active(sender->stream);
    params->encodings[0].max_bitrate_bps = rtc_rtp_send_stream_max_bitrate(sender->stream);
    return RTC_OK;
}

int rtc_rtp_sender_set_parameters(rtc_rtp_sender_t *sender, const rtc_rtp_send_params_t *params) {
    if (!sender || !params || params->encoding_count != 1)
        return RTC_ERR_INVALID;
    rtc_rtp_send_stream_set_send_active(sender->stream, params->encodings[0].active);
    rtc_rtp_send_stream_set_max_bitrate(sender->stream, params->encodings[0].max_bitrate_bps);
    return RTC_OK;
}

void rtc_rtp_receiver_on_frame(rtc_rtp_receiver_t *receiver, rtc_on_frame_fn fn, void *user) {
    if (receiver)
        rtc_rtp_recv_stream_on_frame(receiver->stream, fn, user);
}

rtc_kind_t rtc_rtp_receiver_kind(const rtc_rtp_receiver_t *receiver) {
    return receiver ? receiver->kind : RTC_KIND_AUDIO;
}

const rtc_codec_t *rtc_rtp_receiver_get_codec(const rtc_rtp_receiver_t *receiver) {
    return receiver ? &receiver->codec : NULL;
}

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

/* Per-transceiver lifecycle helpers, called by rtc_peer.c. */
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
    t->sender.stream = rtc_rtp_send_stream_create(&(rtc_rtp_send_stream_config_t){
        .payload_type = codec->payload_type,
        .clock_rate = codec->clock_rate,
    });

    /* Receiver (activated when remote description arrives). RTCP stats are
     * initialized with the sender SSRC so the rtcp_stats struct is valid;
     * the real receiver SSRC is patched in once it is learned from SDP. */
    t->receiver.codec = *codec;
    t->receiver.kind = kind;
    t->receiver.stream = rtc_rtp_recv_stream_create(&(rtc_rtp_recv_stream_config_t){
        .payload_type = codec->payload_type,
        .clock_rate = codec->clock_rate,
        .local_ssrc = rtc_rtp_send_stream_ssrc(t->sender.stream),
    });
}

void rtc_rtp_transceiver_close_resources(struct rtc_rtp_transceiver *t) {
    if (!t)
        return;
    rtc_rtp_send_stream_destroy(t->sender.stream);
    rtc_rtp_recv_stream_destroy(t->receiver.stream);
    t->sender.stream = NULL;
    t->receiver.stream = NULL;
}

void rtc_rtp_sender_attach_logical(struct rtc_rtp_sender *s, rtc_transport_t *transport) {
    if (s)
        rtc_rtp_send_stream_attach_transport(s->stream, transport);
}

void rtc_rtp_sender_attach_twcc(struct rtc_rtp_sender *s, uint8_t ext_id) {
    if (s)
        rtc_rtp_send_stream_attach_twcc(s->stream, ext_id);
}

void rtc_rtp_sender_arm_video(struct rtc_rtp_sender *s) {
    if (!s || s->kind != RTC_KIND_VIDEO)
        return;
    rtc_rtp_send_stream_arm_video(s->stream);
}

void rtc_rtp_receiver_activate(struct rtc_rtp_receiver *r) {
    if (r)
        rtc_rtp_recv_stream_set_active(r->stream, true);
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
    m->ssrc = rtc_rtp_send_stream_ssrc(t->sender.stream);

#ifdef MRTC_ENABLE_TWCC
    if (m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) {
        rtc_sdp_media_add_extmap(m, 5, RTC_EXT_URI_TRANSPORT_CC);
    }
#endif
}
