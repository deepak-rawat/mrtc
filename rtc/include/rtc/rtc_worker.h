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

typedef struct {
    bool closed;
    int timers_pending;
} rtc_worker_stats_t;

rtc_worker_t *rtc_worker_create(const rtc_worker_config_t *cfg);
void rtc_worker_close(rtc_worker_t *worker);
void rtc_worker_destroy(rtc_worker_t *worker);
int rtc_worker_get_stats(rtc_worker_t *worker, rtc_worker_stats_t *out);

#endif /* RTC_WORKER_H */