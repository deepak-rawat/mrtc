/*
 * Tests for RTP one-byte header extensions (RFC 8285).
 */
#include "rtc/rtc_rtp.h"
#include "rtc/rtc_rtp_ext.h"
#include "test_harness.h"

#include <string.h>

TEST(write_and_parse_single_ext) {
    rtc_rtp_ext_t in[1];
    rtc_rtp_ext_make_transport_cc(&in[0], 5, 0xBEEF);

    uint8_t buf[64] = {0};
    int n = rtc_rtp_ext_write_block(buf, sizeof(buf), in, 1);
    ASSERT(n > 0);
    ASSERT_EQ(n % 4, 0);

    /* Header: 0xBEDE, length-in-words */
    ASSERT_EQ(buf[0], 0xBE);
    ASSERT_EQ(buf[1], 0xDE);
    uint16_t words = ((uint16_t)buf[2] << 8) | buf[3];
    ASSERT_EQ((int)words, (n - 4) / 4);

    rtc_rtp_ext_t out[8];
    size_t cnt = 8;
    int rc = rtc_rtp_ext_parse_body(buf + 4, n - 4, out, &cnt);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ((int)cnt, 1);
    ASSERT_EQ((int)out[0].id, 5);
    ASSERT_EQ((int)out[0].len, 2);
    ASSERT_EQ(rtc_rtp_ext_read_transport_cc(&out[0]), 0xBEEF);
}

TEST(write_and_parse_multiple) {
    rtc_rtp_ext_t in[3];
    rtc_rtp_ext_make_transport_cc(&in[0], 5, 1234);
    rtc_rtp_ext_make_abs_send_time(&in[1], 2, 1000000ULL);
    in[2].id = 9;
    in[2].len = 1;
    in[2].data[0] = 0x80;

    uint8_t buf[64] = {0};
    int n = rtc_rtp_ext_write_block(buf, sizeof(buf), in, 3);
    ASSERT(n > 0);
    ASSERT_EQ(n % 4, 0);

    rtc_rtp_ext_t out[8];
    size_t cnt = 8;
    ASSERT_EQ(rtc_rtp_ext_parse_body(buf + 4, n - 4, out, &cnt), RTC_OK);
    ASSERT_EQ((int)cnt, 3);

    const rtc_rtp_ext_t *t = rtc_rtp_ext_find(out, cnt, 5);
    ASSERT(t != NULL);
    ASSERT_EQ(rtc_rtp_ext_read_transport_cc(t), 1234);

    const rtc_rtp_ext_t *al = rtc_rtp_ext_find(out, cnt, 9);
    ASSERT(al != NULL);
    ASSERT_EQ((int)al->len, 1);
    ASSERT_EQ((int)al->data[0], 0x80);

    const rtc_rtp_ext_t *ast = rtc_rtp_ext_find(out, cnt, 2);
    ASSERT(ast != NULL);
    ASSERT_EQ((int)ast->len, 3);
}

TEST(parse_skips_padding) {
    /* Body: id=3 len=1 data=0x42, then two zero pad bytes */
    uint8_t body[4] = {(3 << 4) | 0, 0x42, 0, 0};
    rtc_rtp_ext_t out[4];
    size_t cnt = 4;
    ASSERT_EQ(rtc_rtp_ext_parse_body(body, sizeof(body), out, &cnt), RTC_OK);
    ASSERT_EQ((int)cnt, 1);
    ASSERT_EQ((int)out[0].id, 3);
    ASSERT_EQ((int)out[0].data[0], 0x42);
}

TEST(parse_stops_on_id15) {
    uint8_t body[8] = {(3 << 4) | 0, 0x11, (15 << 4), 0, (4 << 4) | 0, 0xAA, 0, 0};
    rtc_rtp_ext_t out[4];
    size_t cnt = 4;
    ASSERT_EQ(rtc_rtp_ext_parse_body(body, sizeof(body), out, &cnt), RTC_OK);
    ASSERT_EQ((int)cnt, 1);
    ASSERT_EQ((int)out[0].id, 3);
}

TEST(write_rejects_bad_id) {
    rtc_rtp_ext_t in[1] = {{0, 1, {0}}};
    uint8_t buf[32];
    ASSERT(rtc_rtp_ext_write_block(buf, sizeof(buf), in, 1) < 0);
    in[0].id = 15;
    ASSERT(rtc_rtp_ext_write_block(buf, sizeof(buf), in, 1) < 0);
}

TEST(write_zero_count_emits_empty_block) {
    uint8_t buf[8] = {0xFF, 0xFF, 0xFF, 0xFF};
    int n = rtc_rtp_ext_write_block(buf, sizeof(buf), NULL, 0);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(buf[0], 0xBE);
    ASSERT_EQ(buf[1], 0xDE);
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x00);
}

TEST(rtp_build_with_ext_roundtrip) {
    rtc_rtp_ext_t exts[2];
    rtc_rtp_ext_make_transport_cc(&exts[0], 5, 0xABCD);
    rtc_rtp_ext_make_abs_send_time(&exts[1], 2, 2000000ULL);

    uint8_t payload[10];
    for (int i = 0; i < 10; i++)
        payload[i] = (uint8_t)(0xA0 + i);

    rtc_rtp_packet_t pkt;
    ASSERT_EQ(rtc_rtp_build_with_ext(&pkt, 96, 100, 90000, 0x12345678, true, exts, 2, payload,
                                     sizeof(payload)),
              RTC_OK);
    ASSERT(pkt.buf_len > RTP_HEADER_SIZE + sizeof(payload));
    ASSERT((pkt.buf[0] & 0x10) != 0);

    rtc_rtp_packet_t parsed;
    ASSERT_EQ(rtc_rtp_parse(&parsed, pkt.buf, pkt.buf_len), RTC_OK);
    ASSERT(parsed.header.extension);
    ASSERT(parsed.ext_data != NULL);
    ASSERT(parsed.ext_len >= 1 + 2 + 1 + 3);
    ASSERT_EQ((int)parsed.payload_len, 10);
    ASSERT_EQ((int)parsed.payload[0], 0xA0);

    rtc_rtp_ext_t out[4];
    size_t cnt = 4;
    ASSERT_EQ(rtc_rtp_ext_parse_body(parsed.ext_data, parsed.ext_len, out, &cnt), RTC_OK);
    ASSERT_EQ((int)cnt, 2);
    const rtc_rtp_ext_t *tcc = rtc_rtp_ext_find(out, cnt, 5);
    ASSERT(tcc != NULL);
    ASSERT_EQ(rtc_rtp_ext_read_transport_cc(tcc), 0xABCD);
}

TEST(rtp_build_no_ext_unchanged) {
    uint8_t payload[4] = {1, 2, 3, 4};
    rtc_rtp_packet_t pkt;
    ASSERT_EQ(rtc_rtp_build(&pkt, 96, 1, 0, 0xDEADBEEF, false, payload, 4), RTC_OK);
    ASSERT_EQ((int)pkt.buf_len, RTP_HEADER_SIZE + 4);
    ASSERT_EQ((pkt.buf[0] & 0x10), 0);
}

int main(void) {
    RUN_TEST(write_and_parse_single_ext);
    RUN_TEST(write_and_parse_multiple);
    RUN_TEST(parse_skips_padding);
    RUN_TEST(parse_stops_on_id15);
    RUN_TEST(write_rejects_bad_id);
    RUN_TEST(write_zero_count_emits_empty_block);
    RUN_TEST(rtp_build_with_ext_roundtrip);
    RUN_TEST(rtp_build_no_ext_unchanged);
    return 0;
}
