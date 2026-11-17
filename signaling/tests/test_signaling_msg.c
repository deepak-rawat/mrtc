/*
 * Tests for signaling message build/parse.
 */
#include "test_harness.h"
#include "signaling/signaling_msg.h"
#include <rtc/rtc_common.h>
#include <string.h>
#include <stdlib.h>

TEST(json_build_join) {
    char *json = sig_msg_build_join("test-meeting");
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"join\"") != NULL);
    ASSERT(strstr(json, "\"meeting\":\"test-meeting\"") != NULL);
    printf("    join: %s\n", json);
    free(json);
}

TEST(json_build_leave) {
    char *json = sig_msg_build_leave();
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"leave\"") != NULL);
    printf("    leave: %s\n", json);
    free(json);
}

TEST(json_build_offer) {
    char *json = sig_msg_build_offer("abc123", "v=0\r\no=mrtc ...");
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"offer\"") != NULL);
    ASSERT(strstr(json, "\"to\":\"abc123\"") != NULL);
    ASSERT(strstr(json, "\"sdp\":\"v=0") != NULL);
    printf("    offer: %.60s...\n", json);
    free(json);
}

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

TEST(json_build_joined_empty) {
    char *json = sig_msg_build_joined("myid", NULL, 0);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"peers\":[]") != NULL);
    printf("    joined (empty): %s\n", json);
    free(json);
}

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

TEST(json_sfu_mode_and_end_of_candidates) {
    sig_msg_t message;
    char *json = sig_msg_build_joined_mode("alice", NULL, 0, "sfu");
    ASSERT(json != NULL);
    ASSERT_EQ(sig_msg_parse(&message, json, strlen(json)), 0);
    ASSERT_STR_EQ(message.mode, "sfu");
    free(json);
    json = sig_msg_build_candidate("sfu", "");
    ASSERT(json != NULL);
    ASSERT_EQ(sig_msg_parse(&message, json, strlen(json)), 0);
    ASSERT_EQ(message.candidate[0], '\0');
    ASSERT_STR_EQ(message.to, "sfu");
    free(json);
}

TEST(json_rejects_truncated_negotiation) {
    char *sdp = malloc(SIG_MAX_SDP + 1);
    ASSERT(sdp != NULL);
    memset(sdp, 'a', SIG_MAX_SDP);
    sdp[SIG_MAX_SDP] = '\0';
    char *json = sig_msg_build_offer("sfu", sdp);
    ASSERT(json != NULL);
    sig_msg_t message;
    ASSERT_EQ(sig_msg_parse(&message, json, strlen(json)), -1);
    free(json);
    free(sdp);
}

TEST(versioned_join_and_topology_roundtrip) {
    sig_msg_t message = {.type = SIG_MSG_JOIN,
                         .version = SIG_PROTOCOL_VERSION,
                         .can_mesh = true,
                         .can_sfu = true,
                         .can_switch = true};
    strcpy(message.meeting, "room");
    strcpy(message.display_name, "Alice");
    char *json = sig_msg_build(&message);
    ASSERT(json != NULL);
    sig_msg_t parsed;
    ASSERT_EQ(sig_msg_parse(&parsed, json, strlen(json)), 0);
    ASSERT_EQ(parsed.version, SIG_PROTOCOL_VERSION);
    ASSERT(parsed.can_mesh && parsed.can_sfu && parsed.can_switch);
    ASSERT_STR_EQ(parsed.display_name, "Alice");
    free(json);

    message.epoch = 7;
    strcpy(message.mode, "sfu");
    strcpy(message.reason, "network path failed");
    message.peer_count = 1;
    strcpy(message.peers[0], "bob");
    strcpy(message.peer_names[0], "Bob");
    const sig_msg_type_t types[] = {SIG_MSG_TOPOLOGY_PREPARE, SIG_MSG_TOPOLOGY_READY,
                                    SIG_MSG_TOPOLOGY_COMMIT,  SIG_MSG_TOPOLOGY_ACK,
                                    SIG_MSG_TOPOLOGY_ABORT,   SIG_MSG_TOPOLOGY_REQUEST};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        message.type = types[i];
        json = sig_msg_build(&message);
        ASSERT(json != NULL);
        ASSERT_EQ(sig_msg_parse(&parsed, json, strlen(json)), 0);
        ASSERT_EQ(parsed.type, types[i]);
        ASSERT_EQ(parsed.epoch, 7);
        ASSERT_STR_EQ(parsed.mode, "sfu");
        ASSERT_STR_EQ(parsed.peers[0], "bob");
        ASSERT_STR_EQ(parsed.peer_names[0], "Bob");
        ASSERT_STR_EQ(parsed.reason, message.reason);
        free(json);
    }
}

TEST(versioned_negotiation_preserves_session_and_eoc) {
    sig_msg_t message = {.type = SIG_MSG_OFFER, .version = SIG_PROTOCOL_VERSION, .epoch = 9};
    ASSERT_EQ(sig_session_id(message.session_id, SIG_SESSION_MESH, 9, "bob", "alice"), RTC_OK);
    strcpy(message.to, "alice");
    strcpy(message.sdp, "v=0\r\n");
    char *json = sig_msg_build(&message);
    ASSERT(json != NULL);
    sig_msg_t parsed;
    ASSERT_EQ(sig_msg_parse(&parsed, json, strlen(json)), 0);
    ASSERT_STR_EQ(parsed.session_id, "m:9:alice:bob");
    ASSERT_STR_EQ(parsed.sdp, message.sdp);
    free(json);
    message.type = SIG_MSG_CANDIDATE;
    message.sdp[0] = '\0';
    json = sig_msg_build(&message);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"candidate\":\"\"") != NULL);
    ASSERT_EQ(sig_msg_parse(&parsed, json, strlen(json)), 0);
    ASSERT_EQ(parsed.epoch, 9);
    ASSERT_STR_EQ(parsed.session_id, message.session_id);
    ASSERT_EQ(parsed.candidate[0], '\0');
    free(json);
}

TEST(versioned_messages_reject_ambiguous_or_unbounded_fields) {
    const char *invalid[] = {
        "{\"type\":\"topology_commit\",\"version\":2,\"epoch\":1.5,\"mode\":\"sfu\"}",
        "{\"type\":\"topology_commit\",\"version\":2,\"epoch\":4294967296,\"mode\":\"sfu\"}",
        "{\"type\":\"topology_commit\",\"version\":2,\"epoch\":0,\"mode\":\"sfu\"}",
        "{\"type\":\"topology_commit\",\"version\":3,\"epoch\":1,\"mode\":\"sfu\"}",
        "{\"type\":\"topology_commit\",\"version\":2,\"epoch\":1,\"epoch\":2,\"mode\":\"sfu\"}",
        "{\"type\":\"topology_abort\",\"version\":2,\"epoch\":1,\"mode\":\"sfu\"}",
        "{\"type\":\"join\",\"version\":2,\"meeting\":\"room\",\"capabilities\":{\"mesh\":1}}",
        "{\"type\":\"candidate\",\"version\":2,\"epoch\":1,\"session_id\":\"p:1:alice\",\"to\":"
        "\"sfu\"}",
        "{\"type\":\"offer\",\"version\":2,\"epoch\":1,\"to\":\"sfu\",\"sdp\":\"v=0\"}",
        "{\"type\":\"joined\",\"peers\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}",
        "{\"type\":\"joined\",\"peers\":[\"bob\",\"bob\"]}",
        "{\"type\":\"leave\"} trailing",
    };
    sig_msg_t message;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        ASSERT_EQ(sig_msg_parse(&message, invalid[i], strlen(invalid[i])), -1);
    memset(&message, 0, sizeof(message));
    message.type = SIG_MSG_OFFER;
    memset(message.sdp, 'a', sizeof(message.sdp));
    ASSERT(sig_msg_build(&message) == NULL);
}

TEST(session_identity_is_role_and_epoch_scoped) {
    char first[SIG_MAX_SESSION_ID], second[SIG_MAX_SESSION_ID];
    ASSERT_EQ(sig_session_id(first, SIG_SESSION_MESH, 3, "alice", "bob"), RTC_OK);
    ASSERT_EQ(sig_session_id(second, SIG_SESSION_MESH, 3, "bob", "alice"), RTC_OK);
    ASSERT_STR_EQ(first, second);
    ASSERT_EQ(sig_session_id(first, SIG_SESSION_PUBLISH, 3, "alice", NULL), RTC_OK);
    ASSERT_STR_EQ(first, "p:3:alice");
    ASSERT_EQ(sig_session_id(first, SIG_SESSION_SUBSCRIBE, 4, "bob", "alice"), RTC_OK);
    ASSERT_STR_EQ(first, "s:4:bob:alice");
    ASSERT(sig_session_id(first, SIG_SESSION_MESH, 0, "alice", "bob") != RTC_OK);
    ASSERT(sig_session_id(first, SIG_SESSION_MESH, 3, "alice", "alice") != RTC_OK);
    ASSERT(sig_session_id(first, SIG_SESSION_MESH, 3, "alice:other", "bob") != RTC_OK);
}

TEST(conference_envelope_preserves_binary_payload_and_message_id) {
    uint8_t wire[128];
    const uint8_t payload[] = {0, 0xff, 'x'};
    size_t wire_len = 0;
    uint64_t id = UINT64_C(0x0123456789abcdef);
    ASSERT_EQ(sig_dc_write(wire, sizeof(wire), &wire_len, "alice", id, payload, sizeof(payload)),
              RTC_OK);
    ASSERT_EQ(wire[20], 0x01);
    ASSERT_EQ(wire[27], 0xef);
    char peer[SIG_MAX_PEER_ID];
    uint64_t decoded_id;
    const uint8_t *decoded;
    size_t decoded_len;
    ASSERT_EQ(sig_dc_read(wire, wire_len, peer, &decoded_id, &decoded, &decoded_len), RTC_OK);
    ASSERT_STR_EQ(peer, "alice");
    ASSERT(decoded_id == id);
    ASSERT_EQ(decoded_len, sizeof(payload));
    ASSERT_MEM_EQ(decoded, payload, sizeof(payload));
    ASSERT(sig_dc_read(wire, SIG_DC_HEADER_SIZE, peer, &decoded_id, &decoded, &decoded_len) !=
           RTC_OK);
    ASSERT_EQ(sig_dc_write(wire, sizeof(wire), &wire_len, "*", 1, NULL, 0), RTC_OK);
    ASSERT_EQ(sig_dc_read(wire, wire_len, peer, &decoded_id, &decoded, &decoded_len), RTC_OK);
    ASSERT_STR_EQ(peer, "*");
    ASSERT_EQ(decoded_len, 0);
    memset(wire + SIG_DC_HEADER_SIZE, 0, 8);
    ASSERT(sig_dc_read(wire, wire_len, peer, &decoded_id, &decoded, &decoded_len) != RTC_OK);
    ASSERT_EQ(sig_dc_write(wire, 4, &wire_len, "alice", 1, payload, sizeof(payload)),
              RTC_ERR_NOMEM);
    memset(wire, 0, sizeof(wire));
    memcpy(wire, "MRTC", 4);
    memcpy(wire + 4, "bob", 3);
    memcpy(wire + SIG_DC_HEADER_SIZE, payload, sizeof(payload));
    ASSERT_EQ(sig_dc_read(wire, SIG_DC_HEADER_SIZE + sizeof(payload), peer, &decoded_id, &decoded,
                          &decoded_len),
              RTC_OK);
    ASSERT_STR_EQ(peer, "bob");
    ASSERT_EQ(decoded_id, 0);
    ASSERT_MEM_EQ(decoded, payload, sizeof(payload));
}

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
    RUN_TEST(json_sfu_mode_and_end_of_candidates);
    RUN_TEST(json_rejects_truncated_negotiation);
    RUN_TEST(versioned_join_and_topology_roundtrip);
    RUN_TEST(versioned_negotiation_preserves_session_and_eoc);
    RUN_TEST(versioned_messages_reject_ambiguous_or_unbounded_fields);
    RUN_TEST(session_identity_is_role_and_epoch_scoped);
    RUN_TEST(conference_envelope_preserves_binary_payload_and_message_id);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
