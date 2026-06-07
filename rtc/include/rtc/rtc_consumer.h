/*
 * RTC consumer API - outbound media sink on an SFU router.
 */
#ifndef RTC_CONSUMER_H
#define RTC_CONSUMER_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_consumer.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_producer.h"
#include "rtc_rtp_params.h"
#include "rtc_transport.h"

typedef struct rtc_consumer rtc_consumer_t;

typedef struct {
	rtc_producer_t *producer;
	bool paused;
	void *app_data;
} rtc_consumer_options_t;

typedef struct {
	bool closed;
	bool paused;
	rtc_media_kind_t kind;
	uint64_t packets_sent;
	uint64_t bytes_sent;
} rtc_consumer_stats_t;

rtc_consumer_t *rtc_transport_consume(rtc_transport_t *transport,
									  const rtc_consumer_options_t *opts);
const char *rtc_consumer_id(const rtc_consumer_t *consumer);
int rtc_consumer_get_rtp_parameters(rtc_consumer_t *consumer, rtc_rtp_parameters_t *out);
void rtc_consumer_pause(rtc_consumer_t *consumer);
void rtc_consumer_resume(rtc_consumer_t *consumer);
void rtc_consumer_close(rtc_consumer_t *consumer);
void rtc_consumer_destroy(rtc_consumer_t *consumer);
int rtc_consumer_get_stats(rtc_consumer_t *consumer, rtc_consumer_stats_t *out);

#endif /* RTC_CONSUMER_H */