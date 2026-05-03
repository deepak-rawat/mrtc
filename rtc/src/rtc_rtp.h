/*
 * RTP / RTCP (RFC 3550) - Minimal RTP packetization.
 *
 * Supports building and parsing RTP headers for a single media stream.
 */
#ifndef RTC_RTP_H
#define RTC_RTP_H

#include "rtc_types.h"

#define RTP_VERSION     2
#define RTP_HEADER_SIZE 12
#define RTP_MAX_PACKET  1400 /* safe MTU for RTP payload */
#define RTP_SRTP_PAD    10   /* room for SRTP auth tag */

/* RTP header (fixed 12 bytes) */
typedef struct {
    uint8_t version; /* always 2 */
    bool padding;
    bool extension;
    uint8_t csrc_count;
    bool marker;
    uint8_t payload_type;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtc_rtp_header_t;

/* RTP packet */
typedef struct {
    rtc_rtp_header_t header;
    const uint8_t *payload;
    size_t payload_len;
    /* Serialized form */
    uint8_t buf[RTP_MAX_PACKET + RTP_HEADER_SIZE + RTP_SRTP_PAD];
    size_t buf_len;
} rtc_rtp_packet_t;

/* Serialize RTP header + payload into packet buffer */
int rtc_rtp_build(rtc_rtp_packet_t *pkt, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                  bool marker, const uint8_t *payload, size_t payload_len);

/* Parse raw bytes into RTP header + payload pointer */
int rtc_rtp_parse(rtc_rtp_packet_t *pkt, const uint8_t *data, size_t len);

/* ---------- Simple RTP session (sequence & timestamp tracking) ---------- */
typedef struct {
    uint32_t ssrc;
    uint16_t seq;
    uint32_t timestamp;
    uint8_t payload_type;
    uint32_t clock_rate;
} rtc_rtp_session_t;

/* Initialize RTP session with random SSRC */
int rtc_rtp_session_init(rtc_rtp_session_t *sess, uint8_t pt, uint32_t clock_rate);

/* Build next RTP packet. samples = number of audio samples in this frame */
int rtc_rtp_session_send(rtc_rtp_session_t *sess, rtc_rtp_packet_t *pkt, const uint8_t *payload,
                         size_t payload_len, uint32_t samples, bool marker);

#endif /* RTC_RTP_H */
