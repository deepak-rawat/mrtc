/*
 * Dynamic timer scheduler tests.
 */
#include "rtc_timer_sched.h"
#include "test_harness.h"

static int g_order[8];
static int g_order_count;

static void record_order(void *user) {
    int value = *(int *)user;
    g_order[g_order_count++] = value;
}

static void increment_int(void *user) {
    int *value = (int *)user;
    (*value)++;
}

TEST(timer_ordering) {
    rtc_timer_sched_t sched;
    ASSERT_EQ(rtc_timer_sched_init(&sched), RTC_OK);

    int a = 1, b = 2, c = 3;
    g_order_count = 0;
    ASSERT(rtc_timer_sched_add(&sched, 300, record_order, &c) != RTC_TIMER_HANDLE_INVALID);
    ASSERT(rtc_timer_sched_add(&sched, 100, record_order, &a) != RTC_TIMER_HANDLE_INVALID);
    ASSERT(rtc_timer_sched_add(&sched, 200, record_order, &b) != RTC_TIMER_HANDLE_INVALID);

    ASSERT_EQ(rtc_timer_sched_next_timeout_ms(&sched, 50, 1000), 50);
    rtc_timer_sched_fire_due(&sched, 250);

    ASSERT_EQ(g_order_count, 2);
    ASSERT_EQ(g_order[0], 1);
    ASSERT_EQ(g_order[1], 2);
    ASSERT_EQ(rtc_timer_sched_pending_count(&sched), 1);

    rtc_timer_sched_fire_due(&sched, 300);
    ASSERT_EQ(g_order_count, 3);
    ASSERT_EQ(g_order[2], 3);
    ASSERT_EQ(rtc_timer_sched_pending_count(&sched), 0);

    rtc_timer_sched_close(&sched);
}

TEST(timer_cancel) {
    rtc_timer_sched_t sched;
    ASSERT_EQ(rtc_timer_sched_init(&sched), RTC_OK);

    int fired_a = 0;
    int fired_b = 0;
    rtc_timer_handle_t a = rtc_timer_sched_add(&sched, 100, increment_int, &fired_a);
    rtc_timer_handle_t b = rtc_timer_sched_add(&sched, 100, increment_int, &fired_b);
    ASSERT(a != RTC_TIMER_HANDLE_INVALID);
    ASSERT(b != RTC_TIMER_HANDLE_INVALID);

    rtc_timer_sched_cancel(&sched, a);
    ASSERT_EQ(rtc_timer_sched_pending_count(&sched), 1);
    rtc_timer_sched_fire_due(&sched, 100);

    ASSERT_EQ(fired_a, 0);
    ASSERT_EQ(fired_b, 1);
    ASSERT_EQ(rtc_timer_sched_pending_count(&sched), 0);

    rtc_timer_sched_close(&sched);
}

TEST(timer_stale_handle) {
    rtc_timer_sched_t sched;
    ASSERT_EQ(rtc_timer_sched_init(&sched), RTC_OK);

    int fired_a = 0;
    int fired_b = 0;
    rtc_timer_handle_t old_handle = rtc_timer_sched_add(&sched, 100, increment_int, &fired_a);
    ASSERT(old_handle != RTC_TIMER_HANDLE_INVALID);
    rtc_timer_sched_fire_due(&sched, 100);

    rtc_timer_handle_t new_handle = rtc_timer_sched_add(&sched, 200, increment_int, &fired_b);
    ASSERT(new_handle != RTC_TIMER_HANDLE_INVALID);
    ASSERT(old_handle != new_handle);

    rtc_timer_sched_cancel(&sched, old_handle);
    rtc_timer_sched_fire_due(&sched, 200);

    ASSERT_EQ(fired_a, 1);
    ASSERT_EQ(fired_b, 1);

    rtc_timer_sched_close(&sched);
}

TEST(timer_growth) {
    rtc_timer_sched_t sched;
    ASSERT_EQ(rtc_timer_sched_init(&sched), RTC_OK);

    int fired = 0;
    for (int i = 0; i < 256; i++) {
        ASSERT(rtc_timer_sched_add(&sched, (uint64_t)i, increment_int, &fired) !=
               RTC_TIMER_HANDLE_INVALID);
    }
    ASSERT_EQ(rtc_timer_sched_pending_count(&sched), 256);
    rtc_timer_sched_fire_due(&sched, 255);
    ASSERT_EQ(fired, 256);
    ASSERT_EQ(rtc_timer_sched_pending_count(&sched), 0);

    rtc_timer_sched_close(&sched);
}

int main(void) {
    RUN_TEST(timer_ordering);
    RUN_TEST(timer_cancel);
    RUN_TEST(timer_stale_handle);
    RUN_TEST(timer_growth);
    TEST_SUMMARY();
}