/*
 * RTC worker API - event-loop and scheduling shard for client and SFU runtime.
 */
#ifndef RTC_WORKER_H
#define RTC_WORKER_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_worker.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_common.h"

typedef struct rtc_worker rtc_worker_t;

typedef struct {
    const char *name;
    int max_packet_batch;
    int timer_resolution_ms;
} rtc_worker_config_t;

#endif /* RTC_WORKER_H */