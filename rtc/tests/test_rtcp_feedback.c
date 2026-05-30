/*
 * test_rtcp_feedback.c - RTCP feedback packet tests (NACK, PLI, FIR).
 *
 * Tests build/parse round-trips, edge cases, and wire format compliance
 * for all four feedback packet types added in Phase 2.
 */
#include <rtc/rtc.h>
#include "rtc_rtcp.h"
#include "test_harness.h"

#include <string.h>

/* ================================================================== */
/*  NACK Tests                                                         */
/* ================================================================== */

TEST(nack_build_single) {
    uint8_t buf[256];
    size_t len;
    uint16_t lost[] = {42};

    int rc = rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0x11111111, 0x22222222, lost, 1);
    ASSERT_EQ(rc, RTC_OK);
    /* header(4) + sender_ssrc(4) + media_ssrc(4) + 1 FCI(4) = 16 */
    ASSERT_EQ((int)len, 16);
    /* Verify header: V=2, FMT=1, PT=205 */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);
    ASSERT_EQ(buf[0] & 0x1F, RTCP_FMT_NACK);
    ASSERT_EQ(buf[1], RTCP_PT_RTPFB);
    printf("    NACK single: %zu bytes\n", len);
}

TEST(nack_build_consecutive) {
    uint8_t buf[256];
    size_t len;
    /* 5 consecutive: 100, 101, 102, 103, 104 → 1 FCI (pid=100, blp=0x000F) */
    uint16_t lost[] = {100, 101, 102, 103, 104};

    int rc = rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xAAAA, 0xBBBB, lost, 5);
    ASSERT_EQ(rc, RTC_OK);
    /* Should coalesce into 1 FCI: header(4) + ssrcs(8) + 1 FCI(4) = 16 */
    ASSERT_EQ((int)len, 16);
    printf("    NACK consecutive: %zu bytes (coalesced to 1 FCI)\n", len);
}

TEST(nack_build_sparse) {
    uint8_t buf[256];
    size_t len;
    /* Two non-adjacent groups: {10} and {50} → 2 FCIs */
    uint16_t lost[] = {10, 50};

    int rc = rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xAAAA, 0xBBBB, lost, 2);
    ASSERT_EQ(rc, RTC_OK);
    /* 2 FCIs: header(4) + ssrcs(8) + 2*FCI(8) = 20 */
    ASSERT_EQ((int)len, 20);
    printf("    NACK sparse: %zu bytes (2 FCIs)\n", len);
}

TEST(nack_build_max) {
    uint8_t buf[4096];
    size_t len;
    /* Create 64 non-adjacent seqs to get 64 FCIs */
    uint16_t lost[64];
    for (int i = 0; i < 64; i++)
        lost[i] = (uint16_t)(i * 20);

    int rc = rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xAAAA, 0xBBBB, lost, 64);
    ASSERT_EQ(rc, RTC_OK);
    /* 64 FCIs: header(4) + ssrcs(8) + 64*FCI(256) = 268 */
    ASSERT_EQ((int)len, 268);
    printf("    NACK max: %zu bytes (64 FCIs)\n", len);
}

TEST(nack_roundtrip) {
    uint8_t buf[4096];
    size_t len;
    uint16_t lost[] = {100, 101, 102, 200, 201, 300};

    int rc = rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xAAAA, 0xBBBB, lost, 6);
    ASSERT_EQ(rc, RTC_OK);

    rtc_rtcp_nack_t parsed;
    rc = rtc_rtcp_parse_nack(&parsed, buf, len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(parsed.sender_ssrc, 0xAAAA);
    ASSERT_EQ(parsed.media_ssrc, 0xBBBB);
    ASSERT_EQ(parsed.lost_count, 6);

    /* Verify all sequences present (order from parse matches sorted order) */
    ASSERT_EQ(parsed.lost_seqs[0], 100);
    ASSERT_EQ(parsed.lost_seqs[1], 101);
    ASSERT_EQ(parsed.lost_seqs[2], 102);
    ASSERT_EQ(parsed.lost_seqs[3], 200);
    ASSERT_EQ(parsed.lost_seqs[4], 201);
    ASSERT_EQ(parsed.lost_seqs[5], 300);

    printf("    NACK roundtrip: %d lost seqs recovered\n", parsed.lost_count);
}

TEST(nack_parse_invalid) {
    rtc_rtcp_nack_t parsed;

    /* Too short */
    uint8_t short_buf[8] = {0x80 | 1, 205, 0, 0, 0, 0, 0, 0};
    ASSERT(rtc_rtcp_parse_nack(&parsed, short_buf, sizeof(short_buf)) != RTC_OK);

    /* NULL args */
    ASSERT(rtc_rtcp_parse_nack(NULL, short_buf, sizeof(short_buf)) != RTC_OK);

    printf("    NACK parse invalid: correctly rejected\n");
}

/* ================================================================== */
/*  PLI Tests                                                          */
/* ================================================================== */

TEST(pli_build) {
    uint8_t buf[64];
    size_t len;

    int rc = rtc_rtcp_build_pli(buf, sizeof(buf), &len, 0x11111111, 0x22222222);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ((int)len, 12);
    /* Header: V=2, FMT=1, PT=206 */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);
    ASSERT_EQ(buf[0] & 0x1F, RTCP_FMT_PLI);
    ASSERT_EQ(buf[1], RTCP_PT_PSFB);
    printf("    PLI built: %zu bytes\n", len);
}

TEST(pli_roundtrip) {
    uint8_t buf[64];
    size_t len;

    int rc = rtc_rtcp_build_pli(buf, sizeof(buf), &len, 0xDEAD, 0xBEEF);
    ASSERT_EQ(rc, RTC_OK);

    rtc_rtcp_pli_t parsed;
    rc = rtc_rtcp_parse_pli(&parsed, buf, len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.sender_ssrc, 0xDEAD);
    ASSERT_EQ(parsed.media_ssrc, 0xBEEF);
    printf("    PLI roundtrip OK\n");
}

TEST(pli_parse_short) {
    rtc_rtcp_pli_t parsed;
    uint8_t short_buf[8] = {0};
    ASSERT(rtc_rtcp_parse_pli(&parsed, short_buf, sizeof(short_buf)) != RTC_OK);
    printf("    PLI short buffer: correctly rejected\n");
}

/* ================================================================== */
/*  FIR Tests                                                          */
/* ================================================================== */

TEST(fir_build) {
    uint8_t buf[64];
    size_t len;

    int rc = rtc_rtcp_build_fir(buf, sizeof(buf), &len, 0x11111111, 0x22222222, 7);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ((int)len, 20);
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);
    ASSERT_EQ(buf[0] & 0x1F, RTCP_FMT_FIR);
    ASSERT_EQ(buf[1], RTCP_PT_PSFB);
    printf("    FIR built: %zu bytes\n", len);
}

TEST(fir_roundtrip) {
    uint8_t buf[64];
    size_t len;

    int rc = rtc_rtcp_build_fir(buf, sizeof(buf), &len, 0xAAAA, 0xBBBB, 42);
    ASSERT_EQ(rc, RTC_OK);

    rtc_rtcp_fir_t parsed;
    rc = rtc_rtcp_parse_fir(&parsed, buf, len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.sender_ssrc, 0xAAAA);
    ASSERT_EQ(parsed.media_ssrc, 0xBBBB);
    ASSERT_EQ(parsed.seq_nr, 42);
    printf("    FIR roundtrip OK (seq_nr=%d)\n", parsed.seq_nr);
}

TEST(fir_parse_short) {
    rtc_rtcp_fir_t parsed;
    uint8_t short_buf[16] = {0};
    ASSERT(rtc_rtcp_parse_fir(&parsed, short_buf, sizeof(short_buf)) != RTC_OK);
    printf("    FIR short buffer: correctly rejected\n");
}

/* ================================================================== */
/*  Detection / Utility Tests                                          */
/* ================================================================== */

TEST(is_rtcp_extended) {
    /* PT 205 (RTPFB) should be recognized as RTCP */
    uint8_t rtpfb[4] = {0x80 | 1, 205, 0, 2};
    ASSERT(rtc_rtcp_is_rtcp(rtpfb, sizeof(rtpfb)));

    /* PT 206 (PSFB) should be recognized as RTCP */
    uint8_t psfb[4] = {0x80 | 1, 206, 0, 2};
    ASSERT(rtc_rtcp_is_rtcp(psfb, sizeof(psfb)));

    /* PT 96 (RTP) should NOT be recognized as RTCP */
    uint8_t rtp[4] = {0x80, 96, 0, 1};
    ASSERT(!rtc_rtcp_is_rtcp(rtp, sizeof(rtp)));

    printf("    is_rtcp extended: PT 205/206 recognized, PT 96 rejected\n");
}

TEST(get_pt_fmt) {
    uint8_t pli[12];
    size_t len;
    rtc_rtcp_build_pli(pli, sizeof(pli), &len, 0xAA, 0xBB);

    uint8_t pt, fmt;
    ASSERT(rtc_rtcp_get_pt_fmt(pli, len, &pt, &fmt));
    ASSERT_EQ(pt, RTCP_PT_PSFB);
    ASSERT_EQ(fmt, RTCP_FMT_PLI);
    printf("    get_pt_fmt: pt=%d fmt=%d\n", pt, fmt);
}

/* ================================================================== */
int main(void) {
    printf("========================================\n");
    printf("  RTCP Feedback Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    /* NACK */
    RUN_TEST(nack_build_single);
    RUN_TEST(nack_build_consecutive);
    RUN_TEST(nack_build_sparse);
    RUN_TEST(nack_build_max);
    RUN_TEST(nack_roundtrip);
    RUN_TEST(nack_parse_invalid);

    /* PLI */
    RUN_TEST(pli_build);
    RUN_TEST(pli_roundtrip);
    RUN_TEST(pli_parse_short);

    /* FIR */
    RUN_TEST(fir_build);
    RUN_TEST(fir_roundtrip);
    RUN_TEST(fir_parse_short);

    /* Utility */
    RUN_TEST(is_rtcp_extended);
    RUN_TEST(get_pt_fmt);

    rtc_cleanup();
    TEST_SUMMARY();
}
