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
    uint32_t estimated_bitrate_bps;
} rtc_consumer_stats_t;

rtc_consumer_t *rtc_transport_consume(rtc_transport_t *transport,
                                      const rtc_consumer_options_t *opts);
/* The source SSRC is immutable. A fixed sequence offset preserves source
 * gaps, reordering and wrap, including across pause/resume. A source restart
 * with a discontinuous sequence space requires a new producer/consumer.
 * The last 128 sequence positions retain successful protected wire packets,
 * with at most three retransmission attempts each; nothing is queued. */
const char *rtc_consumer_id(const rtc_consumer_t *consumer);
int rtc_consumer_get_rtp_parameters(rtc_consumer_t *consumer, rtc_rtp_parameters_t *out);
void rtc_consumer_pause(rtc_consumer_t *consumer);
void rtc_consumer_resume(rtc_consumer_t *consumer);
void rtc_consumer_close(rtc_consumer_t *consumer);
/* Remove subscriber feedback routing before destroying the consumer. */
void rtc_consumer_destroy(rtc_consumer_t *consumer);
int rtc_consumer_get_stats(rtc_consumer_t *consumer, rtc_consumer_stats_t *out);
/* Call from the subscriber transport's RTCP callback (worker thread).
 * Handles compound NACK/PLI/FIR feedback for this consumer's rewritten SSRC. */
int rtc_consumer_handle_rtcp(rtc_consumer_t *consumer, const uint8_t *data, size_t len);

#endif /* RTC_CONSUMER_H */
