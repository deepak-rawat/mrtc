/*
 * test_sdp.c - SDP component tests.
 *
 * Tests:
 *   1. Generate an audio offer SDP and verify structure
 *   2. Generate a data channel SDP
 *   3. Parse an SDP back and verify fields match
 *   4. Generate → parse round-trip preserves all fields
 *   5. Parse real-world-like SDP text
 *   6. SDP with multiple candidates
 */
#include <rtc/rtc.h>
#include "test_harness.h"

/* ------------------------------------------------------------------ */
/*  Test: generate audio offer SDP                                     */
/* ------------------------------------------------------------------ */
TEST(sdp_generate_audio_offer) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    sdp.type = RTC_SDP_OFFER;
    sdp.media_type = RTC_MEDIA_AUDIO;
    sdp.setup = RTC_SETUP_ACTPASS;
    sdp.payload_type = 111;
    sdp.clockrate = 48000;
    sdp.channels = 2;
    strcpy(sdp.codec_name, "opus");
    strcpy(sdp.ice_ufrag, "AbCdEfG");
    strcpy(sdp.ice_pwd, "12345678901234567890ab");
    strcpy(sdp.fingerprint, "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99");

    /* Add a candidate */
    sdp.candidate_count = 1;
    sdp.candidates[0].type = ICE_CANDIDATE_HOST;
    sdp.candidates[0].component = 1;
    sdp.candidates[0].priority = 2130706431;
    strcpy(sdp.candidates[0].foundation, "H0");
    rtc_addr_from_string(&sdp.candidates[0].addr, "192.168.1.100", 5000);

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(sdp.raw_len > 0);

    /* Verify key SDP lines are present */
    ASSERT(strstr(sdp.raw, "v=0") != NULL);
    ASSERT(strstr(sdp.raw, "m=audio") != NULL);
    ASSERT(strstr(sdp.raw, "a=ice-ufrag:AbCdEfG") != NULL);
    ASSERT(strstr(sdp.raw, "a=ice-pwd:12345678901234567890ab") != NULL);
    ASSERT(strstr(sdp.raw, "a=setup:actpass") != NULL);
    ASSERT(strstr(sdp.raw, "a=sendrecv") != NULL);
    ASSERT(strstr(sdp.raw, "a=rtcp-mux") != NULL);
    ASSERT(strstr(sdp.raw, "opus/48000/2") != NULL);
    ASSERT(strstr(sdp.raw, "a=candidate:") != NULL);
    ASSERT(strstr(sdp.raw, "192.168.1.100") != NULL);
    ASSERT(strstr(sdp.raw, "5000") != NULL);

    printf("    generated %zu bytes of SDP:\n", sdp.raw_len);
    printf("    %.60s...\n", sdp.raw);
}

/* ------------------------------------------------------------------ */
/*  Test: generate data channel SDP                                    */
/* ------------------------------------------------------------------ */
TEST(sdp_generate_data_channel) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    sdp.type = RTC_SDP_OFFER;
    sdp.media_type = RTC_MEDIA_APPLICATION;
    sdp.setup = RTC_SETUP_ACTPASS;
    strcpy(sdp.ice_ufrag, "xYzWvUt");
    strcpy(sdp.ice_pwd, "ABCDEFGHIJKLMNOPQRSTUV");
    strcpy(sdp.fingerprint, "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
                            "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF");

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT(strstr(sdp.raw, "m=application") != NULL);
    ASSERT(strstr(sdp.raw, "webrtc-datachannel") != NULL);
    ASSERT(strstr(sdp.raw, "a=sctp-port:5000") != NULL);

    printf("    data channel SDP: %zu bytes\n", sdp.raw_len);
}

/* ------------------------------------------------------------------ */
/*  Test: parse SDP text                                               */
/* ------------------------------------------------------------------ */
TEST(sdp_parse) {
    const char *sdp_text = "v=0\r\n"
                           "o=mrtc 1234567 1 IN IP4 0.0.0.0\r\n"
                           "s=mrtc\r\n"
                           "t=0 0\r\n"
                           "a=group:BUNDLE 0\r\n"
                           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:0\r\n"
                           "a=ice-ufrag:testUfr\r\n"
                           "a=ice-pwd:testPwd1234567890abc\r\n"
                           "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                           "a=setup:active\r\n"
                           "a=sendrecv\r\n"
                           "a=rtcp-mux\r\n"
                           "a=rtpmap:111 opus/48000/2\r\n"
                           "a=candidate:H0 1 udp 2130706431 10.0.0.1 5000 typ host\r\n"
                           "a=candidate:S1 1 udp 1694498815 203.0.113.5 6000 typ srflx\r\n";

    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    int rc = rtc_sdp_parse(&sdp, sdp_text, strlen(sdp_text));
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(sdp.media_type, RTC_MEDIA_AUDIO);
    ASSERT_STR_EQ(sdp.ice_ufrag, "testUfr");
    ASSERT_STR_EQ(sdp.ice_pwd, "testPwd1234567890abc");
    ASSERT_STR_EQ(sdp.fingerprint, "AA:BB:CC:DD");
    ASSERT_EQ(sdp.setup, RTC_SETUP_ACTIVE);
    ASSERT_EQ(sdp.payload_type, 111);
    ASSERT_EQ(sdp.clockrate, 48000);
    ASSERT_EQ(sdp.channels, 2);
    ASSERT_STR_EQ(sdp.codec_name, "opus");

    /* Candidates */
    ASSERT_EQ(sdp.candidate_count, 2);

    ASSERT_EQ(sdp.candidates[0].type, ICE_CANDIDATE_HOST);
    ASSERT_EQ(sdp.candidates[0].priority, 2130706431);
    char ip[64];
    uint16_t port;
    rtc_addr_to_string(&sdp.candidates[0].addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "10.0.0.1");
    ASSERT_EQ(port, 5000);

    ASSERT_EQ(sdp.candidates[1].type, ICE_CANDIDATE_SRFLX);
    rtc_addr_to_string(&sdp.candidates[1].addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "203.0.113.5");
    ASSERT_EQ(port, 6000);

    printf("    parsed: ufrag=%s codec=%s/%d/%d candidates=%d\n", sdp.ice_ufrag, sdp.codec_name,
           sdp.clockrate, sdp.channels, sdp.candidate_count);
}

/* ------------------------------------------------------------------ */
/*  Test: generate → parse round-trip                                  */
/* ------------------------------------------------------------------ */
TEST(sdp_roundtrip) {
    rtc_sdp_t orig;
    memset(&orig, 0, sizeof(orig));

    orig.type = RTC_SDP_ANSWER;
    orig.media_type = RTC_MEDIA_AUDIO;
    orig.setup = RTC_SETUP_ACTIVE;
    orig.payload_type = 96;
    orig.clockrate = 8000;
    orig.channels = 1;
    strcpy(orig.codec_name, "PCMA");
    strcpy(orig.ice_ufrag, "rndUfrg");
    strcpy(orig.ice_pwd, "rndPwd12345678901234");
    strcpy(orig.fingerprint, "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
                             "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00");

    /* Two candidates */
    orig.candidate_count = 2;
    orig.candidates[0].type = ICE_CANDIDATE_HOST;
    orig.candidates[0].component = 1;
    orig.candidates[0].priority = 2130706431;
    strcpy(orig.candidates[0].foundation, "H0");
    rtc_addr_from_string(&orig.candidates[0].addr, "192.168.1.50", 12345);

    orig.candidates[1].type = ICE_CANDIDATE_SRFLX;
    orig.candidates[1].component = 1;
    orig.candidates[1].priority = 1694498815;
    strcpy(orig.candidates[1].foundation, "S1");
    rtc_addr_from_string(&orig.candidates[1].addr, "8.8.8.8", 54321);

    /* Generate */
    int rc = rtc_sdp_generate(&orig);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse back */
    rtc_sdp_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    rc = rtc_sdp_parse(&parsed, orig.raw, orig.raw_len);
    ASSERT_EQ(rc, RTC_OK);

    /* Verify fields match */
    ASSERT_EQ(parsed.media_type, RTC_MEDIA_AUDIO);
    ASSERT_STR_EQ(parsed.ice_ufrag, "rndUfrg");
    ASSERT_STR_EQ(parsed.ice_pwd, "rndPwd12345678901234");
    ASSERT_EQ(parsed.setup, RTC_SETUP_ACTIVE);
    ASSERT_EQ(parsed.payload_type, 96);
    ASSERT_EQ(parsed.clockrate, 8000);
    ASSERT_EQ(parsed.channels, 1);
    ASSERT_STR_EQ(parsed.codec_name, "PCMA");
    ASSERT_EQ(parsed.candidate_count, 2);

    /* Verify candidate IPs survived round-trip */
    char ip[64];
    uint16_t port;
    rtc_addr_to_string(&parsed.candidates[0].addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "192.168.1.50");
    ASSERT_EQ(port, 12345);

    rtc_addr_to_string(&parsed.candidates[1].addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "8.8.8.8");
    ASSERT_EQ(port, 54321);

    printf("    round-trip: all fields preserved (2 candidates)\n");
}

/* ------------------------------------------------------------------ */
/*  Test: SDP with maximum candidates                                  */
/* ------------------------------------------------------------------ */
TEST(sdp_many_candidates) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    sdp.type = RTC_SDP_OFFER;
    sdp.media_type = RTC_MEDIA_AUDIO;
    sdp.setup = RTC_SETUP_ACTPASS;
    sdp.payload_type = 111;
    sdp.clockrate = 48000;
    sdp.channels = 2;
    strcpy(sdp.codec_name, "opus");
    strcpy(sdp.ice_ufrag, "manyUfr");
    strcpy(sdp.ice_pwd, "manyPwd1234567890abc");
    strcpy(sdp.fingerprint, "FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:"
                            "FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF:FF");

    /* Add 8 candidates */
    sdp.candidate_count = 8;
    for (int i = 0; i < 8; i++) {
        sdp.candidates[i].type = (i % 2 == 0) ? ICE_CANDIDATE_HOST : ICE_CANDIDATE_SRFLX;
        sdp.candidates[i].component = 1;
        sdp.candidates[i].priority = (uint32_t)(2130706431 - i * 1000);
        snprintf(sdp.candidates[i].foundation, sizeof(sdp.candidates[i].foundation), "%c%d",
                 (i % 2 == 0) ? 'H' : 'S', i);
        char ip[32];
        snprintf(ip, sizeof(ip), "10.0.%d.%d", i / 256, i % 256);
        rtc_addr_from_string(&sdp.candidates[i].addr, ip, (uint16_t)(5000 + i));
    }

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse back and verify count */
    rtc_sdp_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    rc = rtc_sdp_parse(&parsed, sdp.raw, sdp.raw_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.candidate_count, 8);

    printf("    8 candidates: generate -> parse preserved all\n");
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  SDP Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(sdp_generate_audio_offer);
    RUN_TEST(sdp_generate_data_channel);
    RUN_TEST(sdp_parse);
    RUN_TEST(sdp_roundtrip);
    RUN_TEST(sdp_many_candidates);

    rtc_cleanup();
    TEST_SUMMARY();
}
