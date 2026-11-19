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
    ASSERT_EQ(rtc_rtcp_build_report(&packet, NULL, false), RTC_ERR_INVALID);
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

TEST(rtcp_bye_lists_reasons_padding_and_atomic_validation) {
    uint8_t padded[] = {0xa2, RTCP_PT_BYE, 0,    6,    0,   0,   0,   11,  0,   0,
                        0,    22,          7,    'l',  'e', 'a', 'v', 'i', 'n', 'g',
                        0x11, 0x22,        0x33, 0x44, 0,   0,   0,   8};
    rtc_rtcp_bye_t bye;
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, padded, sizeof(padded)), RTC_OK);
    ASSERT_EQ(bye.count, 2);
    ASSERT_EQ(bye.ssrcs[0], 11);
    ASSERT_EQ(bye.ssrcs[1], 22);
    ASSERT_EQ(rtc_rtcp_validate_compound(padded, sizeof(padded)), RTC_OK);
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, padded, sizeof(padded) - 1), RTC_ERR_INVALID);
    bye.count = 7;
    padded[0] = 0xa3;
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, padded, sizeof(padded)), RTC_ERR_INVALID);
    ASSERT_EQ(bye.count, 7);
    padded[0] = 0xa2;
    padded[27] = 0;
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, padded, sizeof(padded)), RTC_ERR_INVALID);
    padded[27] = 25;
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, padded, sizeof(padded)), RTC_ERR_INVALID);
    padded[27] = 8;
    uint8_t compound[36];
    memcpy(compound, padded, sizeof(padded));
    const uint8_t rr[] = {0x80, RTCP_PT_RR, 0, 1, 0, 0, 0, 1};
    memcpy(compound + sizeof(padded), rr, sizeof(rr));
    ASSERT_EQ(rtc_rtcp_validate_compound(compound, sizeof(compound)), RTC_ERR_INVALID);

    uint8_t reason[] = {0x81, RTCP_PT_BYE, 0, 3, 0, 0, 0, 1, 4, 'b', 'y', 'e', '!', 0, 0, 0};
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, reason, sizeof(reason)), RTC_OK);
    reason[15] = 1;
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, reason, sizeof(reason)), RTC_ERR_INVALID);
    const uint8_t empty[] = {0x80, RTCP_PT_BYE, 0, 0};
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, empty, sizeof(empty)), RTC_OK);
    ASSERT_EQ(bye.count, 0);
    uint8_t maximum[128] = {0x9f, RTCP_PT_BYE, 0, 31};
    for (unsigned i = 0; i < 31; i++)
        maximum[7 + i * 4] = (uint8_t)(i + 1);
    ASSERT_EQ(rtc_rtcp_parse_bye(&bye, maximum, sizeof(maximum)), RTC_OK);
    ASSERT_EQ(bye.count, 31);
    ASSERT_EQ(bye.ssrcs[30], 31);
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

TEST(rtcp_randomized_bandwidth_intervals_and_bye) {
    uint32_t minimum = rtc_rtcp_interval_ms(6400, 100, 4, 2, true, false, 0);
    uint32_t maximum = rtc_rtcp_interval_ms(6400, 100, 4, 2, true, false, UINT32_MAX);
    ASSERT(minimum >= 2050 && minimum <= 2053);
    ASSERT(maximum >= 6155 && maximum <= 6158);
    uint32_t constrained = rtc_rtcp_interval_ms(8000, 100, 100, 10, false, false, UINT32_MAX);
    ASSERT(constrained >= 14774 && constrained <= 14777);
    ASSERT(rtc_rtcp_interval_ms(6400, 100, 4, 2, true, true, UINT32_MAX) < minimum * 2);
    rtc_rtcp_stats_t stats;
    ASSERT_EQ(rtc_rtcp_stats_init(&stats, 0x12345678), RTC_OK);
    ASSERT_EQ(rtc_rtcp_report_due_at(&stats, 128000, 100, 10000, 0), RTC_ERR_TIMEOUT);
    ASSERT(stats.next_report_time > 10000 && stats.next_report_time <= 13079);
    ASSERT_EQ(rtc_rtcp_report_due_at(&stats, 128000, 100, stats.next_report_time, 0), RTC_OK);
    uint8_t wire[128];
    size_t length;
    ASSERT_EQ(rtc_rtcp_build_bye(wire, sizeof(wire), &length, 0x12345678), RTC_OK);
    ASSERT_EQ(length, 8);
    ASSERT_EQ(rtc_rtcp_compound_feedback(wire, &length, sizeof(wire)), RTC_OK);
    ASSERT_EQ(wire[length - 7], RTCP_PT_BYE);
    ASSERT_EQ(wire[length - 4], 0x12);
    ASSERT_EQ(wire[length - 1], 0x78);
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

TEST(rtcp_membership_activity_bye_and_atomic_sdes) {
    rtc_rtcp_membership_t group = {0};
    ASSERT_EQ(rtc_rtcp_membership_touch(&group, 11, true, true, 1000), RTC_OK);
    ASSERT_EQ(rtc_rtcp_membership_touch(&group, 22, false, true, 1000), RTC_OK);
    rtc_rtcp_stats_t source;
    rtc_rtcp_stats_init(&source, 33);
    source.remote_ssrc = 99;
    source.first_packet = false;
    rtc_rtcp_packet_t packet;
    ASSERT_EQ(rtc_rtcp_build_report(&packet, &source, false), RTC_OK);
    ASSERT_EQ(rtc_rtcp_membership_on_rtcp(&group, packet.buf, packet.buf_len, false,
                                          packet.buf_len + 68, 2000),
              RTC_OK);
    ASSERT_EQ(group.members, 3);
    ASSERT_EQ(group.senders, 2);
    rtc_rtcp_membership_t before = group;
    packet.buf[packet.buf_len - 1] = 1;
    ASSERT_EQ(rtc_rtcp_membership_on_rtcp(&group, packet.buf, packet.buf_len, false,
                                          packet.buf_len + 68, 3000),
              RTC_ERR_INVALID);
    ASSERT_MEM_EQ(&group, &before, sizeof(group));
    uint8_t bye[8];
    size_t len;
    ASSERT_EQ(rtc_rtcp_build_bye(bye, sizeof(bye), &len, 22), RTC_OK);
    ASSERT_EQ(rtc_rtcp_membership_on_rtcp(&group, bye, len, false, len + 68, 4000), RTC_OK);
    ASSERT_EQ(group.members, 2);
    ASSERT_EQ(group.senders, 1);
    ASSERT(group.average_size > 0 && group.average_size < before.average_size);
    ASSERT_EQ(rtc_rtcp_membership_on_rtcp(&group, bye, len, false, len + 68, 4001), RTC_OK);
    ASSERT_EQ(group.members, 2);

    ASSERT_EQ(rtc_rtcp_membership_touch(&group, 33, true, false, 4002), RTC_OK);
    ASSERT_EQ(group.members, 3);
    rtc_rtcp_membership_expire(&group, 500000, 11000);
    ASSERT_EQ(group.senders, 0);
    rtc_rtcp_membership_expire(&group, 500000, 26000);
    ASSERT_EQ(group.members, 2);
    ASSERT_EQ(rtc_rtcp_membership_touch(&group, 33, false, true, 26001), RTC_OK);
    ASSERT_EQ(group.senders, 1);
    memset(&group, 0, sizeof(group));
    for (unsigned i = 0; i < 16; i++)
        ASSERT_EQ(rtc_rtcp_membership_touch(&group, i, i == 0, i == 0, 1), RTC_OK);
    group.average_size = 1000;
    rtc_rtcp_stats_init(&source, 0);
    rtc_rtcp_stats_set_membership(&source, &group, 1);
    ASSERT(source.report_we_sent);
    rtc_rtcp_membership_expire(&group, 8000, 160000);
    ASSERT_EQ(group.senders, 1);
    rtc_rtcp_membership_expire(&group, 8000, 160001);
    ASSERT_EQ(group.senders, 0);
    ASSERT_EQ(group.members, 16);
    rtc_rtcp_stats_set_membership(&source, &group, 160001);
    ASSERT(!source.report_we_sent);
}

TEST(rtcp_membership_reconsideration_and_capacity) {
    rtc_rtcp_membership_t group = {0};
    for (unsigned i = 1; i <= 4; i++)
        ASSERT_EQ(rtc_rtcp_membership_touch(&group, i, i == 1, false, 10000), RTC_OK);
    group.average_size = 1000;
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 1);
    rtc_rtcp_stats_set_membership(&stats, &group, 10000);
    ASSERT_EQ(rtc_rtcp_report_due_at(&stats, 8000, 1000, 10000, 0), RTC_ERR_TIMEOUT);
    uint64_t original = stats.next_report_time;
    for (unsigned i = 5; i <= 8; i++)
        ASSERT_EQ(rtc_rtcp_membership_touch(&group, i, false, false, 11000), RTC_OK);
    rtc_rtcp_stats_set_membership(&stats, &group, 11000);
    ASSERT_EQ(stats.next_report_time, original);
    ASSERT_EQ(rtc_rtcp_report_due_at(&stats, 8000, 1000, original, 0), RTC_ERR_TIMEOUT);
    uint64_t grown = stats.next_report_time;
    ASSERT(grown > original);
    for (unsigned i = 3; i <= 8; i++) {
        uint8_t bye[8];
        size_t len;
        ASSERT_EQ(rtc_rtcp_build_bye(bye, sizeof(bye), &len, i), RTC_OK);
        ASSERT_EQ(rtc_rtcp_membership_on_rtcp(&group, bye, len, false, 1000, original), RTC_OK);
    }
    rtc_rtcp_stats_set_membership(&stats, &group, original);
    ASSERT_EQ(stats.next_report_time, (uint64_t)(original + 0.25 * (grown - original)));
    ASSERT(stats.report_previous_ms > 10000);
    ASSERT_EQ(stats.report_pmembers, 2);
    stats.next_report_time = 50000;
    stats.report_pmembers = 4;
    stats.report_previous_ms = 40000;
    rtc_rtcp_stats_set_membership(&stats, &group, 60000);
    ASSERT_EQ(stats.next_report_time, 55000);
    ASSERT_EQ((uint64_t)stats.report_previous_ms, 50000);
    ASSERT_EQ(rtc_rtcp_report_due_at(&stats, 8000, 1000, 1000000, 0), RTC_OK);

    memset(&group, 0, sizeof(group));
    for (unsigned i = 0; i < RTC_RTCP_MAX_MEMBERS; i++)
        ASSERT_EQ(rtc_rtcp_membership_touch(&group, i, false, true, 1), RTC_OK);
    ASSERT(rtc_rtcp_membership_can_add(&group, 0, false));
    ASSERT(!rtc_rtcp_membership_can_add(&group, RTC_RTCP_MAX_MEMBERS, false));
    ASSERT_EQ(rtc_rtcp_membership_touch(&group, RTC_RTCP_MAX_MEMBERS, false, true, 2),
              RTC_ERR_NOMEM);
    rtc_rtcp_membership_t before = group;
    rtc_rtcp_packet_t report;
    rtc_rtcp_stats_init(&stats, RTC_RTCP_MAX_MEMBERS);
    ASSERT_EQ(rtc_rtcp_build_report(&report, &stats, false), RTC_OK);
    ASSERT_EQ(rtc_rtcp_membership_on_rtcp(&group, report.buf, report.buf_len, false,
                                          report.buf_len + 68, 2),
              RTC_ERR_NOMEM);
    ASSERT_MEM_EQ(&group, &before, sizeof(group));
    rtc_rtcp_membership_expire(&group, 500000, 25001);
    ASSERT_EQ(group.members, RTC_RTCP_MAX_MEMBERS);
    rtc_rtcp_membership_expire(&group, 500000, 34001);
    ASSERT_EQ(group.members, RTC_RTCP_MAX_MEMBERS);
    rtc_rtcp_membership_expire(&group, 500000, 35001);
    ASSERT_EQ(group.members, 0);
    ASSERT_EQ(group.senders, 0);
}

TEST(rtcp_sender_clock_idle_reordering_and_wrap) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 11);
    stats.send_clock_rate = 90000;
    uint32_t initial = UINT32_MAX - 44999;
    rtc_rtcp_stats_on_rtp_send_at(&stats, initial, 10, 1000);
    rtc_rtcp_stats_on_rtp_send_at(&stats, initial - 3000, 10, 1500);
    rtc_rtcp_stats_on_rtp_send_at(&stats, initial, 10, 1700);
    stats.remote_ssrc = 22;
    stats.first_packet = false;
    stats.last_sr_recv_time = 1500;
    rtc_rtcp_packet_t packet, parsed;
    ASSERT_EQ(rtc_rtcp_build_sr_at(&packet, &stats, 2000, 1700000000500ULL), RTC_OK);
    ASSERT_EQ(rtc_rtcp_parse(&parsed, packet.buf, packet.buf_len), RTC_OK);
    ASSERT_EQ(parsed.sr.rtp_timestamp, 45000);
    ASSERT_EQ(parsed.sr.ntp_sec, 3908988800u);
    ASSERT_EQ(parsed.sr.ntp_frac, 0x80000000u);
    ASSERT_EQ(parsed.sr.sender_pkt_count, 3);
    ASSERT_EQ(parsed.reports[0].delay_since_sr, 32768);
    ASSERT_EQ(stats.rtp_timestamp, initial);
    ASSERT_EQ(rtc_rtcp_build_sr_at(&packet, &stats, 11000, 1700000009500ULL), RTC_OK);
    ASSERT_EQ(packet.sr.rtp_timestamp, (uint32_t)(initial + 900000u));
    rtc_rtcp_stats_on_rtp_send_at(&stats, 45000, 10, 2000);
    ASSERT_EQ(rtc_rtcp_stats_reset_source(&stats), RTC_OK);
    ASSERT_EQ(rtc_rtcp_build_sr_at(&packet, &stats, 2500, 1700000001000ULL), RTC_OK);
    ASSERT_EQ(packet.sr.rtp_timestamp, 90000);
    ASSERT_EQ(packet.sr.sender_pkt_count, 4);
    ASSERT_EQ(packet.report_count, 0);
    ASSERT_EQ(rtc_rtcp_build_sr_at(&packet, &stats, 1999, 1700000000499ULL), RTC_OK);
    ASSERT_EQ(packet.sr.rtp_timestamp, 45000);
}

TEST(rtcp_forwarded_clock_and_monotonic_rtt_history) {
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 99);
    stats.send_clock_rate = 90000;
    stats.source_clock_valid = true;
    stats.source_clock_ms = 1000;
    stats.source_clock = (rtc_rtcp_sr_t){.ssrc = 11,
                                         .ntp_sec = UINT32_MAX - 1,
                                         .ntp_frac = 0x80000000u,
                                         .rtp_timestamp = UINT32_MAX - 44999};
    memcpy(stats.cname, "publisher-sync", sizeof("publisher-sync"));
    rtc_rtcp_stats_on_rtp_send_at(&stats, 123, 10, 2900);
    rtc_rtcp_packet_t packet, parsed;
    ASSERT_EQ(rtc_rtcp_build_sr_at(&packet, &stats, 3000, 1700000000000ULL), RTC_OK);
    ASSERT_EQ(rtc_rtcp_parse(&parsed, packet.buf, packet.buf_len), RTC_OK);
    ASSERT_EQ(parsed.sr.ssrc, 99);
    ASSERT_EQ(parsed.sr.rtp_timestamp, 135000);
    ASSERT_EQ(parsed.sr.ntp_sec, 0);
    ASSERT_EQ(parsed.sr.ntp_frac, 0x80000000u);
    ASSERT_EQ(parsed.sr.sender_pkt_count, 1);
    rtc_rtcp_stats_record_sr(&stats, &parsed.sr, 3000);
    rtc_rtcp_rr_block_t rr = {.last_sr = 0x8000, .delay_since_sr = 8192};
    ASSERT_EQ(rtc_rtcp_stats_rtt_from_rr(&stats, &rr, 3375), 250);
    ASSERT_EQ(rtc_rtcp_stats_rtt_from_rr(&stats, &rr, 3100), 0);
    ASSERT_EQ(rtc_rtcp_stats_rtt_from_rr(&stats, &rr, 2999), 0);
    ASSERT_EQ(rtc_rtcp_stats_reset_source(&stats), RTC_OK);
    ASSERT_EQ(rtc_rtcp_stats_rtt_from_rr(&stats, &rr, 3375), 250);
    ASSERT_EQ(rtc_rtcp_build_report(&packet, &stats, true), RTC_OK);
    ASSERT_EQ(packet.buf[37], strlen(stats.cname));
    ASSERT_MEM_EQ(packet.buf + 38, stats.cname, strlen(stats.cname));
    for (unsigned i = 0; i < RTC_RTCP_SR_HISTORY; i++) {
        rtc_rtcp_sr_t newer = {.ntp_sec = 10 + i};
        rtc_rtcp_stats_record_sr(&stats, &newer, 4000 + i);
    }
    ASSERT_EQ(rtc_rtcp_stats_rtt_from_rr(&stats, &rr, 5000), 0);
    uint8_t bye[128];
    size_t len;
    ASSERT_EQ(rtc_rtcp_build_bye(bye, sizeof(bye), &len, 99), RTC_OK);
    ASSERT_EQ(rtc_rtcp_compound_feedback_named(bye, &len, sizeof(bye), stats.cname), RTC_OK);
    ASSERT_EQ(bye[17], strlen(stats.cname));
    ASSERT_MEM_EQ(bye + 18, stats.cname, strlen(stats.cname));
    stats.send_clock_rate = 0;
    ASSERT_EQ(rtc_rtcp_build_sr_at(&packet, &stats, 3000, 0), RTC_ERR_INVALID);
}

int main(void) {
    printf("========================================\n");
    printf("  RTCP Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(rtcp_build_sr);
    RUN_TEST(rtcp_bye_lists_reasons_padding_and_atomic_validation);
    RUN_TEST(rtcp_randomized_bandwidth_intervals_and_bye);
    RUN_TEST(rtcp_remb_bounds_and_quantization);
    RUN_TEST(rtcp_feedback_is_compound_without_reduced_size);
    RUN_TEST(rtcp_compound_cname_and_report_boundaries);
    RUN_TEST(rtcp_interval_loss_and_late_rollover);
    RUN_TEST(rtcp_membership_activity_bye_and_atomic_sdes);
    RUN_TEST(rtcp_membership_reconsideration_and_capacity);
    RUN_TEST(rtcp_sender_clock_idle_reordering_and_wrap);
    RUN_TEST(rtcp_forwarded_clock_and_monotonic_rtt_history);
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
