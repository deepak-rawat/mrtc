/*
 * test_data_channel.c - Data channel component tests.
 *
 * Tests:
 *   1. Create a data channel and verify properties
 *   2. Open handshake: OPEN → ACK transition
 *   3. Send and receive data messages
 *   4. Send text convenience wrapper
 *   5. Close handshake
 *   6. Remote-created data channel
 *   7. Multiple channels
 *   8. Reject send before open
 */
#include <rtc/rtc.h>
#include "test_harness.h"

/* ---------- Test transport (loopback) ---------- */

/* Simulate a DTLS transport: messages sent by one side are received by the other */
static rtc_dc_manager_t *g_peer_a;
static rtc_dc_manager_t *g_peer_b;

static int loopback_send_a(const uint8_t *data, size_t len, void *user) {
    (void)user;
    /* A's outgoing data is B's incoming data */
    return rtc_dc_manager_recv(g_peer_b, data, len);
}

static int loopback_send_b(const uint8_t *data, size_t len, void *user) {
    (void)user;
    /* B's outgoing data is A's incoming data */
    return rtc_dc_manager_recv(g_peer_a, data, len);
}

/* ---------- Callback helpers ---------- */

static int g_open_count = 0;
static int g_close_count = 0;
static uint8_t g_last_msg[1024];
static size_t g_last_msg_len = 0;
static rtc_data_channel_t *g_remote_channel = NULL;

static void on_open(void *user) {
    (void)user;
    g_open_count++;
}

static void on_close(void *user) {
    (void)user;
    g_close_count++;
}

static void on_message(const uint8_t *data, size_t len, void *user) {
    (void)user;
    if (len > sizeof(g_last_msg))
        len = sizeof(g_last_msg);
    memcpy(g_last_msg, data, len);
    g_last_msg_len = len;
}

static void on_remote_channel(rtc_data_channel_t *dc, void *user) {
    (void)user;
    g_remote_channel = dc;
}

static void reset_globals(void) {
    g_open_count = 0;
    g_close_count = 0;
    g_last_msg_len = 0;
    g_remote_channel = NULL;
    memset(g_last_msg, 0, sizeof(g_last_msg));
}

/* ------------------------------------------------------------------ */
/*  Test: create a data channel and verify properties                  */
/* ------------------------------------------------------------------ */
TEST(dc_create) {
    rtc_dc_manager_t mgr;
    rtc_dc_manager_init(&mgr, loopback_send_a, NULL);

    rtc_data_channel_init_t opts = {.ordered = true,
                                    .max_retransmits = -1,
                                    .max_packet_life = -1,
                                    .protocol = "",
                                    .negotiated = false,
                                    .id = -1};

    rtc_data_channel_t *dc = rtc_dc_manager_create_channel(&mgr, "test-channel", &opts);
    ASSERT(dc != NULL);
    ASSERT_STR_EQ(rtc_data_channel_label(dc), "test-channel");
    ASSERT_EQ(rtc_data_channel_state(dc), RTC_DC_CONNECTING);
    ASSERT_EQ(rtc_data_channel_id(dc), 0);

    rtc_dc_manager_close(&mgr);
    printf("    created channel: label=\"%s\" id=%u state=CONNECTING\n", rtc_data_channel_label(dc),
           rtc_data_channel_id(dc));
}

/* ------------------------------------------------------------------ */
/*  Test: OPEN → ACK handshake                                         */
/* ------------------------------------------------------------------ */
TEST(dc_open_handshake) {
    reset_globals();
    rtc_dc_manager_t mgr_a, mgr_b;
    g_peer_a = &mgr_a;
    g_peer_b = &mgr_b;

    rtc_dc_manager_init(&mgr_a, loopback_send_a, NULL);
    rtc_dc_manager_init(&mgr_b, loopback_send_b, NULL);

    /* B listens for remote channels */
    rtc_dc_manager_on_channel(&mgr_b, on_remote_channel, NULL);

    /* A creates a channel */
    rtc_data_channel_t *dc_a = rtc_dc_manager_create_channel(&mgr_a, "chat", NULL);
    ASSERT(dc_a != NULL);
    rtc_data_channel_on_open(dc_a, on_open, NULL);

    /* Simulate DTLS connected on both sides */
    rtc_dc_manager_on_dtls_connected(&mgr_a);
    /* After A sends OPEN, B auto-creates remote channel and sends ACK */

    /* A should now be OPEN (ACK received) */
    ASSERT_EQ(rtc_data_channel_state(dc_a), RTC_DC_OPEN);
    ASSERT_EQ(g_open_count, 1);

    /* B should have received the remote channel */
    ASSERT(g_remote_channel != NULL);
    ASSERT_STR_EQ(rtc_data_channel_label(g_remote_channel), "chat");
    ASSERT_EQ(rtc_data_channel_state(g_remote_channel), RTC_DC_OPEN);

    rtc_dc_manager_close(&mgr_a);
    rtc_dc_manager_close(&mgr_b);
    printf("    OPEN/ACK handshake completed\n");
}

/* ------------------------------------------------------------------ */
/*  Test: send and receive data                                        */
/* ------------------------------------------------------------------ */
TEST(dc_send_recv) {
    reset_globals();
    rtc_dc_manager_t mgr_a, mgr_b;
    g_peer_a = &mgr_a;
    g_peer_b = &mgr_b;

    rtc_dc_manager_init(&mgr_a, loopback_send_a, NULL);
    rtc_dc_manager_init(&mgr_b, loopback_send_b, NULL);
    rtc_dc_manager_on_channel(&mgr_b, on_remote_channel, NULL);

    rtc_data_channel_t *dc_a = rtc_dc_manager_create_channel(&mgr_a, "data", NULL);
    rtc_data_channel_on_open(dc_a, on_open, NULL);
    rtc_dc_manager_on_dtls_connected(&mgr_a);

    ASSERT_EQ(rtc_data_channel_state(dc_a), RTC_DC_OPEN);
    ASSERT(g_remote_channel != NULL);

    /* Register message handler on B's channel */
    rtc_data_channel_on_message(g_remote_channel, on_message, NULL);

    /* A sends data to B */
    uint8_t test_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    int rc = rtc_data_channel_send(dc_a, test_data, sizeof(test_data));
    ASSERT_EQ(rc, RTC_OK);

    /* B should have received the message */
    ASSERT_EQ(g_last_msg_len, sizeof(test_data));
    ASSERT_MEM_EQ(g_last_msg, test_data, sizeof(test_data));

    printf("    sent %zu bytes A→B, received correctly\n", sizeof(test_data));

    /* B sends back to A */
    g_last_msg_len = 0;
    rtc_data_channel_on_message(dc_a, on_message, NULL);
    uint8_t reply[] = {0x01, 0x02, 0x03};
    rc = rtc_data_channel_send(g_remote_channel, reply, sizeof(reply));
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(g_last_msg_len, sizeof(reply));
    ASSERT_MEM_EQ(g_last_msg, reply, sizeof(reply));

    printf("    sent %zu bytes B→A, received correctly\n", sizeof(reply));

    rtc_dc_manager_close(&mgr_a);
    rtc_dc_manager_close(&mgr_b);
}

/* ------------------------------------------------------------------ */
/*  Test: send text                                                    */
/* ------------------------------------------------------------------ */
TEST(dc_send_text) {
    reset_globals();
    rtc_dc_manager_t mgr_a, mgr_b;
    g_peer_a = &mgr_a;
    g_peer_b = &mgr_b;

    rtc_dc_manager_init(&mgr_a, loopback_send_a, NULL);
    rtc_dc_manager_init(&mgr_b, loopback_send_b, NULL);
    rtc_dc_manager_on_channel(&mgr_b, on_remote_channel, NULL);

    rtc_data_channel_t *dc_a = rtc_dc_manager_create_channel(&mgr_a, "text", NULL);
    rtc_dc_manager_on_dtls_connected(&mgr_a);

    ASSERT(g_remote_channel != NULL);
    rtc_data_channel_on_message(g_remote_channel, on_message, NULL);

    int rc = rtc_data_channel_send_text(dc_a, "Hello, World!");
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(g_last_msg_len, 13);
    ASSERT(memcmp(g_last_msg, "Hello, World!", 13) == 0);

    printf("    text message sent and received: \"%.*s\"\n", (int)g_last_msg_len, g_last_msg);

    rtc_dc_manager_close(&mgr_a);
    rtc_dc_manager_close(&mgr_b);
}

/* ------------------------------------------------------------------ */
/*  Test: close handshake                                              */
/* ------------------------------------------------------------------ */
TEST(dc_close) {
    reset_globals();
    rtc_dc_manager_t mgr_a, mgr_b;
    g_peer_a = &mgr_a;
    g_peer_b = &mgr_b;

    rtc_dc_manager_init(&mgr_a, loopback_send_a, NULL);
    rtc_dc_manager_init(&mgr_b, loopback_send_b, NULL);
    rtc_dc_manager_on_channel(&mgr_b, on_remote_channel, NULL);

    rtc_data_channel_t *dc_a = rtc_dc_manager_create_channel(&mgr_a, "closeme", NULL);
    rtc_dc_manager_on_dtls_connected(&mgr_a);

    ASSERT(g_remote_channel != NULL);
    rtc_data_channel_on_close(g_remote_channel, on_close, NULL);

    /* A closes the channel */
    rtc_data_channel_close(dc_a);
    ASSERT_EQ(rtc_data_channel_state(dc_a), RTC_DC_CLOSED);
    /* B should have received close notification */
    ASSERT_EQ(g_close_count, 1);
    ASSERT_EQ(rtc_data_channel_state(g_remote_channel), RTC_DC_CLOSED);

    printf("    close handshake completed\n");

    rtc_dc_manager_close(&mgr_a);
    rtc_dc_manager_close(&mgr_b);
}

/* ------------------------------------------------------------------ */
/*  Test: multiple channels                                            */
/* ------------------------------------------------------------------ */
TEST(dc_multiple_channels) {
    reset_globals();
    rtc_dc_manager_t mgr_a, mgr_b;
    g_peer_a = &mgr_a;
    g_peer_b = &mgr_b;

    rtc_dc_manager_init(&mgr_a, loopback_send_a, NULL);
    rtc_dc_manager_init(&mgr_b, loopback_send_b, NULL);

    int remote_count = 0;
    rtc_dc_manager_on_channel(&mgr_b, on_remote_channel, NULL);

    rtc_data_channel_t *ch1 = rtc_dc_manager_create_channel(&mgr_a, "channel-1", NULL);
    rtc_data_channel_t *ch2 = rtc_dc_manager_create_channel(&mgr_a, "channel-2", NULL);

    ASSERT(ch1 != NULL);
    ASSERT(ch2 != NULL);
    ASSERT(rtc_data_channel_id(ch1) != rtc_data_channel_id(ch2));

    /* Connect both */
    rtc_dc_manager_on_dtls_connected(&mgr_a);

    ASSERT_EQ(rtc_data_channel_state(ch1), RTC_DC_OPEN);
    ASSERT_EQ(rtc_data_channel_state(ch2), RTC_DC_OPEN);
    ASSERT_EQ((int)rtc_u32_map_len(&mgr_b.channels), 2);

    printf("    created 2 channels with IDs %u and %u\n", rtc_data_channel_id(ch1),
           rtc_data_channel_id(ch2));

    rtc_dc_manager_close(&mgr_a);
    rtc_dc_manager_close(&mgr_b);
}

/* ------------------------------------------------------------------ */
/*  Test: reject send before channel is open                           */
/* ------------------------------------------------------------------ */
TEST(dc_send_before_open) {
    rtc_dc_manager_t mgr;
    rtc_dc_manager_init(&mgr, loopback_send_a, NULL);

    rtc_data_channel_t *dc = rtc_dc_manager_create_channel(&mgr, "pending", NULL);
    ASSERT(dc != NULL);
    ASSERT_EQ(rtc_data_channel_state(dc), RTC_DC_CONNECTING);

    /* Try to send — should fail */
    uint8_t data[] = {0x01, 0x02};
    int rc = rtc_data_channel_send(dc, data, sizeof(data));
    ASSERT(rc != RTC_OK);

    printf("    correctly rejected send on non-open channel\n");

    rtc_dc_manager_close(&mgr);
}

/* ------------------------------------------------------------------ */
/*  Test: 16-bit channel ID round-trips (regression for 8-bit truncation) */
/* ------------------------------------------------------------------ */
TEST(dc_large_channel_id) {
    reset_globals();
    rtc_dc_manager_t mgr_a, mgr_b;
    g_peer_a = &mgr_a;
    g_peer_b = &mgr_b;

    rtc_dc_manager_init(&mgr_a, loopback_send_a, NULL);
    rtc_dc_manager_init(&mgr_b, loopback_send_b, NULL);
    rtc_dc_manager_on_channel(&mgr_b, on_remote_channel, NULL);

    /* Force a negotiated channel ID well past the 8-bit boundary. */
    rtc_data_channel_init_t opts = {.ordered = true,
                                    .max_retransmits = -1,
                                    .max_packet_life = -1,
                                    .protocol = "",
                                    .negotiated = true,
                                    .id = 0x1234};
    rtc_data_channel_t *dc_a = rtc_dc_manager_create_channel(&mgr_a, "wide", &opts);
    ASSERT(dc_a != NULL);
    ASSERT_EQ(rtc_data_channel_id(dc_a), 0x1234);

    rtc_data_channel_on_open(dc_a, on_open, NULL);
    rtc_dc_manager_on_dtls_connected(&mgr_a);

    /* The OPEN message must carry the full 16-bit ID; if the wire format
     * truncated to 8 bits, B would create a channel with id 0x34 and the
     * ACK would never match A's pending channel. */
    ASSERT_EQ(rtc_data_channel_state(dc_a), RTC_DC_OPEN);
    ASSERT(g_remote_channel != NULL);
    ASSERT_EQ(rtc_data_channel_id(g_remote_channel), 0x1234);

    /* Data round-trip on the high-id channel */
    rtc_data_channel_on_message(g_remote_channel, on_message, NULL);
    uint8_t payload[] = {0x55, 0xAA, 0x55, 0xAA};
    int rc = rtc_data_channel_send(dc_a, payload, sizeof(payload));
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(g_last_msg_len, sizeof(payload));
    ASSERT_MEM_EQ(g_last_msg, payload, sizeof(payload));

    printf("    16-bit channel id 0x%04x round-trip OK\n", rtc_data_channel_id(dc_a));

    rtc_dc_manager_close(&mgr_a);
    rtc_dc_manager_close(&mgr_b);
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Data Channel Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(dc_create);
    RUN_TEST(dc_open_handshake);
    RUN_TEST(dc_send_recv);
    RUN_TEST(dc_send_text);
    RUN_TEST(dc_close);
    RUN_TEST(dc_multiple_channels);
    RUN_TEST(dc_send_before_open);
    RUN_TEST(dc_large_channel_id);

    rtc_cleanup();
    TEST_SUMMARY();
}
