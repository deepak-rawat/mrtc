/*
 * RTC router API - SFU media routing graph.
 */
#ifndef RTC_ROUTER_H
#define RTC_ROUTER_H

#include "rtc_common.h"
#include "rtc_transport.h"
#include "rtc_worker.h"

typedef struct rtc_router rtc_router_t;

typedef struct {
	void *app_data;
} rtc_router_config_t;

rtc_router_t *rtc_router_create(rtc_worker_t *worker, const rtc_router_config_t *cfg);
void rtc_router_close(rtc_router_t *router);
void rtc_router_destroy(rtc_router_t *router);

rtc_transport_t *rtc_router_create_transport(rtc_router_t *router,
											 const rtc_transport_config_t *cfg);

#endif /* RTC_ROUTER_H */