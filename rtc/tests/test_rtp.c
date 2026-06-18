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
#include "rtc/rtc_rtp.h"
#include "rtc/rtc_rtp_stream.h"
#include "test_harness.h"

static int g_frame_count;
static uint16_t g_frame_seq;
static uint32_t g_frame_ssrc;
static int g_nack_count;
static int g_pli_count;

static void stream_on_frame(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                            uint32_t ssrc, bool marker, void *user) {
    (void)payload;
    (void)len;
    (void)timestamp;
    (void)marker;
    (void)user;
    g_frame_count++;
    g_frame_seq = seq;
    g_frame_ssrc = ssrc;
}

static void stream_on_nack(const uint16_t *lost_seqs, int count, void *user) {
    (void)lost_seqs;
    (void)user;
    g_nack_count += count;
}

static void stream_on_pli(void *user) {
    (void)user;
    g_pli_count++;
}

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

TEST(rtp_parse_too_short) {
    uint8_t short_data[4] = {0x80, 0x60, 0x00, 0x01};
    rtc_rtp_packet_t pkt;
    int rc = rtc_rtp_parse(&pkt, short_data, sizeof(short_data));
    ASSERT(rc != RTC_OK);
    printf("    correctly rejected %zu-byte packet\n", sizeof(short_data));
}

TEST(rtp_recv_stream_on_packet) {
    g_frame_count = 0;
    g_frame_seq = 0;
    g_frame_ssrc = 0;

    rtc_rtp_recv_stream_t *receiver = rtc_rtp_recv_stream_create(&(rtc_rtp_recv_stream_config_t){
        .payload_type = 96,
        .clock_rate = 90000,
        .local_ssrc = 0x11111111,
    });
    ASSERT(receiver != NULL);
    rtc_rtp_recv_stream_set_active(receiver, true);
    rtc_rtp_recv_stream_on_frame(receiver, stream_on_frame, NULL);

    uint8_t payload[] = {0x01, 0x02, 0x03};
    rtc_rtp_packet_t pkt;
    ASSERT_EQ(rtc_rtp_build(&pkt, 96, 55, 9000, 0x22222222, true, payload, sizeof(payload)),
              RTC_OK);
    rtc_rtp_recv_stream_on_packet(receiver, &pkt);

    ASSERT_EQ(g_frame_count, 1);
    ASSERT_EQ(g_frame_seq, 55);
    ASSERT_EQ(g_frame_ssrc, 0x22222222u);
    const rtc_rtcp_stats_t *stats = rtc_rtp_recv_stream_stats(receiver);
    ASSERT_EQ(stats->packets_received, 1);
    ASSERT_EQ(stats->remote_ssrc, 0x22222222u);

    rtc_rtp_recv_stream_destroy(receiver);
    printf("    receive stream: stats updated and frame callback fired\n");
}

TEST(rtp_send_stream_feedback) {
    g_nack_count = 0;
    g_pli_count = 0;

    rtc_rtp_send_stream_t *sender = rtc_rtp_send_stream_create(&(rtc_rtp_send_stream_config_t){
        .payload_type = 96,
        .clock_rate = 90000,
    });
    ASSERT(sender != NULL);
    rtc_rtp_send_stream_on_nack(sender, stream_on_nack, NULL);
    rtc_rtp_send_stream_on_pli(sender, stream_on_pli, NULL);

    uint16_t lost[] = {10, 11, 12};
    rtc_rtp_send_stream_handle_nack(sender, lost, 3);
    ASSERT_EQ(g_nack_count, 3);

    rtc_rtp_send_stream_handle_pli(sender);
    rtc_rtp_send_stream_handle_pli(sender);
    ASSERT_EQ(g_pli_count, 1);

    rtc_rtp_send_stream_destroy(sender);
    printf("    send stream: NACK notify + PLI rate limit OK\n");
}

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
    RUN_TEST(rtp_recv_stream_on_packet);
    RUN_TEST(rtp_send_stream_feedback);

    rtc_cleanup();
    TEST_SUMMARY();
}
