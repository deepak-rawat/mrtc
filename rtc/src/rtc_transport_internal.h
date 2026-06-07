/*
 * rtc_transport_internal.h - Internal logical transport constructors.
 */
#ifndef RTC_TRANSPORT_INTERNAL_H
#define RTC_TRANSPORT_INTERNAL_H

#include "rtc/rtc_router.h"

typedef struct rtc_producer rtc_producer_t;

rtc_transport_t *rtc_transport_create_internal(rtc_router_t *router,
                                               const rtc_transport_config_t *cfg);
int rtc_transport_register_producer(rtc_transport_t *transport, rtc_producer_t *producer,
                                    uint32_t ssrc);
void rtc_transport_unregister_producer(rtc_transport_t *transport, uint32_t ssrc);

#endif /* RTC_TRANSPORT_INTERNAL_H */