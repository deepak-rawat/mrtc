/*
 * Dynamic timer scheduler for worker-owned timers.
 *
 * Internal utility. The scheduler is not thread-safe; callers serialize access
 * from the owning worker/event loop. Cancellation is lazy: canceled timers are
 * discarded when they reach the heap root.
 */
#ifndef RTC_TIMER_SCHED_H
#define RTC_TIMER_SCHED_H

#include "rtc_common.h"

#define RTC_TIMER_HANDLE_INVALID 0ULL

typedef uint64_t rtc_timer_handle_t;
typedef void (*rtc_timer_sched_fn)(void *user);

typedef struct {
    uint64_t deadline_ms;
    rtc_timer_sched_fn fn;
    void *user;
    uint32_t generation;
    int slot;
} rtc_timer_sched_node_t;

typedef struct {
    rtc_timer_sched_node_t *heap;
    int heap_count;
    int heap_cap;

    uint32_t *generations;
    bool *active;
    int slot_count;
    int slot_cap;
} rtc_timer_sched_t;

int rtc_timer_sched_init(rtc_timer_sched_t *sched);
void rtc_timer_sched_close(rtc_timer_sched_t *sched);

rtc_timer_handle_t rtc_timer_sched_add(rtc_timer_sched_t *sched, uint64_t deadline_ms,
                                       rtc_timer_sched_fn fn, void *user);
void rtc_timer_sched_cancel(rtc_timer_sched_t *sched, rtc_timer_handle_t handle);

/* Returns 0 when a timer is due now, a positive timeout in milliseconds when
 * a timer is pending, or default_timeout_ms when no timers are pending. */
int rtc_timer_sched_next_timeout_ms(rtc_timer_sched_t *sched, uint64_t now_ms,
                                    int default_timeout_ms);

void rtc_timer_sched_fire_due(rtc_timer_sched_t *sched, uint64_t now_ms);
bool rtc_timer_sched_pop_due(rtc_timer_sched_t *sched, uint64_t now_ms, rtc_timer_sched_fn *fn,
                             void **user);
int rtc_timer_sched_pending_count(rtc_timer_sched_t *sched);

#endif /* RTC_TIMER_SCHED_H */
