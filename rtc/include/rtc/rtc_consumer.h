/*
 * RTC consumer API - outbound media sink on an SFU router.
 */
#ifndef RTC_CONSUMER_H
#define RTC_CONSUMER_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_consumer.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_common.h"

typedef struct rtc_consumer rtc_consumer_t;

#endif /* RTC_CONSUMER_H */