/*
 * test_worker.c - RTC worker lifecycle tests.
 */
#include <rtc/rtc_worker.h>

#include "rtc_worker_internal.h"
#include "test_harness.h"

#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

static void worker_timer_cb(void *user) {
    _Atomic int *value = (_Atomic int *)user;
    atomic_fetch_add_explicit(value, 1, memory_order_relaxed);
}

static bool wait_for_atomic(_Atomic int *value, int target, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        if (atomic_load_explicit(value, memory_order_relaxed) >= target)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

TEST(worker_create_destroy) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);

    rtc_worker_stats_t stats;
    ASSERT_EQ(rtc_worker_get_stats(worker, &stats), RTC_OK);
    ASSERT(!stats.closed);
    ASSERT_EQ(stats.timers_pending, 0);

    rtc_worker_destroy(worker);
}

TEST(worker_close_idempotent) {
    rtc_worker_t *worker = rtc_worker_create(&(rtc_worker_config_t){
        .name = "test-worker",
        .max_packet_batch = 128,
        .timer_resolution_ms = 5,
    });
    ASSERT(worker != NULL);

    rtc_worker_close(worker);
    rtc_worker_close(worker);

    rtc_worker_stats_t stats;
    ASSERT_EQ(rtc_worker_get_stats(worker, &stats), RTC_OK);
    ASSERT(stats.closed);

    rtc_worker_destroy(worker);
}

TEST(worker_invalid_stats_args) {
    rtc_worker_stats_t stats;
    ASSERT_EQ(rtc_worker_get_stats(NULL, &stats), RTC_ERR_INVALID);

    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    ASSERT_EQ(rtc_worker_get_stats(worker, NULL), RTC_ERR_INVALID);
    rtc_worker_destroy(worker);
}

TEST(worker_timer_fires) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);

    _Atomic int fired = 0;
    rtc_worker_timer_t timer =
        rtc_worker_add_timer(worker, rtc_time_ms() + 20, worker_timer_cb, &fired);
    ASSERT(timer != RTC_WORKER_TIMER_INVALID);
    ASSERT(wait_for_atomic(&fired, 1, 1000));

    rtc_worker_stats_t stats;
    ASSERT_EQ(rtc_worker_get_stats(worker, &stats), RTC_OK);
    ASSERT_EQ(stats.timers_pending, 0);

    rtc_worker_destroy(worker);
}

TEST(worker_timer_cancel) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);

    _Atomic int fired = 0;
    rtc_worker_timer_t timer =
        rtc_worker_add_timer(worker, rtc_time_ms() + 100, worker_timer_cb, &fired);
    ASSERT(timer != RTC_WORKER_TIMER_INVALID);
    rtc_worker_cancel_timer(worker, timer);
    SLEEP_MS(180);
    ASSERT_EQ(atomic_load_explicit(&fired, memory_order_relaxed), 0);

    rtc_worker_stats_t stats;
    ASSERT_EQ(rtc_worker_get_stats(worker, &stats), RTC_OK);
    ASSERT_EQ(stats.timers_pending, 0);

    rtc_worker_destroy(worker);
}

TEST(worker_rejects_timer_after_close) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_worker_close(worker);

    _Atomic int fired = 0;
    rtc_worker_timer_t timer =
        rtc_worker_add_timer(worker, rtc_time_ms() + 10, worker_timer_cb, &fired);
    ASSERT_EQ(timer, RTC_WORKER_TIMER_INVALID);

    rtc_worker_destroy(worker);
}

int main(void) {
    RUN_TEST(worker_create_destroy);
    RUN_TEST(worker_close_idempotent);
    RUN_TEST(worker_invalid_stats_args);
    RUN_TEST(worker_timer_fires);
    RUN_TEST(worker_timer_cancel);
    RUN_TEST(worker_rejects_timer_after_close);
    TEST_SUMMARY();
}