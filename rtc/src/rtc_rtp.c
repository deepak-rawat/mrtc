/*
 * rtc_rtp.c - RTP (RFC 3550) minimal packetization.
 */
#include "rtc_rtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rtc_rtp_build(rtc_rtp_packet_t *pkt, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                  bool marker, const uint8_t *payload, size_t payload_len) {
    if (payload_len > RTP_MAX_PACKET)
        return RTC_ERR_INVALID;

    pkt->header.version = RTP_VERSION;
    pkt->header.padding = false;
    pkt->header.extension = false;
    pkt->header.csrc_count = 0;
    pkt->header.marker = marker;
    pkt->header.payload_type = pt;
    pkt->header.seq = seq;
    pkt->header.timestamp = ts;
    pkt->header.ssrc = ssrc;
    pkt->payload = payload;
    pkt->payload_len = payload_len;

    /* Serialize header */
    uint8_t *b = pkt->buf;

    b[0] = (RTP_VERSION << 6);
    if (marker)
        b[0] |= 0x00; /* no padding, no extension, cc=0 */
    b[1] = pt & 0x7F;
    if (marker)
        b[1] |= 0x80;

    b[2] = (seq >> 8) & 0xFF;
    b[3] = seq & 0xFF;

    b[4] = (ts >> 24) & 0xFF;
    b[5] = (ts >> 16) & 0xFF;
    b[6] = (ts >> 8) & 0xFF;
    b[7] = ts & 0xFF;

    b[8] = (ssrc >> 24) & 0xFF;
    b[9] = (ssrc >> 16) & 0xFF;
    b[10] = (ssrc >> 8) & 0xFF;
    b[11] = ssrc & 0xFF;

    /* Copy payload */
    if (payload_len > 0)
        memcpy(b + RTP_HEADER_SIZE, payload, payload_len);

    pkt->buf_len = RTP_HEADER_SIZE + payload_len;
    return RTC_OK;
}

int rtc_rtp_parse(rtc_rtp_packet_t *pkt, const uint8_t *data, size_t len) {
    if (len < RTP_HEADER_SIZE)
        return RTC_ERR_INVALID;

    const uint8_t *b = data;

    pkt->header.version = (b[0] >> 6) & 0x03;
    if (pkt->header.version != RTP_VERSION)
        return RTC_ERR_INVALID;

    pkt->header.padding = (b[0] >> 5) & 0x01;
    pkt->header.extension = (b[0] >> 4) & 0x01;
    pkt->header.csrc_count = b[0] & 0x0F;
    pkt->header.marker = (b[1] >> 7) & 0x01;
    pkt->header.payload_type = b[1] & 0x7F;

    pkt->header.seq = ((uint16_t)b[2] << 8) | b[3];
    pkt->header.timestamp =
        ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    pkt->header.ssrc =
        ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | ((uint32_t)b[10] << 8) | b[11];

    /* Compute header length including CSRC list and extension */
    size_t hdr_len = RTP_HEADER_SIZE + (size_t)pkt->header.csrc_count * 4;
    if (pkt->header.extension) {
        if (hdr_len + 4 > len)
            return RTC_ERR_INVALID;
        uint16_t ext_len = ((uint16_t)data[hdr_len + 2] << 8) | data[hdr_len + 3];
        hdr_len += 4 + (size_t)ext_len * 4;
    }
    if (hdr_len > len)
        return RTC_ERR_INVALID;

    pkt->payload = data + hdr_len;
    pkt->payload_len = len - hdr_len;

    /* Copy into internal buffer */
    if (len <= sizeof(pkt->buf)) {
        memcpy(pkt->buf, data, len);
        pkt->buf_len = len;
    }

    return RTC_OK;
}

int rtc_rtp_session_init(rtc_rtp_session_t *sess, uint8_t pt, uint32_t clock_rate) {
    memset(sess, 0, sizeof(*sess));
    sess->payload_type = pt;
    sess->clock_rate = clock_rate;

    /* Random SSRC */
    rtc_random_bytes((uint8_t *)&sess->ssrc, sizeof(sess->ssrc));

    /* Random initial sequence number */
    rtc_random_bytes((uint8_t *)&sess->seq, sizeof(sess->seq));

    RTC_LOG_INFO("RTP session: ssrc=0x%08X, pt=%u, rate=%u", sess->ssrc, pt, clock_rate);
    return RTC_OK;
}

int rtc_rtp_session_send(rtc_rtp_session_t *sess, rtc_rtp_packet_t *pkt, const uint8_t *payload,
                         size_t payload_len, uint32_t samples, bool marker) {
    int rc = rtc_rtp_build(pkt, sess->payload_type, sess->seq, sess->timestamp, sess->ssrc, marker,
                           payload, payload_len);
    if (rc != RTC_OK)
        return rc;

    sess->seq++;
    sess->timestamp += samples;
    return RTC_OK;
}
