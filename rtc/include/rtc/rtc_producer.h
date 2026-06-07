/*
 * RTC producer API - inbound media source on an SFU router.
 */
#ifndef RTC_PRODUCER_H
#define RTC_PRODUCER_H

#include "rtc_rtp_params.h"
#include "rtc_transport.h"

typedef struct rtc_producer rtc_producer_t;

typedef struct {
	rtc_media_kind_t kind;
	rtc_rtp_parameters_t rtp;
	const char *label;
	void *app_data;
} rtc_producer_options_t;

typedef struct {
	bool closed;
	rtc_media_kind_t kind;
	uint64_t packets_received;
	uint64_t bytes_received;
} rtc_producer_stats_t;

rtc_producer_t *rtc_transport_produce(rtc_transport_t *transport,
									  const rtc_producer_options_t *opts);
const char *rtc_producer_id(const rtc_producer_t *producer);
int rtc_producer_get_rtp_parameters(rtc_producer_t *producer, rtc_rtp_parameters_t *out);
void rtc_producer_close(rtc_producer_t *producer);
void rtc_producer_destroy(rtc_producer_t *producer);
int rtc_producer_get_stats(rtc_producer_t *producer, rtc_producer_stats_t *out);

#endif /* RTC_PRODUCER_H */