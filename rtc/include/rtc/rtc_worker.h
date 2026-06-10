/*
 * RTC worker API - event-loop and scheduling shard for client and SFU runtime.
 */
#ifndef RTC_WORKER_H
#define RTC_WORKER_H

#include "rtc_common.h"

typedef struct rtc_worker rtc_worker_t;
typedef uint64_t rtc_worker_timer_t;
typedef void (*rtc_worker_timer_fn)(void *user);

#define RTC_WORKER_TIMER_INVALID 0ULL

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
rtc_worker_timer_t rtc_worker_add_timer(rtc_worker_t *worker, uint64_t deadline_ms,
                                        rtc_worker_timer_fn fn, void *user);
void rtc_worker_cancel_timer(rtc_worker_t *worker, rtc_worker_timer_t timer);

#endif /* RTC_WORKER_H */
