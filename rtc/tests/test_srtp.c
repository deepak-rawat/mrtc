/*
 * SRTP component tests.
 *
 * Tests:
 *   1. Init SRTP context with known keys, verify derived session keys
 *   2. Protect a packet: verify payload is encrypted, tag is appended
 *   3. Protect → unprotect round-trip: verify payload restored
 *   4. Unprotect with wrong key: verify authentication failure
 *   5. Multiple packets: verify sequence-dependent encryption
 *   6. Separate send/recv contexts (simulating two peers)
 */
#include <rtc/rtc.h>
#include "rtc/rtc_rtp.h"
#include "rtc_srtp.h"
#include "rtc/rtc_rtcp.h"
#include "test_harness.h"

/* Fixed test keys (deterministic) */
static const uint8_t test_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
static const uint8_t test_salt[14] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
                                      0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D};

/* AEAD_AES_128_GCM uses a 12-byte salt (RFC 7714 §11). */
static const uint8_t test_gcm_salt[12] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
                                          0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B};

TEST(srtp_init) {
    rtc_srtp_ctx_t ctx;
    int rc = rtc_srtp_init(&ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(ctx.initialized);

    /* Session key should be derived (not all zeros) */
    bool key_nonzero = false;
    for (int i = 0; i < 16; i++) {
        if (ctx.session_key[i] != 0) {
            key_nonzero = true;
            break;
        }
    }
    ASSERT(key_nonzero);

    printf("    session_key[0..3]: %02X %02X %02X %02X\n", ctx.session_key[0], ctx.session_key[1],
           ctx.session_key[2], ctx.session_key[3]);

    rtc_srtp_close(&ctx);
}

TEST(srtp_protect_encrypts) {
    rtc_srtp_ctx_t ctx;
    rtc_srtp_init(&ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    /* Build a known RTP packet */
    uint8_t payload[100];
    memset(payload, 0x42, sizeof(payload));

    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 111, 1, 960, 0x12345678, false, payload, sizeof(payload));

    /* Save original for comparison */
    uint8_t original[1500];
    memcpy(original, pkt.buf, pkt.buf_len);
    size_t original_len = pkt.buf_len;

    /* Protect in-place */
    size_t srtp_len = pkt.buf_len;
    int rc = rtc_srtp_protect(&ctx, pkt.buf, &srtp_len, sizeof(pkt.buf));
    ASSERT_EQ(rc, RTC_OK);

    /* Length should grow by auth tag (10 bytes) */
    ASSERT_EQ(srtp_len, original_len + SRTP_AUTH_TAG_LEN);

    /* Header (12 bytes) should be unchanged */
    ASSERT_MEM_EQ(pkt.buf, original, RTP_HEADER_SIZE);

    /* Payload should be different (encrypted) */
    bool encrypted = false;
    for (size_t i = RTP_HEADER_SIZE; i < original_len; i++) {
        if (pkt.buf[i] != original[i]) {
            encrypted = true;
            break;
        }
    }
    ASSERT(encrypted);

    printf("    plain=%zu bytes -> srtp=%zu bytes (+%d tag), payload encrypted\n", original_len,
           srtp_len, SRTP_AUTH_TAG_LEN);

    rtc_srtp_close(&ctx);
}

TEST(srtp_roundtrip) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    rtc_srtp_init(&recv_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    /* Build RTP packet with recognizable payload */
    uint8_t payload[] = "SRTP round-trip test payload!";
    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 111, 1, 0, 0xAABBCCDD, false, payload, sizeof(payload) - 1);

    /* Save original payload for comparison */
    uint8_t original_payload[100];
    memcpy(original_payload, payload, sizeof(payload) - 1);

    /* Protect */
    uint8_t buf[1500];
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;

    int rc = rtc_srtp_protect(&send_ctx, buf, &len, sizeof(buf));
    ASSERT_EQ(rc, RTC_OK);

    /* Unprotect */
    rc = rtc_srtp_unprotect(&recv_ctx, buf, &len);
    ASSERT_EQ(rc, RTC_OK);

    /* Length should be back to original */
    ASSERT_EQ(len, pkt.buf_len);

    /* Payload should match original */
    ASSERT_MEM_EQ(buf + RTP_HEADER_SIZE, original_payload, sizeof(payload) - 1);

    printf("    protect -> unprotect: payload restored (%zu bytes)\n", sizeof(payload) - 1);

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

TEST(srtp_wrong_key_fails) {
    rtc_srtp_ctx_t send_ctx, bad_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    uint8_t bad_key[16] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                           0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
    rtc_srtp_init(&bad_ctx, bad_key, sizeof(bad_key), test_salt, sizeof(test_salt));

    /* Build and protect with correct key */
    uint8_t payload[50];
    memset(payload, 0x55, sizeof(payload));
    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 111, 1, 0, 0x11111111, false, payload, sizeof(payload));

    uint8_t buf[1500];
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;
    rtc_srtp_protect(&send_ctx, buf, &len, sizeof(buf));

    /* Try to unprotect with wrong key */
    int rc = rtc_srtp_unprotect(&bad_ctx, buf, &len);
    ASSERT(rc != RTC_OK);

    printf("    unprotect with wrong key: correctly rejected\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&bad_ctx);
}

TEST(srtp_multiple_packets) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    rtc_srtp_init(&recv_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    for (int i = 0; i < 10; i++) {
        char payload[64];
        int plen = snprintf(payload, sizeof(payload), "Packet number %d", i);

        rtc_rtp_packet_t pkt;
        rtc_rtp_build(&pkt, 111, (uint16_t)i, (uint32_t)(i * 960), 0xDEAD0000, false,
                      (const uint8_t *)payload, (size_t)plen);

        uint8_t buf[1500];
        memcpy(buf, pkt.buf, pkt.buf_len);
        size_t len = pkt.buf_len;

        int rc = rtc_srtp_protect(&send_ctx, buf, &len, sizeof(buf));
        ASSERT_EQ(rc, RTC_OK);

        rc = rtc_srtp_unprotect(&recv_ctx, buf, &len);
        ASSERT_EQ(rc, RTC_OK);

        /* Verify decrypted payload */
        ASSERT_EQ(len, pkt.buf_len);
        ASSERT_MEM_EQ(buf + RTP_HEADER_SIZE, payload, (size_t)plen);
    }

    printf("    10 packets: protect -> unprotect all successful\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

/* Two SSRCs bundled on one SRTP context, with sequence numbers that straddle
 * the 0x8000 wrap boundary. A single shared rollover counter would spuriously
 * roll over when the streams interleave (and fail once the receive order
 * differs from the send order); per-SSRC ROC keeps each stream at ROC 0. */
TEST(srtp_per_ssrc_rollover) {
    enum { N = 6 };
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    rtc_srtp_init(&recv_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    const uint32_t ssrc_a = 0x0A0A0A0A;
    const uint32_t ssrc_b = 0x0B0B0B0B;
    const uint16_t base_a = 100;
    const uint16_t base_b = 50000; /* base_b - base_a > 0x8000 */

    uint8_t pkts[2 * N][1500];
    size_t lens[2 * N];
    char payloads[2 * N][32];

    /* Protect in interleaved send order: A,B,A,B,... */
    for (int i = 0; i < N; i++) {
        for (int s = 0; s < 2; s++) {
            int idx = i * 2 + s;
            uint32_t ssrc = s == 0 ? ssrc_a : ssrc_b;
            uint16_t seq = (uint16_t)((s == 0 ? base_a : base_b) + i);
            int plen =
                snprintf(payloads[idx], sizeof(payloads[idx]), "%c-%d", s == 0 ? 'A' : 'B', i);
            rtc_rtp_packet_t pkt;
            rtc_rtp_build(&pkt, 111, seq, (uint32_t)(i * 960), ssrc, false,
                          (const uint8_t *)payloads[idx], (size_t)plen);
            memcpy(pkts[idx], pkt.buf, pkt.buf_len);
            lens[idx] = pkt.buf_len;
            int rc = rtc_srtp_protect(&send_ctx, pkts[idx], &lens[idx], sizeof(pkts[idx]));
            ASSERT_EQ(rc, RTC_OK);
        }
    }

    /* Unprotect grouped by stream (all A, then all B) — a different order than
     * sent. A shared ROC mis-estimates and fails here; per-SSRC succeeds. */
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < N; i++) {
            int idx = i * 2 + s;
            size_t len = lens[idx];
            int rc = rtc_srtp_unprotect(&recv_ctx, pkts[idx], &len);
            ASSERT_EQ(rc, RTC_OK);
            size_t plen = strlen(payloads[idx]);
            ASSERT_EQ(len, RTP_HEADER_SIZE + plen);
            ASSERT_MEM_EQ(pkts[idx] + RTP_HEADER_SIZE, payloads[idx], plen);
        }
    }

    printf("    per-SSRC rollover: 2 bundled SSRCs reordered, all decrypt\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

TEST(srtp_gcm_roundtrip) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    ASSERT_EQ(rtc_srtp_init_profile(&send_ctx, RTC_SRTP_PROFILE_AEAD_AES_128_GCM, test_key,
                                    sizeof(test_key), test_gcm_salt, sizeof(test_gcm_salt)),
              RTC_OK);
    ASSERT_EQ(rtc_srtp_init_profile(&recv_ctx, RTC_SRTP_PROFILE_AEAD_AES_128_GCM, test_key,
                                    sizeof(test_key), test_gcm_salt, sizeof(test_gcm_salt)),
              RTC_OK);

    const uint8_t payload[] = "AEAD_AES_128_GCM round-trip payload!";
    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 96, 5, 1000, 0xABCDEF01, false, payload, sizeof(payload) - 1);

    uint8_t buf[1500];
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;

    ASSERT_EQ(rtc_srtp_protect(&send_ctx, buf, &len, sizeof(buf)), RTC_OK);
    /* GCM appends a 16-byte tag and leaves the header in the clear. */
    ASSERT_EQ(len, pkt.buf_len + SRTP_GCM_TAG_LEN);
    ASSERT_MEM_EQ(buf, pkt.buf, RTP_HEADER_SIZE);
    bool encrypted = false;
    for (size_t i = RTP_HEADER_SIZE; i < pkt.buf_len; i++) {
        if (buf[i] != pkt.buf[i]) {
            encrypted = true;
            break;
        }
    }
    ASSERT(encrypted);

    ASSERT_EQ(rtc_srtp_unprotect(&recv_ctx, buf, &len), RTC_OK);
    ASSERT_EQ(len, pkt.buf_len);
    ASSERT_MEM_EQ(buf + RTP_HEADER_SIZE, payload, sizeof(payload) - 1);

    printf("    GCM SRTP round-trip: payload restored, 16-byte tag\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

TEST(srtp_gcm_wrong_key_fails) {
    rtc_srtp_ctx_t send_ctx, bad_ctx;
    rtc_srtp_init_profile(&send_ctx, RTC_SRTP_PROFILE_AEAD_AES_128_GCM, test_key, sizeof(test_key),
                          test_gcm_salt, sizeof(test_gcm_salt));
    uint8_t bad_key[16] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                           0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
    rtc_srtp_init_profile(&bad_ctx, RTC_SRTP_PROFILE_AEAD_AES_128_GCM, bad_key, sizeof(bad_key),
                          test_gcm_salt, sizeof(test_gcm_salt));

    uint8_t payload[40];
    memset(payload, 0x5A, sizeof(payload));
    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 96, 9, 0, 0x22223333, false, payload, sizeof(payload));
    uint8_t buf[1500];
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;
    ASSERT_EQ(rtc_srtp_protect(&send_ctx, buf, &len, sizeof(buf)), RTC_OK);

    ASSERT(rtc_srtp_unprotect(&bad_ctx, buf, &len) != RTC_OK);

    printf("    GCM SRTP wrong key: correctly rejected\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&bad_ctx);
}

TEST(srtcp_gcm_roundtrip) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init_profile(&send_ctx, RTC_SRTP_PROFILE_AEAD_AES_128_GCM, test_key, sizeof(test_key),
                          test_gcm_salt, sizeof(test_gcm_salt));
    rtc_srtp_init_profile(&recv_ctx, RTC_SRTP_PROFILE_AEAD_AES_128_GCM, test_key, sizeof(test_key),
                          test_gcm_salt, sizeof(test_gcm_salt));

    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0xDEADBEEF);
    for (int i = 0; i < 20; i++)
        rtc_rtcp_stats_on_rtp_send(&stats, (uint32_t)(i * 960), 160);

    rtc_rtcp_packet_t rtcp_pkt;
    ASSERT_EQ(rtc_rtcp_build_sr(&rtcp_pkt, &stats), RTC_OK);

    uint8_t original[1500];
    memcpy(original, rtcp_pkt.buf, rtcp_pkt.buf_len);
    size_t original_len = rtcp_pkt.buf_len;

    uint8_t buf[1500];
    memcpy(buf, rtcp_pkt.buf, rtcp_pkt.buf_len);
    size_t len = rtcp_pkt.buf_len;
    ASSERT_EQ(rtc_srtp_protect_rtcp(&send_ctx, buf, &len, sizeof(buf)), RTC_OK);
    /* GCM SRTCP adds a 16-byte tag + 4-byte SRTCP index. */
    ASSERT_EQ(len, original_len + SRTP_GCM_TAG_LEN + 4);

    ASSERT_EQ(rtc_srtp_unprotect_rtcp(&recv_ctx, buf, &len), RTC_OK);
    ASSERT_EQ(len, original_len);
    ASSERT_MEM_EQ(buf, original, original_len);

    printf("    GCM SRTCP round-trip: %zu -> %zu -> %zu bytes\n", original_len, original_len + 20,
           len);

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

TEST(srtcp_roundtrip) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    rtc_srtp_init(&recv_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    /* Build a valid RTCP Sender Report using rtc_rtcp */
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0xDEADBEEF);
    for (int i = 0; i < 50; i++)
        rtc_rtcp_stats_on_rtp_send(&stats, (uint32_t)(i * 960), 160);

    rtc_rtcp_packet_t rtcp_pkt;
    int rc = rtc_rtcp_build_sr(&rtcp_pkt, &stats);
    ASSERT_EQ(rc, RTC_OK);

    /* Save original for comparison */
    uint8_t original[1500];
    memcpy(original, rtcp_pkt.buf, rtcp_pkt.buf_len);
    size_t original_len = rtcp_pkt.buf_len;

    /* Protect */
    uint8_t buf[1500];
    memcpy(buf, rtcp_pkt.buf, rtcp_pkt.buf_len);
    size_t len = rtcp_pkt.buf_len;
    rc = rtc_srtp_protect_rtcp(&send_ctx, buf, &len, sizeof(buf));
    ASSERT_EQ(rc, RTC_OK);

    /* Should grow by 4 (SRTCP index) + 10 (auth tag) = 14 bytes */
    ASSERT_EQ(len, original_len + 4 + SRTP_AUTH_TAG_LEN);

    /* Unprotect */
    rc = rtc_srtp_unprotect_rtcp(&recv_ctx, buf, &len);
    ASSERT_EQ(rc, RTC_OK);

    /* Should be back to original size */
    ASSERT_EQ(len, original_len);

    /* Contents should match original */
    ASSERT_MEM_EQ(buf, original, original_len);

    printf("    SRTCP round-trip: %zu -> %zu -> %zu bytes\n", original_len,
           original_len + 4 + SRTP_AUTH_TAG_LEN, len);

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

TEST(srtcp_wrong_key_fails) {
    rtc_srtp_ctx_t send_ctx, bad_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    uint8_t bad_key[16] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                           0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
    rtc_srtp_init(&bad_ctx, bad_key, sizeof(bad_key), test_salt, sizeof(test_salt));

    /* Build RTCP RR */
    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0x11111111);
    for (uint16_t i = 0; i < 10; i++)
        rtc_rtcp_stats_on_rtp_recv(&stats, i, i * 960, 0x22222222, 48000);

    rtc_rtcp_packet_t rtcp_pkt;
    rtc_rtcp_build_rr(&rtcp_pkt, &stats);

    uint8_t buf[1500];
    memcpy(buf, rtcp_pkt.buf, rtcp_pkt.buf_len);
    size_t len = rtcp_pkt.buf_len;
    rtc_srtp_protect_rtcp(&send_ctx, buf, &len, sizeof(buf));

    /* Try to unprotect with wrong key */
    int rc = rtc_srtp_unprotect_rtcp(&bad_ctx, buf, &len);
    ASSERT(rc != RTC_OK);

    printf("    SRTCP unprotect with wrong key: correctly rejected\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&bad_ctx);
}

TEST(srtp_replay_rejected) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    rtc_srtp_init(&recv_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    /* Build, protect once */
    uint8_t payload[] = "replay me";
    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 111, 100, 0, 0xCAFEBABE, false, payload, sizeof(payload) - 1);

    uint8_t buf[1500];
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;
    int rc = rtc_srtp_protect(&send_ctx, buf, &len, sizeof(buf));
    ASSERT_EQ(rc, RTC_OK);

    /* Keep a copy of the SRTP-protected packet for replay */
    uint8_t replay_buf[1500];
    memcpy(replay_buf, buf, len);
    size_t replay_len = len;

    /* First unprotect succeeds */
    rc = rtc_srtp_unprotect(&recv_ctx, buf, &len);
    ASSERT_EQ(rc, RTC_OK);

    /* Replay the SAME bytes — should be rejected by replay window */
    rc = rtc_srtp_unprotect(&recv_ctx, replay_buf, &replay_len);
    ASSERT(rc != RTC_OK);

    printf("    SRTP replay correctly rejected\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

TEST(srtcp_replay_rejected) {
    rtc_srtp_ctx_t send_ctx, recv_ctx;
    rtc_srtp_init(&send_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));
    rtc_srtp_init(&recv_ctx, test_key, sizeof(test_key), test_salt, sizeof(test_salt));

    rtc_rtcp_stats_t stats;
    rtc_rtcp_stats_init(&stats, 0xFEEDFACE);
    for (int i = 0; i < 5; i++)
        rtc_rtcp_stats_on_rtp_send(&stats, (uint32_t)(i * 960), 100);

    rtc_rtcp_packet_t rtcp_pkt;
    int rc = rtc_rtcp_build_sr(&rtcp_pkt, &stats);
    ASSERT_EQ(rc, RTC_OK);

    uint8_t buf[1500];
    memcpy(buf, rtcp_pkt.buf, rtcp_pkt.buf_len);
    size_t len = rtcp_pkt.buf_len;
    rc = rtc_srtp_protect_rtcp(&send_ctx, buf, &len, sizeof(buf));
    ASSERT_EQ(rc, RTC_OK);

    uint8_t replay_buf[1500];
    memcpy(replay_buf, buf, len);
    size_t replay_len = len;

    rc = rtc_srtp_unprotect_rtcp(&recv_ctx, buf, &len);
    ASSERT_EQ(rc, RTC_OK);

    rc = rtc_srtp_unprotect_rtcp(&recv_ctx, replay_buf, &replay_len);
    ASSERT(rc != RTC_OK);

    printf("    SRTCP replay correctly rejected\n");

    rtc_srtp_close(&send_ctx);
    rtc_srtp_close(&recv_ctx);
}

int main(void) {
    printf("========================================\n");
    printf("  SRTP Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(srtp_init);
    RUN_TEST(srtp_protect_encrypts);
    RUN_TEST(srtp_roundtrip);
    RUN_TEST(srtp_wrong_key_fails);
    RUN_TEST(srtp_multiple_packets);
    RUN_TEST(srtp_per_ssrc_rollover);
    RUN_TEST(srtp_gcm_roundtrip);
    RUN_TEST(srtp_gcm_wrong_key_fails);
    RUN_TEST(srtcp_gcm_roundtrip);
    RUN_TEST(srtcp_roundtrip);
    RUN_TEST(srtcp_wrong_key_fails);
    RUN_TEST(srtp_replay_rejected);
    RUN_TEST(srtcp_replay_rejected);

    rtc_cleanup();
    TEST_SUMMARY();
}
