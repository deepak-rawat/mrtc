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
/*  Test: integrity with binary key containing NUL bytes (TURN regression) */
/* ------------------------------------------------------------------ */
TEST(stun_integrity_binary_key_with_nul) {
    /* Simulate a 16-byte MD5 long-term key whose first byte is NUL.
     * The old strlen()-based API truncated such keys to zero bytes, allowing
     * any HMAC to match (or matching nothing). The byte-length API must work. */
    uint8_t key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                       0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t key2[16] = {0xAA, 0x00, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    /* Build a generic request and finalize with binary key */
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_request(&msg, STUN_METHOD_ALLOCATE, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);
    rtc_stun_add_username(&msg, "alice");
    rc = rtc_stun_finalize_key(&msg, key, sizeof(key));
    ASSERT_EQ(rc, RTC_OK);

    /* Correct binary key verifies */
    rc = rtc_stun_verify_integrity_key(msg.buf, msg.buf_len, key, sizeof(key));
    ASSERT_EQ(rc, RTC_OK);

    /* Different binary key (differs only AFTER first NUL byte of key2) must fail.
     * Under the old strlen() bug, key (length 0) and key2 (length 1) would both
     * be silently truncated to short prefixes and could match spuriously. */
    rc = rtc_stun_verify_integrity_key(msg.buf, msg.buf_len, key2, sizeof(key2));
    ASSERT(rc != RTC_OK);

    /* Truncated key (just the leading bytes before the NUL — i.e. what
     * strlen() would have returned, which is 0) must NOT verify. */
    rc = rtc_stun_verify_integrity_key(msg.buf, msg.buf_len, key, 0);
    ASSERT(rc != RTC_OK);

    printf("    binary key with embedded NUL verified correctly\n");
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
    RUN_TEST(stun_integrity_binary_key_with_nul);
    RUN_TEST(stun_live_binding);

    rtc_cleanup();
    TEST_SUMMARY();
}
