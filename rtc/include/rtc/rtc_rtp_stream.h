/*
 * RTP send/receive streams.
 *
 * Composed RTP/RTCP stream API over rtc_transport_t. This layer owns RTP
 * sequencing, RTCP stats, NACK retransmission, PLI rate limiting, and SR/RR
 * emission while keeping the storage layout private.
 */
#ifndef RTC_RTP_STREAM_H
#define RTC_RTP_STREAM_H

#include "rtc_common.h"
#include "rtc_rtcp.h"
#include "rtc_rtp.h"
#include "rtc_transport.h"

/* Max MID/RID length for routing (one-byte RTP extension carries up to 16). */
#define RTC_RTP_MID_MAX 17

typedef struct rtc_rtp_send_stream rtc_rtp_send_stream_t;
typedef struct rtc_rtp_recv_stream rtc_rtp_recv_stream_t;

typedef void (*rtc_rtp_recv_stream_frame_fn)(const uint8_t *payload, size_t len, uint16_t seq,
                                             uint32_t timestamp, uint32_t ssrc, bool marker,
                                             void *user);
typedef void (*rtc_rtp_send_stream_nack_fn)(const uint16_t *lost_seqs, int count, void *user);
typedef void (*rtc_rtp_send_stream_pli_fn)(void *user);

typedef struct {
    uint8_t payload_type;
    uint32_t clock_rate;
} rtc_rtp_send_stream_config_t;

typedef struct {
    uint8_t payload_type;
    uint32_t clock_rate;
    uint32_t local_ssrc;
} rtc_rtp_recv_stream_config_t;

rtc_rtp_send_stream_t *rtc_rtp_send_stream_create(const rtc_rtp_send_stream_config_t *cfg);
void rtc_rtp_send_stream_destroy(rtc_rtp_send_stream_t *stream);

rtc_rtp_recv_stream_t *rtc_rtp_recv_stream_create(const rtc_rtp_recv_stream_config_t *cfg);
void rtc_rtp_recv_stream_destroy(rtc_rtp_recv_stream_t *stream);

void rtc_rtp_send_stream_set_active(rtc_rtp_send_stream_t *stream, bool active);
bool rtc_rtp_send_stream_is_active(const rtc_rtp_send_stream_t *stream);
uint32_t rtc_rtp_send_stream_ssrc(const rtc_rtp_send_stream_t *stream);
const rtc_rtcp_stats_t *rtc_rtp_send_stream_stats(const rtc_rtp_send_stream_t *stream);

void rtc_rtp_send_stream_attach_transport(rtc_rtp_send_stream_t *stream,
                                          rtc_transport_t *transport);
void rtc_rtp_send_stream_attach_twcc(rtc_rtp_send_stream_t *stream, uint8_t ext_id);
void rtc_rtp_send_stream_arm_video(rtc_rtp_send_stream_t *stream);

void rtc_rtp_send_stream_set_send_active(rtc_rtp_send_stream_t *stream, bool active);
bool rtc_rtp_send_stream_send_active(const rtc_rtp_send_stream_t *stream);
void rtc_rtp_send_stream_set_max_bitrate(rtc_rtp_send_stream_t *stream, uint32_t bitrate_bps);
uint32_t rtc_rtp_send_stream_max_bitrate(const rtc_rtp_send_stream_t *stream);

int rtc_rtp_send_stream_send(rtc_rtp_send_stream_t *stream, const uint8_t *payload, size_t len,
                             uint32_t samples, bool marker);
int rtc_rtp_send_stream_get_target_bitrate(const rtc_rtp_send_stream_t *stream);
bool rtc_rtp_send_stream_should_keyframe(rtc_rtp_send_stream_t *stream);

void rtc_rtp_send_stream_on_nack(rtc_rtp_send_stream_t *stream, rtc_rtp_send_stream_nack_fn fn,
                                 void *user);
void rtc_rtp_send_stream_on_pli(rtc_rtp_send_stream_t *stream, rtc_rtp_send_stream_pli_fn fn,
                                void *user);
void rtc_rtp_send_stream_handle_nack(rtc_rtp_send_stream_t *stream, const uint16_t *lost_seqs,
                                     int count);
void rtc_rtp_send_stream_handle_pli(rtc_rtp_send_stream_t *stream);
bool rtc_rtp_send_stream_on_rr(rtc_rtp_send_stream_t *stream, const rtc_rtcp_rr_block_t *rr,
                               int rtt_ms);
void rtc_rtp_send_stream_emit_sr(rtc_rtp_send_stream_t *stream, rtc_transport_t *transport);

void rtc_rtp_recv_stream_set_active(rtc_rtp_recv_stream_t *stream, bool active);
bool rtc_rtp_recv_stream_is_active(const rtc_rtp_recv_stream_t *stream);
bool rtc_rtp_recv_stream_can_receive(const rtc_rtp_recv_stream_t *stream, uint8_t payload_type);
void rtc_rtp_recv_stream_set_ssrc(rtc_rtp_recv_stream_t *stream, uint32_t ssrc);
uint32_t rtc_rtp_recv_stream_ssrc(const rtc_rtp_recv_stream_t *stream);
const rtc_rtcp_stats_t *rtc_rtp_recv_stream_stats(const rtc_rtp_recv_stream_t *stream);

/* Expected MID (from a=mid) for BUNDLE / same-payload-type routing. */
void rtc_rtp_recv_stream_set_mid(rtc_rtp_recv_stream_t *stream, const char *mid);
const char *rtc_rtp_recv_stream_mid(const rtc_rtp_recv_stream_t *stream);
bool rtc_rtp_recv_stream_mid_matches(const rtc_rtp_recv_stream_t *stream, const char *mid);

void rtc_rtp_recv_stream_on_frame(rtc_rtp_recv_stream_t *stream, rtc_rtp_recv_stream_frame_fn fn,
                                  void *user);
void rtc_rtp_recv_stream_on_packet(rtc_rtp_recv_stream_t *stream, const rtc_rtp_packet_t *pkt);
void rtc_rtp_recv_stream_on_sr(rtc_rtp_recv_stream_t *stream, const rtc_rtcp_sr_t *sr);
void rtc_rtp_recv_stream_emit_rr(rtc_rtp_recv_stream_t *stream, rtc_transport_t *transport);

#endif /* RTC_RTP_STREAM_H */
