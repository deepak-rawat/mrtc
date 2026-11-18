/*
 * RTCP component tests.
 *
 * Tests:
 *   1. Build Sender Report and verify structure
 *   2. Build Receiver Report and verify structure
 *   3. Build → parse round-trip for SR
 *   4. Build → parse round-trip for RR
 *   5. Statistics tracking (jitter, loss)
 *   6. RTCP detection (is_rtcp)
 *   7. Parse rejection of invalid packets
 */
#include <rtc/rtc.h>
#include "rtc/rtc_rtcp.h"
#include "test_harness.h"

TEST(rtcp_compound_cname_and_report_boundaries) {
    rtc_rtcp_stats_t stats;
    ASSERT_EQ(rtc_rtcp_stats_init(&stats, 0x11223344), RTC_OK);
    rtc_rtcp_packet_t packet, parsed;
    ASSERT_EQ(rtc_rtcp_build_report(&packet, &stats, false), RTC_OK);
    ASSERT_EQ(rtc_rtcp_parse(&parsed, packet.buf, packet.buf_len), RTC_OK);
    ASSERT_EQ(parsed.report_count, 0);
    ASSERT_EQ(packet.buf[8], 0x81);
    ASSERT_EQ(packet.buf[9], RTCP_PT_SDES);
    ASSERT_EQ(packet.buf[16], 1);
    ASSERT_EQ(packet.buf[17], strlen(rtc_rtcp_cname()));
    ASSERT_MEM_EQ(packet.buf + 18, rtc_rtcp_cname(), packet.buf[17]);
    ASSERT_EQ(packet.buf_len % 4, 0);
    packet.buf[0] = 0x81;
    /* A following SDES chunk must not satisfy a missing RR report block. */
    ASSERT_EQ(rtc_rtcp_parse(&parsed, packet.buf, packet.buf_len), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_rtcp_build_report(&packet, &stats, true), RTC_OK);
    ASSERT_EQ(rtc_rtcp_parse(&parsed, packet.buf, packet.buf_len), RTC_OK);
    ASSERT_EQ(packet.buf[29], RTCP_PT_SDES);
}

TEST(rtcp_interval_loss_and_late_rollover) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_packet_t packet;
    rtc_rtcp_stats_init(&stats, 1);
    rtc_rtcp_stats_on_rtp_recv(&stats, 65534, 0, 2, 0);
    rtc_rtcp_stats_on_rtp_recv(&stats, 1, 960, 2, 0);
    ASSERT_EQ(stats.highest_seq, 65537);
    ASSERT_EQ(stats.packets_expected, 4);
    rtc_rtcp_stats_on_rtp_recv(&stats, 65535, 480, 2, 0);
    ASSERT_EQ(stats.highest_seq, 65537);
    ASSERT_EQ(rtc_rtcp_build_rr(&packet, &stats), RTC_OK);
    ASSERT_EQ(packet.reports[0].fraction_lost, 64);
    rtc_rtcp_stats_report_sent(&stats);
    rtc_rtcp_stats_on_rtp_recv(&stats, 2, 1920, 2, 0);
    ASSERT_EQ(rtc_rtcp_build_rr(&packet, &stats), RTC_OK);
    ASSERT_EQ(packet.reports[0].fraction_lost, 0);
    ASSERT_EQ(packet.reports[0].cumulative_lost, 1);
}

TEST(rtcp_build_sr) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0xDEADBEEF);

    /* Simulate sending some packets */
    for (int i = 0; i < 100; i++) {
        rtc_rtcp_stats_on_rtp_send(&stats, (uint32_t)(i * 960), 160);
    }
    ASSERT_EQ(stats.packets_sent, 100);
    ASSERT_EQ(stats.octets_sent, 16000);

    /* Build SR */
    rtc_rtcp_packet_t pkt;
    int rc = rtc_rtcp_build_sr(&pkt, &stats);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(pkt.header.version, 2);
    ASSERT_EQ(pkt.header.packet_type, RTCP_PT_SR);
    ASSERT(pkt.buf_len >= 28); /* header(4) + SSRC(4) + sender-info(20) */
    ASSERT_EQ(pkt.sender_ssrc, 0xDEADBEEF);
    ASSERT_EQ(pkt.sr.sender_pkt_count, 100);
    ASSERT_EQ(pkt.sr.sender_oct_count, 16000);

    printf("    SR built: %zu bytes, pkts=%u octets=%u\n", pkt.buf_len, pkt.sr.sender_pkt_count,
           pkt.sr.sender_oct_count);
}

TEST(rtcp_remb_bounds_and_quantization) {
    uint8_t wire[64];
    size_t length;
    const uint32_t rates[] = {0, 128000, 800000, UINT32_MAX};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        ASSERT_EQ(rtc_rtcp_build_remb(wire, sizeof(wire), &length, 1, 0x12345678, rates[i]),
                  RTC_OK);
        rtc_rtcp_remb_t parsed;
        ASSERT_EQ(rtc_rtcp_parse_remb(&parsed, wire, length), RTC_OK);
        ASSERT_EQ(parsed.media_ssrc, 0x12345678);
        ASSERT(parsed.bitrate_bps <= rates[i]);
        ASSERT(rates[i] - parsed.bitrate_bps < 16384);
        ASSERT_EQ(rtc_rtcp_parse_remb(&parsed, wire, length - 1), RTC_ERR_INVALID);
    }
    rtc_rtcp_remb_t parsed;
    wire[16] = 2;
    ASSERT_EQ(rtc_rtcp_parse_remb(&parsed, wire, length), RTC_ERR_INVALID);
    wire[16] = 1;
    wire[17] = 0xfc;
    wire[18] = wire[19] = 0;
    ASSERT_EQ(rtc_rtcp_parse_remb(&parsed, wire, length), RTC_OK);
    ASSERT_EQ(parsed.bitrate_bps, 0);
}

TEST(rtcp_feedback_is_compound_without_reduced_size) {
    uint8_t wire[256], original[64];
    size_t length;
    ASSERT_EQ(rtc_rtcp_build_pli(wire, sizeof(wire), &length, 0x11223344, 0x55667788), RTC_OK);
    size_t feedback_length = length;
    memcpy(original, wire, length);
    ASSERT_EQ(rtc_rtcp_compound_feedback(wire, &length, feedback_length), RTC_ERR_NOMEM);
    ASSERT_MEM_EQ(wire, original, feedback_length);
    ASSERT_EQ(rtc_rtcp_compound_feedback(wire, &length, sizeof(wire)), RTC_OK);
    ASSERT_EQ(wire[1], RTCP_PT_RR);
    ASSERT_EQ(wire[9], RTCP_PT_SDES);
    ASSERT_MEM_EQ(wire + length - feedback_length, original, feedback_length);
    size_t compound_length = length;
    ASSERT_EQ(rtc_rtcp_compound_feedback(wire, &length, sizeof(wire)), RTC_OK);
    ASSERT_EQ(length, compound_length);
}

TEST(rtcp_build_rr) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x11111111);

    /* Simulate receiving packets from a remote source */
    for (uint16_t i = 0; i < 50; i++) {
        rtc_rtcp_stats_on_rtp_recv(&stats, i, i * 960, 0x22222222, 48000);
    }

    ASSERT_EQ(stats.packets_received, 50);
    ASSERT_EQ(stats.remote_ssrc, 0x22222222);

    /* Build RR */
    rtc_rtcp_packet_t pkt;
    int rc = rtc_rtcp_build_rr(&pkt, &stats);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(pkt.header.version, 2);
    ASSERT_EQ(pkt.header.packet_type, RTCP_PT_RR);
    ASSERT_EQ(pkt.header.count, 1); /* one report block */
    ASSERT_EQ(pkt.sender_ssrc, 0x11111111);
    ASSERT_EQ(pkt.report_count, 1);
    ASSERT_EQ(pkt.reports[0].ssrc, 0x22222222);

    printf("    RR built: %zu bytes, 1 report block for SSRC 0x%08X\n", pkt.buf_len,
           pkt.reports[0].ssrc);
}

TEST(rtcp_sr_roundtrip) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0xAAAABBBB);

    for (int i = 0; i < 200; i++)
        rtc_rtcp_stats_on_rtp_send(&stats, (uint32_t)(i * 960), 320);

    /* Also receive some packets to get a report block */
    for (uint16_t i = 0; i < 100; i++)
        rtc_rtcp_stats_on_rtp_recv(&stats, i, i * 960, 0xCCCCDDDD, 48000);

    rtc_rtcp_packet_t built;
    int rc = rtc_rtcp_build_sr(&built, &stats);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse it back */
    rtc_rtcp_packet_t parsed;
    rc = rtc_rtcp_parse(&parsed, built.buf, built.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(parsed.header.version, 2);
    ASSERT_EQ(parsed.header.packet_type, RTCP_PT_SR);
    ASSERT_EQ(parsed.sender_ssrc, 0xAAAABBBB);
    ASSERT_EQ(parsed.sr.sender_pkt_count, 200);
    ASSERT_EQ(parsed.sr.sender_oct_count, 64000);
    ASSERT_EQ(parsed.report_count, 1);
    ASSERT_EQ(parsed.reports[0].ssrc, 0xCCCCDDDD);
    ASSERT_EQ(parsed.reports[0].highest_seq, 99);

    printf("    SR round-trip: pkts=%u octets=%u reports=%d\n", parsed.sr.sender_pkt_count,
           parsed.sr.sender_oct_count, parsed.report_count);
}

TEST(rtcp_rr_roundtrip) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x55556666);

    for (uint16_t i = 0; i < 30; i++)
        rtc_rtcp_stats_on_rtp_recv(&stats, i, i * 3000, 0x77778888, 90000);

    rtc_rtcp_packet_t built;
    int rc = rtc_rtcp_build_rr(&built, &stats);
    ASSERT_EQ(rc, RTC_OK);

    rtc_rtcp_packet_t parsed;
    rc = rtc_rtcp_parse(&parsed, built.buf, built.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(parsed.header.packet_type, RTCP_PT_RR);
    ASSERT_EQ(parsed.sender_ssrc, 0x55556666);
    ASSERT_EQ(parsed.report_count, 1);
    ASSERT_EQ(parsed.reports[0].ssrc, 0x77778888);
    ASSERT_EQ(parsed.reports[0].highest_seq, 29);
    ASSERT_EQ(parsed.reports[0].cumulative_lost, 0); /* no loss */
    ASSERT_EQ(parsed.reports[0].fraction_lost, 0);

    printf("    RR round-trip: highest_seq=%u lost=%d\n", parsed.reports[0].highest_seq,
           parsed.reports[0].cumulative_lost);
}

TEST(rtcp_stats_loss) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x12345678);

    /* Receive packets 0, 1, 2, then skip 3, 4, receive 5 */
    rtc_rtcp_stats_on_rtp_recv(&stats, 0, 0, 0xAABBCCDD, 48000);
    rtc_rtcp_stats_on_rtp_recv(&stats, 1, 960, 0xAABBCCDD, 48000);
    rtc_rtcp_stats_on_rtp_recv(&stats, 2, 1920, 0xAABBCCDD, 48000);
    /* Skip 3 and 4 */
    rtc_rtcp_stats_on_rtp_recv(&stats, 5, 4800, 0xAABBCCDD, 48000);

    /* Expected: seq 0-5 = 6 packets expected, 4 received, 2 lost */
    ASSERT_EQ(stats.packets_received, 4);
    ASSERT_EQ(stats.highest_seq, 5);
    ASSERT_EQ(stats.packets_lost, 2);

    printf("    loss tracking: received=%u expected=%u lost=%u\n", stats.packets_received,
           stats.packets_expected, stats.packets_lost);
}

/* RFC 3550 §5.1: real streams pick a random initial seq. */
TEST(rtcp_stats_loss_nonzero_start) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x12345678);

    /* Start at seq 12345, receive 5 packets cleanly */
    for (uint16_t i = 0; i < 5; i++)
        rtc_rtcp_stats_on_rtp_recv(&stats, (uint16_t)(12345 + i), i * 960, 0xAABBCCDD, 48000);

    ASSERT_EQ(stats.packets_received, 5);
    ASSERT_EQ(stats.packets_expected, 5);
    ASSERT_EQ(stats.packets_lost, 0);

    /* Skip 12350-12351 and receive 12352 — one gap of 2 lost. */
    rtc_rtcp_stats_on_rtp_recv(&stats, 12352, 7 * 960, 0xAABBCCDD, 48000);
    ASSERT_EQ(stats.packets_received, 6);
    ASSERT_EQ(stats.packets_expected, 8);
    ASSERT_EQ(stats.packets_lost, 2);

    printf("    nonzero-start loss: base=%u highest=%u received=%u expected=%u lost=%u\n",
           stats.base_seq, stats.highest_seq, stats.packets_received, stats.packets_expected,
           stats.packets_lost);
}

TEST(rtcp_is_rtcp) {
    /* Build a valid SR and check detection */
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x11112222);
    rtc_rtcp_stats_on_rtp_send(&stats, 960, 160);

    rtc_rtcp_packet_t pkt;
    rtc_rtcp_build_sr(&pkt, &stats);

    ASSERT(rtc_rtcp_is_rtcp(pkt.buf, pkt.buf_len));

    /* RTP packet should NOT be detected as RTCP */
    uint8_t rtp_data[20] = {0x80, 0x60, 0x00, 0x01, /* version=2, pt=96 */
                            0x00, 0x00, 0x03, 0xC0, 0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT(!rtc_rtcp_is_rtcp(rtp_data, sizeof(rtp_data)));

    /* Too short */
    uint8_t short_data[2] = {0x80, 0xC8};
    ASSERT(!rtc_rtcp_is_rtcp(short_data, sizeof(short_data)));

    printf("    RTCP detection: correct for SR, RTP, and short data\n");
}

TEST(rtcp_parse_invalid) {
    rtc_rtcp_packet_t pkt;

    /* Too short */
    uint8_t short_data[4] = {0x80, 0xC8, 0x00, 0x06};
    ASSERT(rtc_rtcp_parse(&pkt, short_data, sizeof(short_data)) != RTC_OK);

    /* Wrong version */
    uint8_t bad_version[8] = {0x40, 0xC8, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    ASSERT(rtc_rtcp_parse(&pkt, bad_version, sizeof(bad_version)) != RTC_OK);

    /* Length too long for available data */
    uint8_t bad_length[8] = {0x80, 0xC8, 0x00, 0x10, /* claims 64 bytes */
                             0x00, 0x00, 0x00, 0x00};
    ASSERT(rtc_rtcp_parse(&pkt, bad_length, sizeof(bad_length)) != RTC_OK);

    uint8_t padded_rr[12] = {0xa0, RTCP_PT_RR, 0, 2, 0, 0, 0, 1, 0, 0, 0, 4};
    ASSERT_EQ(rtc_rtcp_parse(&pkt, padded_rr, sizeof(padded_rr)), RTC_OK);
    ASSERT_EQ(pkt.buf_len, sizeof(padded_rr));
    ASSERT_MEM_EQ(pkt.buf, padded_rr, sizeof(padded_rr));
    padded_rr[11] = 0;
    ASSERT_EQ(rtc_rtcp_parse(&pkt, padded_rr, sizeof(padded_rr)), RTC_ERR_INVALID);
    padded_rr[11] = 1;
    ASSERT_EQ(rtc_rtcp_parse(&pkt, padded_rr, sizeof(padded_rr)), RTC_ERR_INVALID);

    printf("    correctly rejected invalid RTCP packets\n");
}

TEST(rtcp_sr_no_reports) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x99998888);

    /* Only send, never receive — no report blocks */
    for (int i = 0; i < 10; i++)
        rtc_rtcp_stats_on_rtp_send(&stats, (uint32_t)(i * 960), 160);

    rtc_rtcp_packet_t pkt;
    int rc = rtc_rtcp_build_sr(&pkt, &stats);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(pkt.header.count, 0);
    ASSERT_EQ(pkt.report_count, 0);

    /* Parse back */
    rtc_rtcp_packet_t parsed;
    rc = rtc_rtcp_parse(&parsed, pkt.buf, pkt.buf_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.header.count, 0);
    ASSERT_EQ(parsed.report_count, 0);
    ASSERT_EQ(parsed.sr.sender_pkt_count, 10);

    printf("    SR with no report blocks: pkts=%u\n", parsed.sr.sender_pkt_count);
}

TEST(rtcp_sr_recv_and_rtt_helpers) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x11111111);

    rtc_rtcp_sr_t sr;
    memset(&sr, 0, sizeof(sr));
    sr.ntp_sec = 0x12345678;
    sr.ntp_frac = 0x9ABCDEF0;
    rtc_rtcp_stats_on_sr_recv(&stats, &sr);
    ASSERT_EQ(stats.last_sr_ntp, 0x56789ABCu);
    ASSERT(stats.last_sr_recv_time > 0);

    /* Build last_sr on the same wall clock rtc_rtcp_rtt_from_rr now uses. */
    uint64_t now_ms = rtc_time_unix_ms();
    uint32_t now_sec = (uint32_t)(now_ms / 1000) + 2208988800u;
    uint32_t now_frac = (uint32_t)((now_ms % 1000) * 4294967ULL);
    uint32_t now_compact = (now_sec & 0xFFFF) << 16 | (now_frac >> 16);

    rtc_rtcp_rr_block_t rr;
    memset(&rr, 0, sizeof(rr));
    rr.last_sr = now_compact - (uint32_t)((100ULL * 65536ULL) / 1000ULL);
    int rtt_ms = rtc_rtcp_rtt_from_rr(&rr);
    ASSERT(rtt_ms >= 90 && rtt_ms <= 200);

    printf("    SR receive + RTT helpers: rtt=%dms\n", rtt_ms);
}

int main(void) {
    printf("========================================\n");
    printf("  RTCP Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(rtcp_build_sr);
    RUN_TEST(rtcp_remb_bounds_and_quantization);
    RUN_TEST(rtcp_feedback_is_compound_without_reduced_size);
    RUN_TEST(rtcp_compound_cname_and_report_boundaries);
    RUN_TEST(rtcp_interval_loss_and_late_rollover);
    RUN_TEST(rtcp_build_rr);
    RUN_TEST(rtcp_sr_roundtrip);
    RUN_TEST(rtcp_rr_roundtrip);
    RUN_TEST(rtcp_stats_loss);
    RUN_TEST(rtcp_stats_loss_nonzero_start);
    RUN_TEST(rtcp_is_rtcp);
    RUN_TEST(rtcp_parse_invalid);
    RUN_TEST(rtcp_sr_no_reports);
    RUN_TEST(rtcp_sr_recv_and_rtt_helpers);

    rtc_cleanup();
    TEST_SUMMARY();
}
