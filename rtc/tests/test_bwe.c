/*
 * Tests for the BWE (GCC) estimator.
 */
#include "rtc/rtc_bwe.h"
#include "test_harness.h"

#include <stdio.h>

static rtc_bwe_t *make_bwe(void) {
    rtc_bwe_config_t cfg = {
        .initial_bps = 500000,
        .min_bps = 100000,
        .max_bps = 4000000,
    };
    return rtc_bwe_create(&cfg);
}

/* Feed a steady-rate stream with no induced delay: BWE should increase. */
TEST(steady_stream_increases) {
    rtc_bwe_t *b = make_bwe();
    ASSERT(b != NULL);

    uint64_t t_us = 1000000;
    /* Simulate 1500-byte packets every 5ms (≈2.4 Mbps), no transit delay. */
    uint32_t start = rtc_bwe_target_bitrate(b);
    for (int i = 0; i < 400; i++) {
        rtc_bwe_on_packet_feedback(b, t_us, t_us, 1200);
        t_us += 5000;
    }
    uint32_t end = rtc_bwe_target_bitrate(b);
    printf("    steady stream: %u -> %u bps\n", start, end);
    ASSERT(end > start);
    rtc_bwe_destroy(b);
}

/* Inject a growing delay → BWE should detect overuse and decrease. */
TEST(delay_growth_triggers_decrease) {
    rtc_bwe_t *b = make_bwe();
    ASSERT(b != NULL);

    uint64_t send = 1000000;
    uint64_t recv = 1000000;

    /* Warm up: 50 bursts with steady transit. */
    for (int i = 0; i < 50; i++) {
        rtc_bwe_on_packet_feedback(b, send, recv, 1200);
        send += 10000;
        recv += 10000;
    }
    uint32_t peak = rtc_bwe_target_bitrate(b);
    /* Push BWE up first via multiplicative increases */
    ASSERT(peak >= 500000);

    /* Now inject growing one-way delay: each burst arrives 4ms later than
     * the previous transit delta would imply. */
    int extra_us = 0;
    for (int i = 0; i < 80; i++) {
        extra_us += 4000; /* +4ms per group */
        rtc_bwe_on_packet_feedback(b, send, recv + (uint64_t)extra_us, 1200);
        send += 10000;
        recv += 10000;
    }
    uint32_t after = rtc_bwe_target_bitrate(b);
    printf("    after delay growth: peak=%u after=%u bps state=%d\n", peak, after,
           (int)rtc_bwe_state(b));
    ASSERT(rtc_bwe_state(b) == RTC_BWE_OVERUSE);
    ASSERT(after < peak);
    rtc_bwe_destroy(b);
}

/* High loss → loss-based estimator clamps. */
TEST(high_loss_clamps_bitrate) {
    rtc_bwe_t *b = make_bwe();
    ASSERT(b != NULL);

    /* Drive bitrate up first so the loss-based cap is visible. */
    uint64_t t_us = 1000000;
    for (int i = 0; i < 300; i++) {
        rtc_bwe_on_packet_feedback(b, t_us, t_us, 1200);
        t_us += 5000;
    }
    uint32_t before = rtc_bwe_target_bitrate(b);

    /* 25% loss applied repeatedly should drop loss-based estimate well
     * below the delay-based estimate. */
    for (int i = 0; i < 25; i++)
        rtc_bwe_on_loss(b, 64); /* 64/256 = 25% */
    uint32_t after = rtc_bwe_target_bitrate(b);
    printf("    high loss: %u -> %u bps\n", before, after);
    ASSERT(after < before);
    rtc_bwe_destroy(b);
}

/* Low loss → loss-based estimator increases over time, but doesn't blow past max. */
TEST(low_loss_does_not_decrease) {
    rtc_bwe_t *b = make_bwe();
    ASSERT(b != NULL);

    uint32_t before = rtc_bwe_target_bitrate(b);
    for (int i = 0; i < 10; i++)
        rtc_bwe_on_loss(b, 1); /* ~0.4% */
    uint32_t after = rtc_bwe_target_bitrate(b);
    printf("    low loss: %u -> %u bps\n", before, after);
    ASSERT(after >= before);
    rtc_bwe_destroy(b);
}

/* Callback fires on significant change. */
static int g_cb_count;
static uint32_t g_last_cb_bps;
static void on_bitrate_cb(uint32_t bps, void *user) {
    (void)user;
    g_cb_count++;
    g_last_cb_bps = bps;
}

TEST(callback_fires_on_change) {
    g_cb_count = 0;
    g_last_cb_bps = 0;
    rtc_bwe_t *b = make_bwe();
    ASSERT(b != NULL);
    rtc_bwe_on_bitrate_change(b, on_bitrate_cb, NULL);

    uint64_t t_us = 1000000;
    for (int i = 0; i < 200; i++) {
        rtc_bwe_on_packet_feedback(b, t_us, t_us, 1200);
        t_us += 5000;
    }
    printf("    callback count=%d last=%u\n", g_cb_count, g_last_cb_bps);
    ASSERT(g_cb_count > 0);
    rtc_bwe_destroy(b);
}

int main(void) {
    RUN_TEST(steady_stream_increases);
    RUN_TEST(delay_growth_triggers_decrease);
    RUN_TEST(high_loss_clamps_bitrate);
    RUN_TEST(low_loss_does_not_decrease);
    RUN_TEST(callback_fires_on_change);
    return 0;
}
