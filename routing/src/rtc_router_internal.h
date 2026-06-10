/*
 * Internal router helpers.
 */
#ifndef RTC_ROUTER_INTERNAL_H
#define RTC_ROUTER_INTERNAL_H

#include "rtc/rtc_router.h"

typedef struct rtc_producer rtc_producer_t;

rtc_worker_t *rtc_router_worker(rtc_router_t *router);
int rtc_router_register_producer(rtc_transport_t *transport, rtc_producer_t *producer,
                                 uint32_t ssrc);
void rtc_router_unregister_producer(rtc_transport_t *transport, uint32_t ssrc);

#endif /* RTC_ROUTER_INTERNAL_H */
