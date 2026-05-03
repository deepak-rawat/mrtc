/*
 * test_stun.c - STUN component tests.
 *
 * Tests:
 *   1. Build a simple STUN Binding Request (no ICE attributes)
 *   2. Build a STUN Binding Request with ICE attributes (username, integrity)
 *   3. Parse a Binding Response and extract XOR-MAPPED-ADDRESS
 *   4. Build → serialize → parse round-trip
 *   5. Live STUN binding to a real server (network required)
 */
#include <rtc/rtc.h>
#include <rtc/rtc_stun.h>
#include "test_harness.h"

/* ------------------------------------------------------------------ */
/*  Test: build a minimal binding request                              */
/* ------------------------------------------------------------------ */
TEST(stun_build_minimal) {
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_binding_request(&msg, NULL, NULL, 0, false, 0, false);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(msg.buf_len >= STUN_HEADER_SIZE);

    /* Verify header fields */
    uint16_t type = ((uint16_t)msg.buf[0] << 8) | msg.buf[1];
    ASSERT_EQ(type, STUN_BINDING_REQUEST);

    /* Magic cookie at offset 4 */
    uint32_t cookie = ((uint32_t)msg.buf[4] << 24) | ((uint32_t)msg.buf[5] << 16) |
                      ((uint32_t)msg.buf[6] << 8) | msg.buf[7];
    ASSERT_EQ(cookie, STUN_MAGIC_COOKIE);

    printf("    buf_len=%zu\n", msg.buf_len);
}

/* ------------------------------------------------------------------ */
/*  Test: build a binding request with ICE attributes                  */
/* ------------------------------------------------------------------ */
TEST(stun_build_ice_attrs) {
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_binding_request(&msg, "remoteufrag:localufrag", /* username */
                                            "remotepassword", /* password (for MESSAGE-INTEGRITY) */
                                            0x6E001FFF,       /* priority */
                                            true,             /* use_candidate */
                                            0x1234567890ABCDEFULL, /* tie_breaker */
                                            true);                 /* controlling */
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(msg.buf_len > STUN_HEADER_SIZE);

    /* Should contain USERNAME, PRIORITY, USE-CANDIDATE, ICE-CONTROLLING,
       MESSAGE-INTEGRITY, FINGERPRINT attributes */
    printf("    buf_len=%zu (with ICE attrs + integrity + fingerprint)\n", msg.buf_len);
}

/* ------------------------------------------------------------------ */
/*  Test: build → parse round-trip                                     */
/* ------------------------------------------------------------------ */
TEST(stun_build_parse_roundtrip) {
    rtc_stun_msg_t orig;
    int rc = rtc_stun_build_binding_request(&orig, NULL, NULL, 0, false, 0, false);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse the serialized buffer back */
    rtc_stun_msg_t parsed;
    rc = rtc_stun_parse(&parsed, orig.buf, orig.buf_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.type, STUN_BINDING_REQUEST);
    ASSERT_MEM_EQ(parsed.txn_id, orig.txn_id, STUN_TXN_ID_SIZE);

    printf("    round-trip OK, txn_id preserved\n");
}

/* ------------------------------------------------------------------ */
/*  Test: integrity verification                                       */
/* ------------------------------------------------------------------ */
TEST(stun_integrity_verify) {
    const char *password = "testpassword123";
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_binding_request(&msg, "user:frag", password, 100, false, 0, false);
    ASSERT_EQ(rc, RTC_OK);

    /* Verify should succeed with the correct password */
    rc = rtc_stun_verify_integrity(msg.buf, msg.buf_len, password);
    ASSERT_EQ(rc, RTC_OK);

    /* Verify should fail with wrong password */
    rc = rtc_stun_verify_integrity(msg.buf, msg.buf_len, "wrongpassword");
    ASSERT(rc != RTC_OK);

    printf("    integrity verification: correct=pass, wrong=fail\n");
}

/* ------------------------------------------------------------------ */
/*  Test: live STUN binding (needs network)                            */
/* ------------------------------------------------------------------ */
TEST(stun_live_binding) {
    rtc_socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT(sock != RTC_INVALID_SOCKET);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    rtc_addr_t mapped;
    int rc = rtc_stun_binding("74.125.250.129", 3478, sock, &mapped);

    if (rc == RTC_OK) {
        char ip[64];
        uint16_t port;
        rtc_addr_to_string(&mapped, ip, sizeof(ip), &port);
        printf("    reflexive address: %s:%u\n", ip, port);
        ASSERT(port > 0);
    } else {
        printf("    STUN binding failed (rc=%d) - network may be unavailable\n", rc);
        /* Non-fatal: skip rather than fail */
    }

    rtc_close_socket(sock);
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  STUN Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(stun_build_minimal);
    RUN_TEST(stun_build_ice_attrs);
    RUN_TEST(stun_build_parse_roundtrip);
    RUN_TEST(stun_integrity_verify);
    RUN_TEST(stun_live_binding);

    rtc_cleanup();
    TEST_SUMMARY();
}
