/*
 * rtc_worker_internal.h - Internal worker runtime hooks.
 */
#ifndef RTC_WORKER_INTERNAL_H
#define RTC_WORKER_INTERNAL_H

#include "rtc/rtc_worker.h"

#include "rtc_timer_sched.h"

typedef rtc_timer_handle_t rtc_worker_timer_t;
typedef void (*rtc_worker_timer_fn)(void *user);

#define RTC_WORKER_TIMER_INVALID RTC_TIMER_HANDLE_INVALID

rtc_worker_timer_t rtc_worker_add_timer(rtc_worker_t *worker, uint64_t deadline_ms,
                                        rtc_worker_timer_fn fn, void *user);
void rtc_worker_cancel_timer(rtc_worker_t *worker, rtc_worker_timer_t timer);

#endif /* RTC_WORKER_INTERNAL_H */
