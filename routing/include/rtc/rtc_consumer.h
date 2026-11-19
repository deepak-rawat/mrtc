/*
 * RTC consumer API - outbound media sink on an SFU router.
 */
#ifndef RTC_CONSUMER_H
#define RTC_CONSUMER_H

#include "rtc/rtc_producer.h"
#include "rtc/rtc_rtp_params.h"
#include "rtc/rtc_transport.h"

typedef struct rtc_consumer rtc_consumer_t;

#define RTC_CONSUMER_HISTORY_SIZE    128
#define RTC_CONSUMER_MAX_RETRANSMITS 3

typedef struct {
    rtc_producer_t *producer;
    bool paused;
    void *app_data;
} rtc_consumer_options_t;

typedef struct {
    bool closed;
    bool paused;
    bool source_ended;
    rtc_media_kind_t kind;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t packets_dropped;
    uint64_t packets_retransmitted;
    uint64_t packets_queued;
    uint32_t estimated_bitrate_bps;
    uint64_t bitrate_feedback_ms;
    uint64_t last_packet_ms;
    uint32_t allocated_bitrate_bps; /* UINT32_MAX: independent/default policy */
} rtc_consumer_stats_t;

rtc_consumer_t *rtc_transport_consume(rtc_transport_t *transport,
                                      const rtc_consumer_options_t *opts);
/* The source SSRC is immutable. A fixed sequence offset preserves source
 * gaps, reordering and wrap, including across pause/resume. A source restart
 * with a discontinuous sequence space requires a new producer/consumer.
 * The last 128 sequence positions retain successful protected wire packets,
 * with at most three retransmission attempts each. Opt-in transport pacing
 * stages complete video frames; cached ciphertext is never re-protected. */
const char *rtc_consumer_id(const rtc_consumer_t *consumer);
int rtc_consumer_get_rtp_parameters(rtc_consumer_t *consumer, rtc_rtp_parameters_t *out);
void rtc_consumer_pause(rtc_consumer_t *consumer);
void rtc_consumer_resume(rtc_consumer_t *consumer);
void rtc_consumer_close(rtc_consumer_t *consumer);
/* Remove subscriber feedback routing before destroying the consumer. */
void rtc_consumer_destroy(rtc_consumer_t *consumer);
int rtc_consumer_get_stats(rtc_consumer_t *consumer, rtc_consumer_stats_t *out);
/* Video-only cap, including zero; UINT32_MAX restores independent feedback.
 * The caller groups legs by subscriber/room/epoch. No cross-worker call is
 * made while holding a consumer media mutex. */
int rtc_consumer_set_bitrate_allocation(rtc_consumer_t *consumer, uint32_t bitrate_bps);
#define RTC_CONSUMER_MAX_ALLOCATION_LEGS 64
/* Fairly split one subscriber's aggregate video budget. UINT32_MAX pools
 * fresh leg estimates (500 kbps per fresh/new leg without feedback).
 * Excludes paused/ended sources and sources idle for ten seconds. Audio is
 * untouched. The caller owns the array and consumer lifetimes for the call. */
int rtc_consumer_allocate_video(rtc_consumer_t *const *consumers, size_t count, uint32_t budget_bps,
                                uint64_t now_ms);
/* Call from the subscriber transport's RTCP callback (worker thread).
 * Handles compound NACK/PLI/FIR feedback for this consumer's rewritten SSRC. */
int rtc_consumer_handle_rtcp(rtc_consumer_t *consumer, const uint8_t *data, size_t len);

#endif /* RTC_CONSUMER_H */
