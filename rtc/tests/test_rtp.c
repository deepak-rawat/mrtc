/*
 * RTP component tests.
 *
 * Tests:
 *   1. Build an RTP packet and verify header fields
 *   2. Parse a serialized RTP packet
 *   3. Build → parse round-trip with payload verification
 *   4. RTP session: auto-incrementing sequence and timestamp
 *   5. Marker bit handling
 *   6. Maximum payload size
 */
#include <rtc/rtc.h>
#include "rtc_rtp.h"
#include "test_harness.h"

/* ------------------------------------------------------------------ */
/*  Test: build a packet and verify header fields in the buffer        */
/* ------------------------------------------------------------------ */
TEST(rtp_build_basic) {
    uint8_t payload[] = "Hello RTP";
    rtc_rtp_packet_t pkt;
    int rc = rtc_rtp_build(&pkt, 111, 1, 960, 0xDEADBEEF, false, payload, sizeof(payload) - 1);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(pkt.buf_len, RTP_HEADER_SIZE + sizeof(payload) - 1);

    /* Version 2 in the first byte (top 2 bits) */
    ASSERT_EQ((pkt.buf[0] >> 6) & 0x03, RTP_VERSION);

    /* Payload type in second byte (lower 7 bits) */
    ASSERT_EQ(pkt.buf[1] & 0x7F, 111);

    /* Sequence number (big-endian at offset 2) */
    uint16_t seq = ((uint16_t)pkt.buf[2] << 8) | pkt.buf[3];
    ASSERT_EQ(seq, 1);

    /* Timestamp (big-endian at offset 4) */
    uint32_t ts = ((uint32_t)pkt.buf[4] << 24) | ((uint32_t)pkt.buf[5] << 16) |
                  ((uint32_t)pkt.buf[6] << 8) | pkt.buf[7];
    ASSERT_EQ(ts, 960);

    /* SSRC (big-endian at offset 8) */
    uint32_t ssrc = ((uint32_t)pkt.buf[8] << 24) | ((uint32_t)pkt.buf[9] << 16) |
                    ((uint32_t)pkt.buf[10] << 8) | pkt.buf[11];
    ASSERT_EQ(ssrc, 0xDEADBEEF);

    /* Payload follows header */
    ASSERT_MEM_EQ(pkt.buf + RTP_HEADER_SIZE, payload, sizeof(payload) - 1);

    printf("    built %zu bytes: pt=%d seq=%u ts=%u ssrc=0x%08X\n", pkt.buf_len, 111, seq, ts,
           ssrc);
}

/* ------------------------------------------------------------------ */
/*  Test: parse a serialized packet back                               */
/* ------------------------------------------------------------------ */
TEST(rtp_parse) {
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    rtc_rtp_packet_t built;
    rtc_rtp_build(&built, 96, 42, 12345, 0x12345678, true, payload, sizeof(payload));

    rtc_rtp_packet_t parsed;
    int rc = rtc_rtp_parse(&parsed, built.buf, built.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(parsed.header.version, RTP_VERSION);
    ASSERT_EQ(parsed.header.payload_type, 96);
    ASSERT_EQ(parsed.header.seq, 42);
    ASSERT_EQ(parsed.header.timestamp, 12345);
    ASSERT_EQ(parsed.header.ssrc, 0x12345678);
    ASSERT_EQ(parsed.header.marker, true);
    ASSERT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_MEM_EQ(parsed.payload, payload, sizeof(payload));

    printf("    parsed: pt=%u seq=%u ts=%u ssrc=0x%08X marker=%d payload_len=%zu\n",
           parsed.header.payload_type, parsed.header.seq, parsed.header.timestamp,
           parsed.header.ssrc, parsed.header.marker, parsed.payload_len);
}

/* ------------------------------------------------------------------ */
/*  Test: build → parse round-trip preserves all fields                */
/* ------------------------------------------------------------------ */
TEST(rtp_roundtrip) {
    uint8_t payload[200];
    memset(payload, 0xAB, sizeof(payload));

    rtc_rtp_packet_t built;
    int rc = rtc_rtp_build(&built, 111, 1000, 48000, 0xCAFEBABE, false, payload, sizeof(payload));
    ASSERT_EQ(rc, RTC_OK);

    rtc_rtp_packet_t parsed;
    rc = rtc_rtp_parse(&parsed, built.buf, built.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(parsed.header.payload_type, 111);
    ASSERT_EQ(parsed.header.seq, 1000);
    ASSERT_EQ(parsed.header.timestamp, 48000);
    ASSERT_EQ(parsed.header.ssrc, 0xCAFEBABE);
    ASSERT_EQ(parsed.header.marker, false);
    ASSERT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_MEM_EQ(parsed.payload, payload, sizeof(payload));

    printf("    round-trip: 200-byte payload preserved\n");
}

/* ------------------------------------------------------------------ */
/*  Test: RTP session tracks sequence and timestamp                    */
/* ------------------------------------------------------------------ */
TEST(rtp_session_tracking) {
    rtc_rtp_session_t sess;
    int rc = rtc_rtp_session_init(&sess, 111, 48000);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(sess.ssrc != 0); /* random SSRC */

    printf("    session ssrc=0x%08X initial_seq=%u\n", sess.ssrc, sess.seq);

    uint16_t initial_seq = sess.seq;
    uint32_t initial_ts = sess.timestamp;

    uint8_t payload[10] = {0};

    /* Send 5 packets, each with 960 samples (20ms at 48kHz) */
    for (int i = 0; i < 5; i++) {
        rtc_rtp_packet_t pkt;
        rc = rtc_rtp_session_send(&sess, &pkt, payload, sizeof(payload), 960, (i == 0));
        ASSERT_EQ(rc, RTC_OK);

        /* Parse back and verify fields */
        rtc_rtp_packet_t parsed;
        rc = rtc_rtp_parse(&parsed, pkt.buf, pkt.buf_len);
        ASSERT_EQ(rc, RTC_OK);

        ASSERT_EQ(parsed.header.seq, (uint16_t)(initial_seq + i));
        ASSERT_EQ(parsed.header.timestamp, initial_ts + (uint32_t)(i * 960));
        ASSERT_EQ(parsed.header.ssrc, sess.ssrc);
        ASSERT_EQ(parsed.header.marker, (i == 0));

        printf("    pkt[%d]: seq=%u ts=%u marker=%d\n", i, parsed.header.seq,
               parsed.header.timestamp, parsed.header.marker);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: reject too-short data                                        */
/* ------------------------------------------------------------------ */
TEST(rtp_parse_too_short) {
    uint8_t short_data[4] = {0x80, 0x60, 0x00, 0x01};
    rtc_rtp_packet_t pkt;
    int rc = rtc_rtp_parse(&pkt, short_data, sizeof(short_data));
    ASSERT(rc != RTC_OK);
    printf("    correctly rejected %zu-byte packet\n", sizeof(short_data));
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  RTP Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(rtp_build_basic);
    RUN_TEST(rtp_parse);
    RUN_TEST(rtp_roundtrip);
    RUN_TEST(rtp_session_tracking);
    RUN_TEST(rtp_parse_too_short);

    rtc_cleanup();
    TEST_SUMMARY();
}
