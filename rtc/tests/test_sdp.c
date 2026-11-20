/*
 * SDP component tests.
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
#include "rtc/rtc_sdp.h"
#include "test_harness.h"

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
    rtc_ice_candidate_t cand0 = {0};
    cand0.type = ICE_CANDIDATE_HOST;
    cand0.component = 1;
    cand0.priority = 2130706431;
    strcpy(cand0.foundation, "H0");
    rtc_addr_from_string(&cand0.addr, "192.168.1.100", 5000);
    rtc_sdp_add_candidate(&sdp, &cand0);

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
    rtc_sdp_close(&sdp);
}

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
    rtc_sdp_close(&sdp);
}

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
                           "a=candidate:S1 1 udp 1694498815 203.0.113.5 6000 typ srflx "
                           "raddr 10.0.0.1 rport 5000\r\n";

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
    ASSERT_EQ((int)rtc_sdp_candidate_count(&sdp), 2);

    const rtc_ice_candidate_t *c0 = rtc_sdp_get_candidate(&sdp, 0);
    ASSERT_EQ(c0->type, ICE_CANDIDATE_HOST);
    ASSERT_EQ(c0->priority, 2130706431);
    char ip[64];
    uint16_t port;
    rtc_addr_to_string(&c0->addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "10.0.0.1");
    ASSERT_EQ(port, 5000);

    const rtc_ice_candidate_t *c1 = rtc_sdp_get_candidate(&sdp, 1);
    ASSERT_EQ(c1->type, ICE_CANDIDATE_SRFLX);
    rtc_addr_to_string(&c1->addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "203.0.113.5");
    ASSERT_EQ(port, 6000);
    ASSERT(c1->has_related_addr);
    rtc_addr_to_string(&c1->related_addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "10.0.0.1");
    ASSERT_EQ(port, 5000);

    printf("    parsed: ufrag=%s codec=%s/%d/%d candidates=%zu\n", sdp.ice_ufrag, sdp.codec_name,
           sdp.clockrate, sdp.channels, rtc_sdp_candidate_count(&sdp));
    rtc_sdp_close(&sdp);
}

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
    rtc_ice_candidate_t cand0 = {0};
    cand0.type = ICE_CANDIDATE_HOST;
    cand0.component = 1;
    cand0.priority = 2130706431;
    strcpy(cand0.foundation, "H0");
    rtc_addr_from_string(&cand0.addr, "192.168.1.50", 12345);
    rtc_sdp_add_candidate(&orig, &cand0);

    rtc_ice_candidate_t cand1 = {0};
    cand1.type = ICE_CANDIDATE_SRFLX;
    cand1.component = 1;
    cand1.priority = 1694498815;
    strcpy(cand1.foundation, "S1");
    rtc_addr_from_string(&cand1.addr, "8.8.8.8", 54321);
    rtc_addr_from_string(&cand1.related_addr, "192.168.1.50", 12345);
    cand1.has_related_addr = true;
    rtc_sdp_add_candidate(&orig, &cand1);

    rtc_ice_candidate_t cand2 = {0};
    cand2.type = ICE_CANDIDATE_RELAY;
    cand2.component = 1;
    cand2.priority = 16777215;
    strcpy(cand2.foundation, "R2");
    rtc_addr_from_string(&cand2.addr, "203.0.113.7", 3478);
    rtc_sdp_add_candidate(&orig, &cand2);

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
    ASSERT_EQ((int)rtc_sdp_candidate_count(&parsed), 3);

    /* Verify candidate IPs survived round-trip */
    char ip[64];
    uint16_t port;
    const rtc_ice_candidate_t *pc0 = rtc_sdp_get_candidate(&parsed, 0);
    rtc_addr_to_string(&pc0->addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "192.168.1.50");
    ASSERT_EQ(port, 12345);
    ASSERT_EQ(pc0->type, ICE_CANDIDATE_HOST);

    const rtc_ice_candidate_t *pc1 = rtc_sdp_get_candidate(&parsed, 1);
    rtc_addr_to_string(&pc1->addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "8.8.8.8");
    ASSERT_EQ(port, 54321);
    ASSERT_EQ(pc1->type, ICE_CANDIDATE_SRFLX);
    ASSERT(pc1->has_related_addr);
    rtc_addr_to_string(&pc1->related_addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "192.168.1.50");
    ASSERT_EQ(port, 12345);

    const rtc_ice_candidate_t *pc2 = rtc_sdp_get_candidate(&parsed, 2);
    rtc_addr_to_string(&pc2->addr, ip, sizeof(ip), &port);
    ASSERT_STR_EQ(ip, "203.0.113.7");
    ASSERT_EQ(port, 3478);
    ASSERT_EQ(pc2->type, ICE_CANDIDATE_RELAY);

    printf("    round-trip: all fields preserved (3 candidates)\n");
    rtc_sdp_close(&orig);
    rtc_sdp_close(&parsed);
}

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
    for (int i = 0; i < 8; i++) {
        rtc_ice_candidate_t c = {0};
        c.type = (i % 2 == 0) ? ICE_CANDIDATE_HOST : ICE_CANDIDATE_SRFLX;
        c.component = 1;
        c.priority = (uint32_t)(2130706431 - i * 1000);
        snprintf(c.foundation, sizeof(c.foundation), "%c%d", (i % 2 == 0) ? 'H' : 'S', i);
        char ip[32];
        snprintf(ip, sizeof(ip), "10.0.%d.%d", i / 256, i % 256);
        rtc_addr_from_string(&c.addr, ip, (uint16_t)(5000 + i));
        rtc_sdp_add_candidate(&sdp, &c);
    }

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse back and verify count */
    rtc_sdp_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    rc = rtc_sdp_parse(&parsed, sdp.raw, sdp.raw_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ((int)rtc_sdp_candidate_count(&parsed), 8);

    printf("    8 candidates: generate -> parse preserved all\n");
    rtc_sdp_close(&sdp);
    rtc_sdp_close(&parsed);
}

TEST(sdp_extmap_roundtrip) {
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_OFFER;
    sdp.setup = RTC_SETUP_ACTPASS;
    strcpy(sdp.ice_ufrag, "uf");
    strcpy(sdp.ice_pwd, "pwdpwdpwdpwdpwdpwdpwdpw");
    strcpy(sdp.fingerprint, "00:11");
    sdp.media_count = 1;
    sdp.media[0].media_type = RTC_MEDIA_VIDEO;
    sdp.media[0].payload_type = 96;
    sdp.media[0].clockrate = 90000;
    sdp.media[0].mid_index = 0;
    strcpy(sdp.media[0].codec_name, "VP8");
    ASSERT_EQ(rtc_sdp_media_add_extmap(&sdp.media[0], 2,
                                       "http://www.webrtc.org/experiments/rtp-hdrext/"
                                       "abs-send-time"),
              RTC_OK);
    ASSERT_EQ(rtc_sdp_media_add_extmap(&sdp.media[0], 5,
                                       "http://www.ietf.org/id/"
                                       "draft-holmer-rmcat-transport-wide-cc-extensions-01"),
              RTC_OK);

    int rc = rtc_sdp_generate(&sdp);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(strstr(sdp.raw, "a=extmap:2 http://www.webrtc.org") != NULL);
    ASSERT(strstr(sdp.raw, "a=extmap:5 http://www.ietf.org") != NULL);

    rtc_sdp_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    rc = rtc_sdp_parse(&parsed, sdp.raw, sdp.raw_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ((int)parsed.media[0].extmap_count, 2);
    ASSERT_EQ((int)rtc_sdp_media_find_extmap_id(&parsed.media[0], "http://www.webrtc.org/"
                                                                  "experiments/rtp-hdrext/"
                                                                  "abs-send-time"),
              2);
    ASSERT_EQ((int)rtc_sdp_media_find_extmap_id(&parsed.media[0], "http://www.ietf.org/id/"
                                                                  "draft-holmer-rmcat-transport-"
                                                                  "wide-cc-extensions-01"),
              5);

    rtc_sdp_close(&sdp);
    rtc_sdp_close(&parsed);
}

TEST(sdp_preserves_late_mid_and_rejects_extension_aliases) {
    const char offer[] =
        "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=extmap:14 urn:ietf:params:rtp-hdrext:toffset\r\n"
        "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
        "a=extmap:13 urn:3gpp:video-orientation\r\n"
        "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
        "a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type\r\n"
        "a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing\r\n"
        "a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space\r\n"
        "a=extmap:4/sendrecv urn:ietf:params:rtp-hdrext:sdes:mid\r\n";
    rtc_sdp_t parsed = {0};
    ASSERT_EQ(rtc_sdp_parse(&parsed, offer, strlen(offer)), RTC_OK);
    ASSERT_EQ(rtc_sdp_media_find_extmap_id(&parsed.media[0], "urn:ietf:params:rtp-hdrext:sdes:mid"),
              4);
    ASSERT_EQ(rtc_sdp_media_add_extmap(&parsed.media[0], 4, "urn:example:different-extension"),
              RTC_ERR_INVALID);
    rtc_sdp_close(&parsed);
    char conflict[sizeof(offer) + 128];
    snprintf(conflict, sizeof(conflict), "%sa=extmap:4 urn:example:different-extension\r\n", offer);
    ASSERT_EQ(rtc_sdp_parse(&parsed, conflict, strlen(conflict)), RTC_ERR_SDP);
    rtc_sdp_close(&parsed);
}

TEST(sdp_candidate_helpers) {
    ASSERT_EQ(rtc_ice_candidate_priority(ICE_CANDIDATE_HOST, 65535, 1), 2130706431u);
    ASSERT_EQ(rtc_ice_candidate_priority(ICE_CANDIDATE_SRFLX, 65535, 1), 1694498815u);

    rtc_ice_candidate_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.type = ICE_CANDIDATE_SRFLX;
    cand.component = 1;
    cand.priority = rtc_ice_candidate_priority(cand.type, 65535, cand.component);
    strcpy(cand.foundation, "S0");
    ASSERT_EQ(rtc_addr_from_string(&cand.addr, "203.0.113.7", 40000), RTC_OK);
    ASSERT_EQ(rtc_addr_from_string(&cand.related_addr, "10.1.2.3", 50000), RTC_OK);
    cand.has_related_addr = true;

    char line[256];
    ASSERT_EQ(rtc_ice_candidate_to_string(&cand, line, sizeof(line)), RTC_OK);
    ASSERT_STR_EQ(line, "candidate:S0 1 udp 1694498815 203.0.113.7 40000 typ srflx "
                        "raddr 10.1.2.3 rport 50000");

    rtc_ice_candidate_t parsed;
    ASSERT_EQ(rtc_sdp_parse_candidate_line(line, &parsed), RTC_OK);
    ASSERT_EQ(parsed.type, ICE_CANDIDATE_SRFLX);
    ASSERT(parsed.has_related_addr);

    printf("    candidate helpers: priority + srflx raddr/rport OK\n");
}

TEST(browser_candidates_preserve_mdns_and_tcp_metadata) {
    const char *name = "12345678-1234-1234-1234-123456789abc.local";
    char line[256];
    snprintf(line, sizeof(line),
             "candidate:mdns 1 UDP 2122260223 %s 50000 typ host generation 0 "
             "ufrag browser",
             name);
    rtc_ice_candidate_t candidate;
    ASSERT_EQ(rtc_sdp_parse_candidate_line(line, &candidate), RTC_OK);
    ASSERT_STR_EQ(candidate.hostname, name);
    ASSERT_STR_EQ(candidate.protocol, "udp");
    ASSERT_STR_EQ(candidate.username_fragment, "browser");
    ASSERT_EQ(candidate.hostname_port, 50000);
    char encoded[256];
    ASSERT_EQ(rtc_ice_candidate_to_string(&candidate, encoded, sizeof(encoded)), RTC_OK);
    rtc_ice_candidate_t roundtrip;
    ASSERT_EQ(rtc_sdp_parse_candidate_line(encoded, &roundtrip), RTC_OK);
    ASSERT_STR_EQ(roundtrip.hostname, name);
    ASSERT_STR_EQ(roundtrip.username_fragment, "browser");
    ASSERT_EQ(rtc_sdp_parse_candidate_line("candidate:tcp 1 TCP 1518280447 127.0.0.1 9 typ host "
                                           "tcptype active",
                                           &candidate),
              RTC_OK);
    ASSERT_STR_EQ(candidate.protocol, "tcp");
    ASSERT_STR_EQ(candidate.tcp_type, "active");
    ASSERT_EQ(rtc_ice_candidate_to_string(&candidate, encoded, sizeof(encoded)), RTC_OK);
    ASSERT(strstr(encoded, "tcptype active") != NULL);
    ASSERT_EQ(rtc_sdp_parse_candidate_line("candidate:prflx 1 udp 1862270975 127.0.0.1 50000 typ "
                                           "prflx",
                                           &candidate),
              RTC_OK);
    ASSERT_EQ(candidate.type, ICE_CANDIDATE_PRFLX);
}

TEST(browser_candidate_rejection_is_not_blanket_filtering) {
    const char *invalid[] = {
        "candidate:x 1 udp 2122260223 bad..local 50000 typ host",
        "candidate:x 1 udp 2122260223 -bad.local 50000 typ host",
        "candidate:x 1 udp 2122260223 bad-.local 50000 typ host",
        "candidate:x 1 udp 2122260223 example.com 50000 typ host",
        "candidate:x 1 udp 2122260223 valid.local 50000 typ srflx",
        "candidate:x 1 tcp 1518280447 127.0.0.1 9 typ host",
        "candidate:x 1 tcp 1518280447 127.0.0.1 9 typ host tcptype fake",
        "candidate:x 1 udp 2122260223 127.0.0.1 50000 typ host tcptype active",
        "candidate:x 1 quic 2122260223 127.0.0.1 50000 typ host",
        "candidate:x 257 udp 2122260223 127.0.0.1 50000 typ host",
        "candidate:x 1 udp 4294967296 127.0.0.1 50000 typ host",
        "candidate:x 1 udp -1 127.0.0.1 50000 typ host",
        "candidate:x 1 udp 2122260223 127.0.0.1 50000junk typ host",
        "candidate:x 1 udp 2122260223 127.0.0.1 0 typ host",
        "candidate:x 1 udp 2122260223 127.0.0.1 50000 typ host ufrag",
    };
    rtc_ice_candidate_t candidate;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        ASSERT(rtc_sdp_parse_candidate_line(invalid[i], &candidate) != RTC_OK);
}

TEST(embedded_candidates_are_retained_or_rejected_explicitly) {
    const char *text = "v=0\r\n"
                       "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                       "a=candidate:mdns 1 udp 2122260223 camera.local 50000 typ host";
    size_t len = strlen(text);
    char *bytes = malloc(len);
    ASSERT(bytes != NULL);
    memcpy(bytes, text, len);
    rtc_sdp_t sdp = {0};
    ASSERT_EQ(rtc_sdp_parse(&sdp, bytes, len), RTC_OK);
    ASSERT_EQ(rtc_sdp_candidate_count(&sdp), 1);
    ASSERT_STR_EQ(rtc_sdp_get_candidate(&sdp, 0)->hostname, "camera.local");
    free(bytes);
    const char *bad = "v=0\r\na=candidate:malformed\r\n";
    ASSERT(rtc_sdp_parse(&sdp, bad, strlen(bad)) != RTC_OK);
    rtc_sdp_close(&sdp);
}

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
    RUN_TEST(sdp_extmap_roundtrip);
    RUN_TEST(sdp_preserves_late_mid_and_rejects_extension_aliases);
    RUN_TEST(sdp_candidate_helpers);
    RUN_TEST(browser_candidates_preserve_mdns_and_tcp_metadata);
    RUN_TEST(browser_candidate_rejection_is_not_blanket_filtering);
    RUN_TEST(embedded_candidates_are_retained_or_rejected_explicitly);

    rtc_cleanup();
    TEST_SUMMARY();
}
