/*
 * test_signaling_msg.c - Tests for signaling message build/parse.
 */
#include "test_harness.h"
#include "signaling/signaling_msg.h"
#include <rtc/rtc_common.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
TEST(json_build_join) {
    char *json = sig_msg_build_join("test-meeting");
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"join\"") != NULL);
    ASSERT(strstr(json, "\"meeting\":\"test-meeting\"") != NULL);
    printf("    join: %s\n", json);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_build_leave) {
    char *json = sig_msg_build_leave();
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"leave\"") != NULL);
    printf("    leave: %s\n", json);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_build_offer) {
    char *json = sig_msg_build_offer("abc123", "v=0\r\no=mrtc ...");
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"offer\"") != NULL);
    ASSERT(strstr(json, "\"to\":\"abc123\"") != NULL);
    ASSERT(strstr(json, "\"sdp\":\"v=0") != NULL);
    printf("    offer: %.60s...\n", json);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_build_joined) {
    const char *peers[] = {"peer1", "peer2"};
    char *json = sig_msg_build_joined("myid", peers, 2);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"joined\"") != NULL);
    ASSERT(strstr(json, "\"peer_id\":\"myid\"") != NULL);
    ASSERT(strstr(json, "\"peer1\"") != NULL);
    ASSERT(strstr(json, "\"peer2\"") != NULL);
    printf("    joined: %s\n", json);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_build_joined_empty) {
    char *json = sig_msg_build_joined("myid", NULL, 0);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"peers\":[]") != NULL);
    printf("    joined (empty): %s\n", json);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_parse_joined) {
    const char *peers[] = {"p1", "p2", "p3"};
    char *json = sig_msg_build_joined("me123", peers, 3);
    ASSERT(json != NULL);

    sig_msg_t msg;
    int rc = sig_msg_parse(&msg, json, strlen(json));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(msg.type, SIG_MSG_JOINED);
    ASSERT_STR_EQ(msg.peer_id, "me123");
    ASSERT_EQ(msg.peer_count, 3);
    ASSERT_STR_EQ(msg.peers[0], "p1");
    ASSERT_STR_EQ(msg.peers[1], "p2");
    ASSERT_STR_EQ(msg.peers[2], "p3");
    printf("    parsed joined: id=%s peers=%d\n", msg.peer_id, msg.peer_count);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_parse_offer) {
    char *json = sig_msg_build_relay_offer("sender42", "v=0\r\ntest sdp");
    ASSERT(json != NULL);

    sig_msg_t msg;
    int rc = sig_msg_parse(&msg, json, strlen(json));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(msg.type, SIG_MSG_OFFER);
    ASSERT_STR_EQ(msg.from, "sender42");
    ASSERT(strstr(msg.sdp, "v=0") != NULL);
    printf("    parsed offer: from=%s\n", msg.from);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_parse_candidate) {
    char *json = sig_msg_build_candidate("target1", "candidate:H0 1 udp 2130706431 ...");
    ASSERT(json != NULL);

    sig_msg_t msg;
    int rc = sig_msg_parse(&msg, json, strlen(json));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(msg.type, SIG_MSG_CANDIDATE);
    ASSERT_STR_EQ(msg.to, "target1");
    ASSERT(strstr(msg.candidate, "candidate:H0") != NULL);
    printf("    parsed candidate: to=%s\n", msg.to);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_parse_error) {
    char *json = sig_msg_build_error("meeting full");
    ASSERT(json != NULL);

    sig_msg_t msg;
    int rc = sig_msg_parse(&msg, json, strlen(json));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(msg.type, SIG_MSG_ERROR);
    ASSERT_STR_EQ(msg.error_msg, "meeting full");
    printf("    parsed error: %s\n", msg.error_msg);
    free(json);
}

/* ------------------------------------------------------------------ */
TEST(json_parse_malformed) {
    sig_msg_t msg;
    /* Not valid JSON */
    ASSERT_EQ(sig_msg_parse(&msg, "not json", 8), -1);
    /* Missing "type" field */
    ASSERT_EQ(sig_msg_parse(&msg, "{\"foo\":1}", 9), -1);
    /* Unknown type */
    ASSERT_EQ(sig_msg_parse(&msg, "{\"type\":\"bogus\"}", 16), -1);
    printf("    malformed messages correctly rejected\n");
}

/* ------------------------------------------------------------------ */
TEST(json_roundtrip_all_types) {
    sig_msg_t msg;
    char *json;

    /* join */
    json = sig_msg_build_join("room1");
    ASSERT_EQ(sig_msg_parse(&msg, json, strlen(json)), 0);
    ASSERT_EQ(msg.type, SIG_MSG_JOIN);
    ASSERT_STR_EQ(msg.meeting, "room1");
    free(json);

    /* leave */
    json = sig_msg_build_leave();
    ASSERT_EQ(sig_msg_parse(&msg, json, strlen(json)), 0);
    ASSERT_EQ(msg.type, SIG_MSG_LEAVE);
    free(json);

    /* answer */
    json = sig_msg_build_answer("tgt", "sdp-answer");
    ASSERT_EQ(sig_msg_parse(&msg, json, strlen(json)), 0);
    ASSERT_EQ(msg.type, SIG_MSG_ANSWER);
    ASSERT_STR_EQ(msg.to, "tgt");
    ASSERT_STR_EQ(msg.sdp, "sdp-answer");
    free(json);

    /* peer_joined */
    json = sig_msg_build_peer_joined("newpeer");
    ASSERT_EQ(sig_msg_parse(&msg, json, strlen(json)), 0);
    ASSERT_EQ(msg.type, SIG_MSG_PEER_JOINED);
    ASSERT_STR_EQ(msg.peer_id, "newpeer");
    free(json);

    /* peer_left */
    json = sig_msg_build_peer_left("gonepeer");
    ASSERT_EQ(sig_msg_parse(&msg, json, strlen(json)), 0);
    ASSERT_EQ(msg.type, SIG_MSG_PEER_LEFT);
    ASSERT_STR_EQ(msg.peer_id, "gonepeer");
    free(json);

    printf("    all message types round-trip OK\n");
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Signaling Message Tests\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(json_build_join);
    RUN_TEST(json_build_leave);
    RUN_TEST(json_build_offer);
    RUN_TEST(json_build_joined);
    RUN_TEST(json_build_joined_empty);
    RUN_TEST(json_parse_joined);
    RUN_TEST(json_parse_offer);
    RUN_TEST(json_parse_candidate);
    RUN_TEST(json_parse_error);
    RUN_TEST(json_parse_malformed);
    RUN_TEST(json_roundtrip_all_types);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
