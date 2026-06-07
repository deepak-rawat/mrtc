/*
 * RTC producer API - inbound media source on an SFU router.
 */
#ifndef RTC_PRODUCER_H
#define RTC_PRODUCER_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_producer.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_common.h"

typedef struct rtc_producer rtc_producer_t;

#endif /* RTC_PRODUCER_H */