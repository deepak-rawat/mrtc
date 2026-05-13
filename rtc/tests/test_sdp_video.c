/*
 * test_sdp_video.c - SDP tests for video and multi-media support.
 *
 * Tests:
 *   1. Generate a video SDP and verify structure
 *   2. Parse a video SDP
 *   3. Generate multi-media SDP (audio + video + data channel)
 *   4. Parse multi-media SDP and verify all media lines
 *   5. Multi-media round-trip (generate → parse)
 *   6. Video codec variants (VP8, H264)
 */
#include <rtc/rtc.h>
#include "test_harness.h"

/* ------------------------------------------------------------------ */
/*  Test: generate video offer SDP                                     */
/* ------------------------------------------------------------------ */
TEST(sdp_generate_video) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    sdp.type = RTC_SDP_OFFER;
    sdp.media_type = RTC_MEDIA_VIDEO;
    sdp.setup = RTC_SETUP_ACTPASS;
    sdp.payload_type = 96;
    sdp.clockrate = 90000;
    sdp.channels = 0;
    strcpy(sdp.codec_name, "VP8");
    strcpy(sdp.ice_ufrag, "VidUfrg");
    strcpy(sdp.ice_pwd, "VideoPwd12345678901234");
    strcpy(sdp.fingerprint, "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99");

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(sdp.raw_len > 0);

    /* Verify key SDP lines */
    ASSERT(strstr(sdp.raw, "m=video") != NULL);
    ASSERT(strstr(sdp.raw, "VP8/90000") != NULL);
    ASSERT(strstr(sdp.raw, "a=sendrecv") != NULL);
    ASSERT(strstr(sdp.raw, "a=rtcp-mux") != NULL);
    /* Video should NOT have channel count in rtpmap */
    ASSERT(strstr(sdp.raw, "VP8/90000/") == NULL);

    printf("    generated video SDP: %zu bytes\n", sdp.raw_len);
}

/* ------------------------------------------------------------------ */
/*  Test: parse video SDP text                                         */
/* ------------------------------------------------------------------ */
TEST(sdp_parse_video) {
    const char *sdp_text = "v=0\r\n"
                           "o=mrtc 1234567 1 IN IP4 0.0.0.0\r\n"
                           "s=mrtc\r\n"
                           "t=0 0\r\n"
                           "a=group:BUNDLE 0\r\n"
                           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:0\r\n"
                           "a=ice-ufrag:vidUfrg\r\n"
                           "a=ice-pwd:vidPwd12345678901234\r\n"
                           "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                           "a=setup:actpass\r\n"
                           "a=sendrecv\r\n"
                           "a=rtcp-mux\r\n"
                           "a=rtpmap:96 VP8/90000\r\n"
                           "a=candidate:H0 1 udp 2130706431 10.0.0.1 5000 typ host\r\n";

    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    int rc = rtc_sdp_parse(&sdp, sdp_text, strlen(sdp_text));
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(sdp.media_type, RTC_MEDIA_VIDEO);
    ASSERT_EQ(sdp.payload_type, 96);
    ASSERT_EQ(sdp.clockrate, 90000);
    ASSERT_STR_EQ(sdp.codec_name, "VP8");
    ASSERT_EQ((int)rtc_sdp_candidate_count(&sdp), 1);

    /* Multi-media array should also have the video */
    ASSERT_EQ(sdp.media_count, 1);
    ASSERT_EQ(sdp.media[0].media_type, RTC_MEDIA_VIDEO);
    ASSERT_EQ(sdp.media[0].payload_type, 96);
    ASSERT_STR_EQ(sdp.media[0].codec_name, "VP8");

    printf("    parsed video SDP: codec=%s/%d\n", sdp.codec_name, sdp.clockrate);
}

/* ------------------------------------------------------------------ */
/*  Test: generate multi-media SDP (audio + video + data)              */
/* ------------------------------------------------------------------ */
TEST(sdp_generate_multi_media) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    sdp.type = RTC_SDP_OFFER;
    sdp.setup = RTC_SETUP_ACTPASS;
    strcpy(sdp.ice_ufrag, "MmUfrag");
    strcpy(sdp.ice_pwd, "MultiMediaPwd123456789");
    strcpy(sdp.fingerprint, "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
                            "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00");

    /* Audio media line */
    sdp.media[0].media_type = RTC_MEDIA_AUDIO;
    sdp.media[0].payload_type = 111;
    sdp.media[0].clockrate = 48000;
    sdp.media[0].channels = 2;
    strcpy(sdp.media[0].codec_name, "opus");
    sdp.media[0].mid_index = 0;

    /* Video media line */
    sdp.media[1].media_type = RTC_MEDIA_VIDEO;
    sdp.media[1].payload_type = 96;
    sdp.media[1].clockrate = 90000;
    sdp.media[1].channels = 0;
    strcpy(sdp.media[1].codec_name, "VP8");
    sdp.media[1].mid_index = 1;

    /* Data channel */
    sdp.media[2].media_type = RTC_MEDIA_APPLICATION;
    sdp.media[2].mid_index = 2;

    sdp.media_count = 3;

    /* Add a candidate */
    rtc_ice_candidate_t vcand = {0};
    vcand.type = ICE_CANDIDATE_HOST;
    vcand.component = 1;
    vcand.priority = 2130706431;
    strcpy(vcand.foundation, "H0");
    rtc_addr_from_string(&vcand.addr, "192.168.1.50", 9000);
    rtc_sdp_add_candidate(&sdp, &vcand);

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(sdp.raw_len > 0);

    /* Verify all three media lines present */
    ASSERT(strstr(sdp.raw, "m=audio") != NULL);
    ASSERT(strstr(sdp.raw, "m=video") != NULL);
    ASSERT(strstr(sdp.raw, "m=application") != NULL);

    /* Verify BUNDLE group has all three */
    ASSERT(strstr(sdp.raw, "a=group:BUNDLE 0 1 2") != NULL);

    /* Verify codec lines */
    ASSERT(strstr(sdp.raw, "opus/48000/2") != NULL);
    ASSERT(strstr(sdp.raw, "VP8/90000") != NULL);
    ASSERT(strstr(sdp.raw, "a=sctp-port:5000") != NULL);

    printf("    generated multi-media SDP: %zu bytes\n", sdp.raw_len);
    printf("    %.80s...\n", sdp.raw);
}

/* ------------------------------------------------------------------ */
/*  Test: parse multi-media SDP                                        */
/* ------------------------------------------------------------------ */
TEST(sdp_parse_multi_media) {
    const char *sdp_text = "v=0\r\n"
                           "o=mrtc 9999 1 IN IP4 0.0.0.0\r\n"
                           "s=mrtc\r\n"
                           "t=0 0\r\n"
                           "a=group:BUNDLE 0 1 2\r\n"
                           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:0\r\n"
                           "a=ice-ufrag:mmUfrag\r\n"
                           "a=ice-pwd:mmPwd12345678901234ab\r\n"
                           "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                           "a=setup:actpass\r\n"
                           "a=sendrecv\r\n"
                           "a=rtcp-mux\r\n"
                           "a=rtpmap:111 opus/48000/2\r\n"
                           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:1\r\n"
                           "a=ice-ufrag:mmUfrag\r\n"
                           "a=ice-pwd:mmPwd12345678901234ab\r\n"
                           "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                           "a=setup:actpass\r\n"
                           "a=sendrecv\r\n"
                           "a=rtcp-mux\r\n"
                           "a=rtpmap:96 VP8/90000\r\n"
                           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:2\r\n"
                           "a=ice-ufrag:mmUfrag\r\n"
                           "a=ice-pwd:mmPwd12345678901234ab\r\n"
                           "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                           "a=setup:actpass\r\n"
                           "a=sctp-port:5000\r\n"
                           "a=candidate:H0 1 udp 2130706431 10.0.0.1 5000 typ host\r\n";

    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    int rc = rtc_sdp_parse(&sdp, sdp_text, strlen(sdp_text));
    ASSERT_EQ(rc, RTC_OK);

    /* Should have 3 media lines */
    ASSERT_EQ(sdp.media_count, 3);

    /* Audio */
    ASSERT_EQ(sdp.media[0].media_type, RTC_MEDIA_AUDIO);
    ASSERT_EQ(sdp.media[0].payload_type, 111);
    ASSERT_EQ(sdp.media[0].clockrate, 48000);
    ASSERT_EQ(sdp.media[0].channels, 2);
    ASSERT_STR_EQ(sdp.media[0].codec_name, "opus");

    /* Video */
    ASSERT_EQ(sdp.media[1].media_type, RTC_MEDIA_VIDEO);
    ASSERT_EQ(sdp.media[1].payload_type, 96);
    ASSERT_EQ(sdp.media[1].clockrate, 90000);
    ASSERT_STR_EQ(sdp.media[1].codec_name, "VP8");

    /* Data channel */
    ASSERT_EQ(sdp.media[2].media_type, RTC_MEDIA_APPLICATION);

    /* Candidates */
    ASSERT_EQ((int)rtc_sdp_candidate_count(&sdp), 1);

    printf("    parsed %d media lines: audio, video, application\n", sdp.media_count);
}

/* ------------------------------------------------------------------ */
/*  Test: multi-media round-trip                                       */
/* ------------------------------------------------------------------ */
TEST(sdp_multi_media_roundtrip) {
    rtc_sdp_t orig;
    memset(&orig, 0, sizeof(orig));

    orig.type = RTC_SDP_OFFER;
    orig.setup = RTC_SETUP_ACTIVE;
    strcpy(orig.ice_ufrag, "RtUfrag");
    strcpy(orig.ice_pwd, "RoundTripPwd12345678ab");
    strcpy(orig.fingerprint, "FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00:"
                             "FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00");

    orig.media[0].media_type = RTC_MEDIA_AUDIO;
    orig.media[0].payload_type = 111;
    orig.media[0].clockrate = 48000;
    orig.media[0].channels = 2;
    strcpy(orig.media[0].codec_name, "opus");
    orig.media[0].mid_index = 0;

    orig.media[1].media_type = RTC_MEDIA_VIDEO;
    orig.media[1].payload_type = 96;
    orig.media[1].clockrate = 90000;
    orig.media[1].channels = 0;
    strcpy(orig.media[1].codec_name, "VP8");
    orig.media[1].mid_index = 1;

    orig.media_count = 2;

    rtc_ice_candidate_t rtcand = {0};
    rtcand.type = ICE_CANDIDATE_HOST;
    rtcand.component = 1;
    rtcand.priority = 2130706431;
    strcpy(rtcand.foundation, "H0");
    rtc_addr_from_string(&rtcand.addr, "172.16.0.10", 7000);
    rtc_sdp_add_candidate(&orig, &rtcand);

    int rc = rtc_sdp_generate(&orig);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse back */
    rtc_sdp_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    rc = rtc_sdp_parse(&parsed, orig.raw, orig.raw_len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(parsed.media_count, 2);

    /* Audio survived */
    ASSERT_EQ(parsed.media[0].media_type, RTC_MEDIA_AUDIO);
    ASSERT_STR_EQ(parsed.media[0].codec_name, "opus");
    ASSERT_EQ(parsed.media[0].clockrate, 48000);

    /* Video survived */
    ASSERT_EQ(parsed.media[1].media_type, RTC_MEDIA_VIDEO);
    ASSERT_STR_EQ(parsed.media[1].codec_name, "VP8");
    ASSERT_EQ(parsed.media[1].clockrate, 90000);

    /* Candidate survived */
    ASSERT_EQ((int)rtc_sdp_candidate_count(&parsed), 1);
    char ip[64];
    uint16_t port;
    const rtc_ice_candidate_t *pc0 = rtc_sdp_get_candidate(&parsed, 0);
    rtc_addr_to_string(&pc0->addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "172.16.0.10");
    ASSERT_EQ(port, 7000);

    printf("    round-trip: 2 media lines preserved\n");
    rtc_sdp_close(&orig);
    rtc_sdp_close(&parsed);
}

/* ------------------------------------------------------------------ */
/*  Test: H264 video codec                                             */
/* ------------------------------------------------------------------ */
TEST(sdp_video_h264) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    sdp.type = RTC_SDP_OFFER;
    sdp.media_type = RTC_MEDIA_VIDEO;
    sdp.setup = RTC_SETUP_ACTPASS;
    sdp.payload_type = 102;
    sdp.clockrate = 90000;
    sdp.channels = 0;
    strcpy(sdp.codec_name, "H264");
    strcpy(sdp.ice_ufrag, "H264Ufr");
    strcpy(sdp.ice_pwd, "H264Pwd12345678901234");
    strcpy(sdp.fingerprint, "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99");

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT(strstr(sdp.raw, "m=video") != NULL);
    ASSERT(strstr(sdp.raw, "H264/90000") != NULL);

    /* Parse back */
    rtc_sdp_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    rc = rtc_sdp_parse(&parsed, sdp.raw, sdp.raw_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.media_type, RTC_MEDIA_VIDEO);
    ASSERT_STR_EQ(parsed.codec_name, "H264");
    ASSERT_EQ(parsed.clockrate, 90000);

    printf("    H264 codec round-trip OK\n");
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  SDP Video / Multi-Media Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(sdp_generate_video);
    RUN_TEST(sdp_parse_video);
    RUN_TEST(sdp_generate_multi_media);
    RUN_TEST(sdp_parse_multi_media);
    RUN_TEST(sdp_multi_media_roundtrip);
    RUN_TEST(sdp_video_h264);

    rtc_cleanup();
    TEST_SUMMARY();
}
