/*
 * AIMD rate controller tests.
 */
#include "test_harness.h"
#include "rtc_rate_control.h"
#include <rtc/rtc_common.h>

/* ------------------------------------------------------------------ */
TEST(rc_no_loss_increase) {
    rtc_rate_control_config_t cfg = {
        .target_bitrate_kbps = 500, .min_bitrate_kbps = 100, .max_bitrate_kbps = 2500};
    rtc_rate_controller_t *rc = rtc_rate_control_create(&cfg);
    ASSERT(rc != NULL);

    int initial = rtc_rate_control_get_bitrate(rc);
    ASSERT_EQ(initial, 500);

    /* 10 rounds of zero loss */
    for (int i = 0; i < 10; i++)
        rtc_rate_control_on_rtcp_rr(rc, 0, 50, 0);

    int after = rtc_rate_control_get_bitrate(rc);
    ASSERT(after > initial);
    printf("    no loss: %d → %d kbps\n", initial, after);

    rtc_rate_control_destroy(rc);
}

/* ------------------------------------------------------------------ */
TEST(rc_high_loss_decrease) {
    rtc_rate_control_config_t cfg = {
        .target_bitrate_kbps = 1000, .min_bitrate_kbps = 100, .max_bitrate_kbps = 2500};
    rtc_rate_controller_t *rc = rtc_rate_control_create(&cfg);

    int initial = rtc_rate_control_get_bitrate(rc);

    /* 10% loss = fraction_lost ~26/256 */
    for (int i = 0; i < 5; i++)
        rtc_rate_control_on_rtcp_rr(rc, 26, 50, 0);

    int after = rtc_rate_control_get_bitrate(rc);
    ASSERT(after < initial);
    printf("    10%% loss: %d → %d kbps\n", initial, after);

    rtc_rate_control_destroy(rc);
}

/* ------------------------------------------------------------------ */
TEST(rc_clamp_min_max) {
    rtc_rate_control_config_t cfg = {
        .target_bitrate_kbps = 200, .min_bitrate_kbps = 100, .max_bitrate_kbps = 500};
    rtc_rate_controller_t *rc = rtc_rate_control_create(&cfg);

    /* Drive down with heavy loss */
    for (int i = 0; i < 50; i++)
        rtc_rate_control_on_rtcp_rr(rc, 128, 100, 0); /* 50% loss */

    int low = rtc_rate_control_get_bitrate(rc);
    ASSERT_EQ(low, 100); /* Clamped to min */

    /* Drive up with no loss */
    for (int i = 0; i < 200; i++)
        rtc_rate_control_on_rtcp_rr(rc, 0, 10, 0);

    int high = rtc_rate_control_get_bitrate(rc);
    ASSERT_EQ(high, 500); /* Clamped to max */

    printf("    clamp: min=%d, max=%d\n", low, high);
    rtc_rate_control_destroy(rc);
}

/* ------------------------------------------------------------------ */
TEST(rc_keyframe_on_spike) {
    rtc_rate_control_config_t cfg = {
        .target_bitrate_kbps = 500, .min_bitrate_kbps = 100, .max_bitrate_kbps = 2500};
    rtc_rate_controller_t *rc = rtc_rate_control_create(&cfg);

    ASSERT(!rtc_rate_control_should_keyframe(rc));

    /* Loss spike > 10% → fraction_lost > 26 */
    rtc_rate_control_on_rtcp_rr(rc, 51, 50, 0); /* ~20% loss */
    ASSERT(rtc_rate_control_should_keyframe(rc));

    /* Flag should be cleared after reading */
    ASSERT(!rtc_rate_control_should_keyframe(rc));

    printf("    keyframe requested on loss spike, cleared after read\n");
    rtc_rate_control_destroy(rc);
}

/* ------------------------------------------------------------------ */
TEST(rc_high_rtt_penalty) {
    rtc_rate_control_config_t cfg = {
        .target_bitrate_kbps = 1000, .min_bitrate_kbps = 100, .max_bitrate_kbps = 2500};
    rtc_rate_controller_t *rc = rtc_rate_control_create(&cfg);

    int initial = rtc_rate_control_get_bitrate(rc);

    /* Low loss but high RTT */
    rtc_rate_control_on_rtcp_rr(rc, 0, 500, 0);

    int after = rtc_rate_control_get_bitrate(rc);
    /* Should increase from no loss but decrease from high RTT — net effect depends */
    printf("    high RTT (500ms): %d → %d kbps\n", initial, after);

    rtc_rate_control_destroy(rc);
}

/* ------------------------------------------------------------------ */
TEST(rc_per_sender_independent) {
    rtc_rate_control_config_t cfg = {
        .target_bitrate_kbps = 1000, .min_bitrate_kbps = 100, .max_bitrate_kbps = 5000};
    rtc_rate_controller_t *rc_a = rtc_rate_control_create(&cfg);
    rtc_rate_controller_t *rc_b = rtc_rate_control_create(&cfg);

    /* Feed high loss to A, no loss to B */
    for (int i = 0; i < 5; i++) {
        rtc_rate_control_on_rtcp_rr(rc_a, 51, 50, 0); /* ~20% loss */
        rtc_rate_control_on_rtcp_rr(rc_b, 0, 50, 0);  /* 0% loss */
    }

    int br_a = rtc_rate_control_get_bitrate(rc_a);
    int br_b = rtc_rate_control_get_bitrate(rc_b);

    ASSERT(br_a < 1000); /* A should have decreased */
    ASSERT(br_b > 1000); /* B should have increased */
    ASSERT(br_a < br_b); /* A < B */

    printf("    per-sender: A=%d kbps, B=%d kbps (independent)\n", br_a, br_b);
    rtc_rate_control_destroy(rc_a);
    rtc_rate_control_destroy(rc_b);
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Rate Control Tests\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(rc_no_loss_increase);
    RUN_TEST(rc_high_loss_decrease);
    RUN_TEST(rc_clamp_min_max);
    RUN_TEST(rc_keyframe_on_spike);
    RUN_TEST(rc_high_rtt_penalty);
    RUN_TEST(rc_per_sender_independent);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
