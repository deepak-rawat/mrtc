/*
 * test_worker.c - RTC worker lifecycle tests.
 */
#include <rtc/rtc_worker.h>

#include "test_harness.h"

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

int main(void) {
    RUN_TEST(worker_create_destroy);
    RUN_TEST(worker_close_idempotent);
    RUN_TEST(worker_invalid_stats_args);
    TEST_SUMMARY();
}