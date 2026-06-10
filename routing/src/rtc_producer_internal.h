/*
 * Internal producer hooks used by logical transports.
 */
#ifndef RTC_PRODUCER_INTERNAL_H
#define RTC_PRODUCER_INTERNAL_H

#include "rtc/rtc_producer.h"

#include "rtc/rtc_rtp.h"

typedef struct rtc_consumer rtc_consumer_t;

uint32_t rtc_producer_ssrc(const rtc_producer_t *producer);
int rtc_producer_add_consumer(rtc_producer_t *producer, rtc_consumer_t *consumer);
void rtc_producer_remove_consumer(rtc_producer_t *producer, rtc_consumer_t *consumer);
void rtc_producer_on_rtp(rtc_producer_t *producer, const rtc_rtp_packet_t *pkt);

#endif /* RTC_PRODUCER_INTERNAL_H */
