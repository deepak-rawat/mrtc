/*
 * Tests for Transport-Wide Congestion Control (sender + receiver + parse).
 */
#include "rtc_twcc_sender.h"
#include "rtc_twcc_receiver.h"
#include "rtc_rtcp.h"
#include "test_harness.h"

#include <string.h>

TEST(sender_assigns_sequential) {
    rtc_twcc_sender_t s;
    rtc_twcc_sender_init(&s);
    uint16_t a = rtc_twcc_sender_assign(&s, 1000, 100);
    uint16_t b = rtc_twcc_sender_assign(&s, 2000, 100);
    uint16_t c = rtc_twcc_sender_assign(&s, 3000, 100);
    ASSERT_EQ((int)a, 0);
    ASSERT_EQ((int)b, 1);
    ASSERT_EQ((int)c, 2);

    const rtc_twcc_sent_pkt_t *e = rtc_twcc_sender_lookup(&s, 1);
    ASSERT(e != NULL);
    ASSERT_EQ((int)e->send_time_us, 2000);
    ASSERT_EQ((int)e->size, 100);
}

TEST(sender_wraparound) {
    rtc_twcc_sender_t s;
    rtc_twcc_sender_init(&s);
    for (int i = 0; i < RTC_TWCC_SENDER_RING + 5; i++)
        rtc_twcc_sender_assign(&s, (uint64_t)i * 1000, 200);
    /* Old seqs overwritten — lookup of seq 0 returns NULL (slot now holds RING) */
    const rtc_twcc_sent_pkt_t *e = rtc_twcc_sender_lookup(&s, 0);
    ASSERT(e == NULL);
    /* Recent seq is fine */
    e = rtc_twcc_sender_lookup(&s, (uint16_t)(RTC_TWCC_SENDER_RING + 4));
    ASSERT(e != NULL);
}

TEST(feedback_roundtrip_all_received) {
    rtc_twcc_receiver_t r;
    rtc_twcc_receiver_init(&r);

    /* 5 packets, 10ms apart */
    uint64_t base = 1000000ULL; /* 1.0s */
    for (int i = 0; i < 5; i++)
        rtc_twcc_receiver_on_packet(&r, (uint16_t)(100 + i), base + (uint64_t)i * 10000);

    uint8_t fb[256];
    size_t fb_len = 0;
    ASSERT_EQ(rtc_twcc_receiver_build_feedback(&r, 0xAAAA1111, 0xBBBB2222, fb, sizeof(fb), &fb_len),
              RTC_OK);
    ASSERT(fb_len >= 20);
    ASSERT_EQ((fb_len % 4), 0u);
    ASSERT_EQ((int)fb[0], 0x80 | 15);
    ASSERT_EQ((int)fb[1], 205);

    rtc_rtcp_twcc_t out;
    ASSERT_EQ(rtc_rtcp_parse_twcc(&out, fb, fb_len), RTC_OK);
    ASSERT_EQ((int)out.sender_ssrc, (int)0xAAAA1111);
    ASSERT_EQ((int)out.media_ssrc, (int)0xBBBB2222);
    ASSERT_EQ((int)out.base_seq, 100);
    ASSERT_EQ((int)out.item_count, 5);
    for (int i = 0; i < 5; i++) {
        ASSERT(out.items[i].received);
        ASSERT_EQ((int)out.items[i].seq, 100 + i);
    }
    /* Delta between consecutive packets should be ~10ms (10000us), within
     * one 250µs tick of quantization error and one 64ms reference quantize. */
    int64_t d = (int64_t)out.items[1].recv_time_us - (int64_t)out.items[0].recv_time_us;
    ASSERT(d >= 9500 && d <= 10500);
}

TEST(feedback_with_gaps) {
    rtc_twcc_receiver_t r;
    rtc_twcc_receiver_init(&r);

    uint64_t base = 2000000ULL;
    rtc_twcc_receiver_on_packet(&r, 10, base);
    /* seq 11 missing */
    rtc_twcc_receiver_on_packet(&r, 12, base + 20000);
    /* seq 13 missing */
    rtc_twcc_receiver_on_packet(&r, 14, base + 40000);

    uint8_t fb[256];
    size_t fb_len = 0;
    ASSERT_EQ(rtc_twcc_receiver_build_feedback(&r, 1, 2, fb, sizeof(fb), &fb_len), RTC_OK);

    rtc_rtcp_twcc_t out;
    ASSERT_EQ(rtc_rtcp_parse_twcc(&out, fb, fb_len), RTC_OK);
    ASSERT_EQ((int)out.item_count, 5);
    ASSERT(out.items[0].received);
    ASSERT(!out.items[1].received);
    ASSERT(out.items[2].received);
    ASSERT(!out.items[3].received);
    ASSERT(out.items[4].received);
}

TEST(empty_feedback_rejected) {
    rtc_twcc_receiver_t r;
    rtc_twcc_receiver_init(&r);
    uint8_t fb[64];
    size_t fb_len = 0;
    ASSERT(rtc_twcc_receiver_build_feedback(&r, 1, 2, fb, sizeof(fb), &fb_len) != RTC_OK);
}

int main(void) {
    RUN_TEST(sender_assigns_sequential);
    RUN_TEST(sender_wraparound);
    RUN_TEST(feedback_roundtrip_all_received);
    RUN_TEST(feedback_with_gaps);
    RUN_TEST(empty_feedback_rejected);
    return 0;
}
