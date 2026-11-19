/*
 * RTP send/receive streams.
 *
 * Composed RTP/RTCP stream API over rtc_transport_t. This layer owns RTP
 * sequencing, RTCP stats, NACK retransmission, PLI rate limiting, and SR/RR
 * emission while keeping the storage layout private.
 * Sending and explicit keyframe requests marshal to the transport worker so
 * RTP sequencing/cache publication cannot race inbound NACK/PLI processing.
 * Configure other stream state before starting media, or on that worker.
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
typedef int (*rtc_rtp_recv_stream_feedback_fn)(const uint8_t *data, size_t len, void *user);
typedef void (*rtc_rtp_recv_stream_source_fn)(uint32_t previous_ssrc, uint32_t current_ssrc,
                                              void *user);

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
/* Initial negotiation only; cannot change payload type after media starts. */
int rtc_rtp_send_stream_set_payload_type(rtc_rtp_send_stream_t *stream, uint8_t payload_type);
int rtc_rtp_recv_stream_set_payload_type(rtc_rtp_recv_stream_t *stream, uint8_t payload_type);

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
void rtc_rtp_send_stream_set_remote_feedback(rtc_rtp_send_stream_t *stream, uint8_t feedback);
void rtc_rtp_send_stream_on_remb(rtc_rtp_send_stream_t *stream, uint32_t bitrate_bps);
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
uint64_t rtc_rtp_recv_stream_source_generation(const rtc_rtp_recv_stream_t *stream);
const rtc_rtcp_stats_t *rtc_rtp_recv_stream_stats(const rtc_rtp_recv_stream_t *stream);

/* Expected MID (from a=mid) for BUNDLE / same-payload-type routing. */
void rtc_rtp_recv_stream_set_mid(rtc_rtp_recv_stream_t *stream, const char *mid);
const char *rtc_rtp_recv_stream_mid(const rtc_rtp_recv_stream_t *stream);
bool rtc_rtp_recv_stream_mid_matches(const rtc_rtp_recv_stream_t *stream, const char *mid);

void rtc_rtp_recv_stream_on_frame(rtc_rtp_recv_stream_t *stream, rtc_rtp_recv_stream_frame_fn fn,
                                  void *user);
/* Called on source replacement/retirement; current_ssrc == 0 means ended or
 * expired. Does not change the negotiated receiver's enabled state. */
void rtc_rtp_recv_stream_on_source_change(rtc_rtp_recv_stream_t *stream,
                                          rtc_rtp_recv_stream_source_fn fn, void *user);
void rtc_rtp_recv_stream_on_rtcp_activity(rtc_rtp_recv_stream_t *stream, uint64_t now_ms);
void rtc_rtp_recv_stream_on_packet(rtc_rtp_recv_stream_t *stream, const rtc_rtp_packet_t *pkt);
void rtc_rtp_recv_stream_on_sr(rtc_rtp_recv_stream_t *stream, const rtc_rtcp_sr_t *sr);
void rtc_rtp_recv_stream_emit_rr(rtc_rtp_recv_stream_t *stream, rtc_transport_t *transport);
/* Session binds the protected RTCP path; callback override is useful for
 * alternate transports. Invoke tick on the same worker as on_packet. */
void rtc_rtp_recv_stream_attach_transport(rtc_rtp_recv_stream_t *stream,
                                          rtc_transport_t *transport);
void rtc_rtp_recv_stream_on_feedback(rtc_rtp_recv_stream_t *stream,
                                     rtc_rtp_recv_stream_feedback_fn fn, void *user);
void rtc_rtp_recv_stream_set_feedback(rtc_rtp_recv_stream_t *stream, uint8_t feedback);
void rtc_rtp_recv_stream_tick(rtc_rtp_recv_stream_t *stream, uint64_t now_ms);
int rtc_rtp_recv_stream_request_keyframe(rtc_rtp_recv_stream_t *stream);

#endif /* RTC_RTP_STREAM_H */
