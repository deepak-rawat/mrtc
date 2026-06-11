/*
 * NACK retransmit buffer tests.
 */
#include "rtc/rtc_nack_buf.h"
#include "test_harness.h"

#include <string.h>

TEST(create_destroy) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(NACK_BUF_DEFAULT_SIZE);
    ASSERT(buf != NULL);
    ASSERT_EQ(buf->capacity, 512);
    ASSERT_EQ(buf->mask, 511);
    rtc_nack_buf_destroy(buf);

    /* Destroy NULL is safe */
    rtc_nack_buf_destroy(NULL);
    printf("    create/destroy OK\n");
}

TEST(store_and_get) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(64);
    ASSERT(buf != NULL);

    uint8_t pkt[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    rtc_nack_buf_store(buf, pkt, sizeof(pkt), 42, false, 0);

    const uint8_t *out;
    size_t out_len;
    ASSERT(rtc_nack_buf_get(buf, 42, &out, &out_len));
    ASSERT_EQ((int)out_len, (int)sizeof(pkt));
    ASSERT_MEM_EQ(out, pkt, sizeof(pkt));

    printf("    store and get OK\n");
    rtc_nack_buf_destroy(buf);
}

TEST(store_multiple) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(64);
    ASSERT(buf != NULL);

    for (uint16_t seq = 100; seq < 110; seq++) {
        uint8_t pkt[12];
        memset(pkt, (uint8_t)seq, sizeof(pkt));
        rtc_nack_buf_store(buf, pkt, sizeof(pkt), seq, false, 0);
    }

    for (uint16_t seq = 100; seq < 110; seq++) {
        const uint8_t *out;
        size_t out_len;
        ASSERT(rtc_nack_buf_get(buf, seq, &out, &out_len));
        ASSERT_EQ((int)out_len, 12);
        /* First byte should match (uint8_t)seq */
        ASSERT_EQ(out[0], (uint8_t)seq);
    }

    printf("    store multiple OK\n");
    rtc_nack_buf_destroy(buf);
}

TEST(get_missing) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(64);
    ASSERT(buf != NULL);

    const uint8_t *out;
    size_t out_len;
    ASSERT(!rtc_nack_buf_get(buf, 999, &out, &out_len));

    printf("    get missing returns false\n");
    rtc_nack_buf_destroy(buf);
}

TEST(wraparound) {
    int cap = 64; /* power of 2 */
    rtc_nack_buf_t *buf = rtc_nack_buf_create(cap);
    ASSERT(buf != NULL);

    /* Store cap+10 packets (overwriting oldest 10) */
    for (int i = 0; i < cap + 10; i++) {
        uint8_t pkt[8];
        uint16_t seq = (uint16_t)i;
        memset(pkt, (uint8_t)(i & 0xFF), sizeof(pkt));
        rtc_nack_buf_store(buf, pkt, sizeof(pkt), seq, false, 0);
    }

    /* Newest packets (10..cap+9) should be accessible */
    for (int i = 10; i < cap + 10; i++) {
        const uint8_t *out;
        size_t out_len;
        ASSERT(rtc_nack_buf_get(buf, (uint16_t)i, &out, &out_len));
        ASSERT_EQ(out[0], (uint8_t)(i & 0xFF));
    }

    /* Oldest overwritten packets (0..9) should NOT match
     * (slot exists but seq doesn't match) */
    for (int i = 0; i < 10; i++) {
        const uint8_t *out;
        size_t out_len;
        bool found = rtc_nack_buf_get(buf, (uint16_t)i, &out, &out_len);
        /* The slot at index i was overwritten by seq (i + cap), so seq won't match */
        ASSERT(!found);
    }

    printf("    wraparound OK\n");
    rtc_nack_buf_destroy(buf);
}

TEST(wraparound_seq_wrap) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(64);
    ASSERT(buf != NULL);

    /* Store across uint16 boundary: 65530 .. 65535, 0 .. 4 */
    for (int i = 0; i < 11; i++) {
        uint16_t seq = (uint16_t)(65530 + i); /* wraps at 65536 */
        uint8_t pkt[4] = {(uint8_t)(seq >> 8), (uint8_t)(seq & 0xFF), 0, 0};
        rtc_nack_buf_store(buf, pkt, sizeof(pkt), seq, false, 0);
    }

    /* Verify all retrievable */
    for (int i = 0; i < 11; i++) {
        uint16_t seq = (uint16_t)(65530 + i);
        const uint8_t *out;
        size_t out_len;
        ASSERT(rtc_nack_buf_get(buf, seq, &out, &out_len));
    }

    printf("    seq wrap OK\n");
    rtc_nack_buf_destroy(buf);
}

TEST(max_packet_size) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(4);
    ASSERT(buf != NULL);

    uint8_t pkt[NACK_BUF_MAX_PKT_SIZE];
    memset(pkt, 0xAB, sizeof(pkt));
    rtc_nack_buf_store(buf, pkt, sizeof(pkt), 1, false, 0);

    const uint8_t *out;
    size_t out_len;
    ASSERT(rtc_nack_buf_get(buf, 1, &out, &out_len));
    ASSERT_EQ((int)out_len, NACK_BUF_MAX_PKT_SIZE);
    ASSERT_EQ(out[0], 0xAB);
    ASSERT_EQ(out[NACK_BUF_MAX_PKT_SIZE - 1], 0xAB);

    printf("    max packet size OK (%d bytes)\n", NACK_BUF_MAX_PKT_SIZE);
    rtc_nack_buf_destroy(buf);
}

TEST(zero_length_rejected) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(4);
    ASSERT(buf != NULL);

    uint8_t pkt[4] = {1, 2, 3, 4};
    rtc_nack_buf_store(buf, pkt, 0, 1, false, 0); /* zero length → ignored */

    const uint8_t *out;
    size_t out_len;
    ASSERT(!rtc_nack_buf_get(buf, 1, &out, &out_len));

    printf("    zero length rejected\n");
    rtc_nack_buf_destroy(buf);
}

TEST(sequential_overwrite) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(4);
    ASSERT(buf != NULL);

    uint8_t pkt1[] = {0x01, 0x02};
    uint8_t pkt2[] = {0x03, 0x04, 0x05};
    rtc_nack_buf_store(buf, pkt1, sizeof(pkt1), 10, false, 0);
    rtc_nack_buf_store(buf, pkt2, sizeof(pkt2), 10, false, 0); /* same seq → overwrite */

    const uint8_t *out;
    size_t out_len;
    ASSERT(rtc_nack_buf_get(buf, 10, &out, &out_len));
    ASSERT_EQ((int)out_len, 3); /* latest version */
    ASSERT_MEM_EQ(out, pkt2, sizeof(pkt2));

    printf("    sequential overwrite OK\n");
    rtc_nack_buf_destroy(buf);
}

TEST(retransmit_caps_count) {
    rtc_nack_buf_t *buf = rtc_nack_buf_create(64);
    ASSERT(buf != NULL);

    uint8_t pkt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    rtc_nack_buf_store(buf, pkt, sizeof(pkt), 7, true, 12345);

    const uint8_t *out;
    size_t out_len;
    uint16_t tw;

    for (int i = 0; i < RTC_NACK_MAX_RETRANSMITS; i++) {
        ASSERT(rtc_nack_buf_retransmit(buf, 7, &out, &out_len, &tw));
        ASSERT_EQ((int)out_len, (int)sizeof(pkt));
        ASSERT_EQ((int)tw, 12345);
    }
    /* One past the cap returns false and clears outputs */
    ASSERT(!rtc_nack_buf_retransmit(buf, 7, &out, &out_len, &tw));
    ASSERT(out == NULL);
    ASSERT_EQ((int)out_len, 0);
    ASSERT_EQ((int)tw, 0);

    /* Re-storing the same seq resets the retransmit counter */
    rtc_nack_buf_store(buf, pkt, sizeof(pkt), 7, false, 0);
    ASSERT(rtc_nack_buf_retransmit(buf, 7, &out, &out_len, &tw));
    ASSERT_EQ((int)tw, 0); /* no TWCC this time */

    printf("    retransmit cap + counter reset OK\n");
    rtc_nack_buf_destroy(buf);
}

int main(void) {
    printf("========================================\n");
    printf("  NACK Buffer Tests\n");
    printf("========================================\n\n");

    RUN_TEST(create_destroy);
    RUN_TEST(store_and_get);
    RUN_TEST(store_multiple);
    RUN_TEST(get_missing);
    RUN_TEST(wraparound);
    RUN_TEST(wraparound_seq_wrap);
    RUN_TEST(max_packet_size);
    RUN_TEST(zero_length_rejected);
    RUN_TEST(sequential_overwrite);
    RUN_TEST(retransmit_caps_count);

    TEST_SUMMARY();
}
