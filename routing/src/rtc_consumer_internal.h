/*
 * Internal consumer hooks used by producers.
 */
#ifndef RTC_CONSUMER_INTERNAL_H
#define RTC_CONSUMER_INTERNAL_H

#include "rtc/rtc_consumer.h"

#include "rtc/rtc_rtp.h"

void rtc_consumer_on_producer_rtp(rtc_consumer_t *consumer, const rtc_rtp_packet_t *pkt);
uint32_t rtc_consumer_bitrate_limit(rtc_consumer_t *consumer, uint64_t now_ms);
void rtc_consumer_source_ended(rtc_consumer_t *consumer);

#endif /* RTC_CONSUMER_INTERNAL_H */
