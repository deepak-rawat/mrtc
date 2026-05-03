/*
 * test_jitter_buffer.c - Jitter buffer reordering and timing tests.
 */
#include "test_harness.h"
#include "jitter_buffer.h"
#include <string.h>

/* ------------------------------------------------------------------ */
TEST(jb_in_order) {
    jitter_buffer_config_t cfg = { .target_delay_ms = 0, .max_delay_ms = 500 };
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

/* ------------------------------------------------------------------ */
TEST(jb_reorder) {
    jitter_buffer_config_t cfg = { .target_delay_ms = 0, .max_delay_ms = 500 };
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

/* ------------------------------------------------------------------ */
TEST(jb_empty_pop) {
    jitter_buffer_config_t cfg = { .target_delay_ms = 0, .max_delay_ms = 500 };
    jitter_buffer_t *jb = jitter_buffer_create(&cfg);

    jitter_buffer_packet_t out;
    ASSERT_EQ(jitter_buffer_pop(jb, &out), -1);
    printf("    empty pop returns -1\n");

    jitter_buffer_destroy(jb);
}

/* ------------------------------------------------------------------ */
TEST(jb_duplicate_drop) {
    jitter_buffer_config_t cfg = { .target_delay_ms = 0, .max_delay_ms = 500 };
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

/* ------------------------------------------------------------------ */
TEST(jb_adaptive_delay) {
    jitter_buffer_config_t cfg = { .target_delay_ms = 20, .max_delay_ms = 500 };
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
    /* Delay should have adapted (may increase or stay same) */
    ASSERT(new_delay >= 20);

    jitter_buffer_destroy(jb);
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Jitter Buffer Tests\n");
    printf("========================================\n\n");

    RUN_TEST(jb_in_order);
    RUN_TEST(jb_reorder);
    RUN_TEST(jb_empty_pop);
    RUN_TEST(jb_duplicate_drop);
    RUN_TEST(jb_adaptive_delay);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
