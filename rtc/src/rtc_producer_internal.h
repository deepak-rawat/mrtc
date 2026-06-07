/*
 * rtc_producer_internal.h - Internal producer hooks used by logical transports.
 */
#ifndef RTC_PRODUCER_INTERNAL_H
#define RTC_PRODUCER_INTERNAL_H

#include "rtc/rtc_producer.h"

#include "rtc_rtp.h"

uint32_t rtc_producer_ssrc(const rtc_producer_t *producer);
void rtc_producer_on_rtp(rtc_producer_t *producer, const rtc_rtp_packet_t *pkt);

#endif /* RTC_PRODUCER_INTERNAL_H */