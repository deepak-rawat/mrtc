/*
 * Jitter buffer reordering and timing tests.
 */
#include "test_harness.h"
#include "jitter_buffer.h"
#include <string.h>

TEST(jb_in_order) {
    jitter_buffer_config_t cfg = {.target_delay_ms = 0, .max_delay_ms = 500};
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);
    ASSERT(jb != NULL);

    uint8_t d1[] = "pkt1", d2[] = "pkt2", d3[] = "pkt3";
    jitter_buffer_push(jb, d1, 4, 1, 1000, false);
    jitter_buffer_push(jb, d2, 4, 2, 2000, false);
    jitter_buffer_push(jb, d3, 4, 3, 3000, true);

    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 1);
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 2);
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 3);
    ASSERT(out.marker);

    printf("    in-order: 3 packets popped in sequence\n");
    jitter_buffer_destroy(jb);
}

TEST(jb_reorder) {
    jitter_buffer_config_t cfg = {.target_delay_ms = 0, .max_delay_ms = 500};
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);

    uint8_t d1[] = "a", d2[] = "b", d3[] = "c";
    /* Push out of order: 3, 1, 2 */
    jitter_buffer_push(jb, d3, 1, 3, 3000, false);
    jitter_buffer_push(jb, d1, 1, 1, 1000, false);
    jitter_buffer_push(jb, d2, 1, 2, 2000, false);

    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 1);
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 2);
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 3);

    printf("    reorder: 3,1,2 → popped as 1,2,3\n");
    jitter_buffer_destroy(jb);
}

TEST(jb_empty_pop) {
    jitter_buffer_config_t cfg = {.target_delay_ms = 0, .max_delay_ms = 500};
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);

    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop(jb, &out), -1);
    printf("    empty pop returns -1\n");

    jitter_buffer_destroy(jb);
}

TEST(jb_duplicate_drop) {
    jitter_buffer_config_t cfg = {.target_delay_ms = 0, .max_delay_ms = 500};
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);

    uint8_t d[] = "x";
    jitter_buffer_push(jb, d, 1, 1, 1000, false);
    jitter_buffer_push(jb, d, 1, 2, 2000, false);

    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 1);
    ASSERT_EQ(jitter_buffer_pop(jb, &out), 0);
    ASSERT_EQ(out.seq, 2);
    ASSERT_EQ(jitter_buffer_pop(jb, &out), -1);

    printf("    2 packets pushed, 2 popped, then empty\n");
    jitter_buffer_destroy(jb);
}

TEST(jb_adaptive_delay) {
    jitter_buffer_config_t cfg = {.target_delay_ms = 20, .max_delay_ms = 500};
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);

    int initial_delay = jitter_buffer_get_delay(jb);

    /* Push packets with high jitter (simulated by timestamps) */
    for (int i = 0; i < 20; i++) {
        uint8_t d = (uint8_t)i;
        /* Simulate high inter-arrival jitter by varying timestamps */
        uint32_t ts = (uint32_t)(i * 3000 + (i % 3) * 1000);
        jitter_buffer_push(jb, &d, 1, (uint16_t)i, ts, false);
    }

    int new_delay = jitter_buffer_get_delay(jb);
    printf("    initial delay: %dms, after jitter: %dms\n", initial_delay, new_delay);
    ASSERT(new_delay >= 20);
    jitter_buffer_destroy(jb);
}

TEST(jb_codec_clock_and_timestamp_wrap) {
    const uint32_t rates[] = {48000, 90000};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        jitter_buffer_config_t cfg = {
            .target_delay_ms = 20, .max_delay_ms = 500, .clock_rate = rates[r]};
        jitter_buffer_t *jb = jitter_buffer_create(&cfg);
        ASSERT(jb != NULL);
        uint8_t data = 1;
        uint32_t timestamp = UINT32_MAX - rates[r] / 100;
        jitter_buffer_push(jb, &data, 1, 1, timestamp, false);
        jb->est_jitter_ms = 32;
        for (unsigned i = 0; i < 32; i++) {
            jb->last_arrival_ms = rtc_time_ms() - 20;
            timestamp += rates[r] / 50;
            jitter_buffer_push(jb, &data, 1, (uint16_t)(i + 2), timestamp, false);
        }
        ASSERT(jb->est_jitter_ms <= 1);
        ASSERT_EQ(jitter_buffer_get_delay(jb), 20);
        jitter_buffer_destroy(jb);
    }
}

TEST(jb_reordered_timestamp_does_not_inflate_delay) {
    jitter_buffer_config_t cfg = {.target_delay_ms = 20, .max_delay_ms = 500, .clock_rate = 48000};
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);
    ASSERT(jb != NULL);
    uint8_t data = 1;
    jitter_buffer_push(jb, &data, 1, 2, 1920, false);
    jitter_buffer_push(jb, &data, 1, 1, 960, false);
    ASSERT(jb->est_jitter_ms <= 2);
    ASSERT_EQ(jitter_buffer_get_delay(jb), 20);
    jitter_buffer_destroy(jb);
}

TEST(jb_audio_deadlines_loss_and_dtx) {
    jitter_buffer_t *jb = jitter_buffer_create(
        &(jitter_buffer_config_t){.target_delay_ms = 40, .max_delay_ms = 300, .clock_rate = 48000});
    ASSERT(jb != NULL);
    uint8_t data = 1;
    ASSERT_EQ(jitter_buffer_push_audio(jb, &data, 1, 65535, UINT32_MAX - 959, 960, 1000), RTC_OK);
    ASSERT_EQ(jitter_buffer_push_audio(jb, &data, 1, 1, 960, 960, 1040), RTC_OK);
    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1039, &out), -1);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1040, &out), 0);
    ASSERT(out.data != NULL);
    ASSERT_EQ(out.seq, 65535);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1060, &out), 0);
    ASSERT(out.data == NULL);
    ASSERT_EQ(out.sample_count, 960);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1080, &out), 0);
    ASSERT(out.data != NULL);
    ASSERT_EQ(out.seq, 1);
    ASSERT_EQ(jitter_buffer_push_audio(jb, &data, 1, 2, 8640, 960, 1200), RTC_OK);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1100, &out), -1);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1239, &out), -1);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1240, &out), 0);
    ASSERT(out.data != NULL);
    ASSERT_EQ(out.seq, 2);
    jitter_buffer_destroy(jb);
}

TEST(jb_audio_concealment_is_bounded_and_silence_resumes) {
    jitter_buffer_t *jb = jitter_buffer_create(
        &(jitter_buffer_config_t){.target_delay_ms = 40, .max_delay_ms = 300, .clock_rate = 48000});
    ASSERT(jb != NULL);
    uint8_t data = 1;
    ASSERT_EQ(jitter_buffer_push_audio(jb, &data, 1, 10, 0, 960, 1000), RTC_OK);
    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1040, &out), 0);
    unsigned samples = 0;
    for (uint64_t now = 1060; now <= 1160; now += 20) {
        ASSERT_EQ(jitter_buffer_pop_audio(jb, now, &out), 0);
        ASSERT(out.data == NULL);
        samples += (unsigned)out.sample_count;
    }
    ASSERT_EQ(samples, 48000 * JB_AUDIO_MAX_CONCEAL_MS / 1000);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 1180, &out), -1);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 2000, &out), -1);
    ASSERT_EQ(jitter_buffer_push_audio(jb, &data, 1, 11, 48000, 1920, 2000), RTC_OK);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 2040, &out), 0);
    ASSERT(out.data != NULL);
    ASSERT_EQ(out.sample_count, 1920);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 2060, &out), -1);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 2080, &out), 0);
    ASSERT(out.data == NULL);
    ASSERT_EQ(out.sample_count, 1920);
    ASSERT_EQ(jitter_buffer_push_audio(jb, &data, 1, 11, 48000, 1920, 2080), RTC_OK);
    ASSERT_EQ(rtc_u32_map_len(&jb->by_seq), 0);
    ASSERT_EQ(jitter_buffer_pop_audio(jb, 3000, &out), -1);
    jitter_buffer_destroy(jb);
}

int main(void) {
    printf("========================================\n");
    printf("  Jitter Buffer Tests\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(jb_in_order);
    RUN_TEST(jb_reorder);
    RUN_TEST(jb_empty_pop);
    RUN_TEST(jb_duplicate_drop);
    RUN_TEST(jb_adaptive_delay);
    RUN_TEST(jb_codec_clock_and_timestamp_wrap);
    RUN_TEST(jb_reordered_timestamp_does_not_inflate_delay);
    RUN_TEST(jb_audio_deadlines_loss_and_dtx);
    RUN_TEST(jb_audio_concealment_is_bounded_and_silence_resumes);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
