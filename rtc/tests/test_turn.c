/*
 * TURN unit tests (no network): STUN method builders,
 * ChannelData framing, long-term credential computation.
 */
#include "test_harness.h"
#include "rtc_stun.h"
#include "rtc_turn.h"
#include "rtc/rtc_common.h"
#include <string.h>

TEST(turn_stream_fragmentation_padding_and_bounds) {
    uint8_t data[32] = {0x40, 0x01, 0, 1, 0x55, 0, 0, 0};
    size_t length;
    for (size_t size = 0; size < 8; size++)
        ASSERT_EQ(rtc_turn_stream_frame_length(data, size, &length), RTC_ERR_TIMEOUT);
    ASSERT_EQ(rtc_turn_stream_frame_length(data, sizeof(data), &length), RTC_OK);
    ASSERT_EQ(length, 8);
    data[8] = 0x01;
    data[9] = 0x01;
    ASSERT_EQ(rtc_turn_stream_frame_length(data + 8, 19, &length), RTC_ERR_TIMEOUT);
    ASSERT_EQ(rtc_turn_stream_frame_length(data + 8, 20, &length), RTC_OK);
    ASSERT_EQ(length, 20);
    data[0] = 0x80;
    ASSERT_EQ(rtc_turn_stream_frame_length(data, sizeof(data), &length), RTC_ERR_INVALID);
    data[0] = 0x40;
    data[2] = 0xff;
    data[3] = 0xff;
    ASSERT_EQ(rtc_turn_stream_frame_length(data, sizeof(data), &length), RTC_ERR_INVALID);
}

TEST(turn_allocate_request) {
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_request(&msg, STUN_METHOD_ALLOCATE, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_stun_add_requested_transport(&msg, 17); /* UDP */
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_stun_finalize(&msg, NULL);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse back */
    rtc_stun_msg_t parsed;
    rc = rtc_stun_parse(&parsed, msg.buf, msg.buf_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(parsed.type, STUN_METHOD_ALLOCATE);

    /* Find REQUESTED-TRANSPORT */
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(&parsed, STUN_ATTR_REQUESTED_TRANSPORT, &alen);
    ASSERT(val != NULL);
    ASSERT_EQ(alen, 4);
    ASSERT_EQ(val[0], 17); /* UDP */
    printf("    Allocate request: %zu bytes, transport=UDP\n", msg.buf_len);
}

TEST(turn_ipv6_relay_address_and_truncation) {
    uint8_t wire[] = {0x01, 0x03, 0x00, 0x18, 0x21, 0x12, 0xa4, 0x42, 0x00, 0x01, 0x02,
                      0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x00, 0x16,
                      0x00, 0x14, 0x00, 0x02, 0xe2, 0x42, 0x21, 0x12, 0xa4, 0x42, 0x00,
                      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0a};
    rtc_stun_msg_t response;
    rtc_addr_t address;
    ASSERT_EQ(rtc_stun_parse(&response, wire, sizeof(wire)), RTC_OK);
    ASSERT_EQ(rtc_stun_get_relayed_address(&response, &address), RTC_OK);
    char host[64];
    uint16_t port;
    ASSERT_EQ(rtc_addr_to_string(&address, host, sizeof(host), &port), RTC_OK);
    ASSERT_EQ(strcmp(host, "::1"), 0);
    ASSERT_EQ(port, 50000);
    wire[3] = 12;
    wire[23] = 8;
    ASSERT_EQ(rtc_stun_parse(&response, wire, 32), RTC_OK);
    ASSERT_EQ(rtc_stun_get_relayed_address(&response, &address), RTC_ERR_INVALID);
}

TEST(turn_channel_bind_request) {
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_request(&msg, STUN_METHOD_CHANNEL_BIND, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);

    rc = rtc_stun_add_channel_number(&msg, 0x4001);
    ASSERT_EQ(rc, RTC_OK);

    rtc_addr_t peer;
    rtc_addr_from_string(&peer, "192.168.1.100", 5000);
    rc = rtc_stun_add_xor_peer_address(&msg, &peer);
    ASSERT_EQ(rc, RTC_OK);

    rc = rtc_stun_finalize(&msg, NULL);
    ASSERT_EQ(rc, RTC_OK);

    /* Parse and verify */
    rtc_stun_msg_t parsed;
    rc = rtc_stun_parse(&parsed, msg.buf, msg.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    uint16_t clen;
    const uint8_t *cval = rtc_stun_find_attr(&parsed, STUN_ATTR_CHANNEL_NUMBER, &clen);
    ASSERT(cval != NULL);
    uint16_t channel = ((uint16_t)cval[0] << 8) | cval[1];
    ASSERT_EQ(channel, 0x4001);

    const uint8_t *pval = rtc_stun_find_attr(&parsed, STUN_ATTR_XOR_PEER_ADDRESS, &clen);
    ASSERT(pval != NULL);
    printf("    ChannelBind request: channel=0x%04x\n", channel);
}

TEST(turn_channel_data_frame) {
    uint8_t buf[256];
    const uint8_t payload[] = "Hello via TURN!";
    size_t frame_len;

    int rc =
        rtc_turn_build_channel_data(buf, sizeof(buf), 0x4001, payload, sizeof(payload), &frame_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(frame_len >= TURN_CHANNEL_DATA_HEADER + sizeof(payload));

    /* Parse back */
    uint16_t channel;
    const uint8_t *data;
    size_t data_len;
    rc = rtc_turn_parse_channel_data(buf, frame_len, &channel, &data, &data_len);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(channel, 0x4001);
    ASSERT_EQ(data_len, sizeof(payload));
    ASSERT(memcmp(data, payload, sizeof(payload)) == 0);
    printf("    ChannelData frame: channel=0x%04x, %zu bytes\n", channel, data_len);
}

TEST(turn_channel_data_invalid) {
    uint16_t channel;
    const uint8_t *data;
    size_t data_len;

    /* Too short */
    uint8_t short_buf[] = {0x40, 0x01};
    ASSERT(rtc_turn_parse_channel_data(short_buf, 2, &channel, &data, &data_len) != RTC_OK);

    /* Invalid channel (< 0x4000) */
    uint8_t bad_chan[] = {0x00, 0x01, 0x00, 0x04, 0x01, 0x02, 0x03, 0x04};
    ASSERT(rtc_turn_parse_channel_data(bad_chan, 8, &channel, &data, &data_len) != RTC_OK);

    /* Build with invalid channel */
    uint8_t buf[64];
    size_t len;
    ASSERT(rtc_turn_build_channel_data(buf, sizeof(buf), 0x3000, (const uint8_t *)"x", 1, &len) !=
           RTC_OK);

    printf("    invalid ChannelData correctly rejected\n");
}

TEST(turn_long_term_credential) {
    uint8_t key[16];
    int rc = rtc_stun_long_term_key("user", "mrtc", "pass", key);
    ASSERT_EQ(rc, RTC_OK);

    /* Verify key is non-zero */
    int nonzero = 0;
    for (int i = 0; i < 16; i++)
        if (key[i])
            nonzero++;
    ASSERT(nonzero > 0);

    /* Same inputs → same key */
    uint8_t key2[16];
    rtc_stun_long_term_key("user", "mrtc", "pass", key2);
    ASSERT(memcmp(key, key2, 16) == 0);

    /* Different password → different key */
    uint8_t key3[16];
    rtc_stun_long_term_key("user", "mrtc", "wrong", key3);
    ASSERT(memcmp(key, key3, 16) != 0);

    printf("    long-term key: %02x%02x%02x%02x...\n", key[0], key[1], key[2], key[3]);
}

TEST(turn_create_perm_request) {
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_request(&msg, STUN_METHOD_CREATE_PERM, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);

    rtc_addr_t peer;
    rtc_addr_from_string(&peer, "10.0.0.1", 9000);
    rc = rtc_stun_add_xor_peer_address(&msg, &peer);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_stun_finalize(&msg, NULL);
    ASSERT_EQ(rc, RTC_OK);

    rtc_stun_msg_t parsed;
    rc = rtc_stun_parse(&parsed, msg.buf, msg.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(&parsed, STUN_ATTR_XOR_PEER_ADDRESS, &alen);
    ASSERT(val != NULL);
    printf("    CreatePermission request: peer address present\n");
}

TEST(turn_refresh_zero) {
    rtc_stun_msg_t msg;
    int rc = rtc_stun_build_request(&msg, STUN_METHOD_REFRESH, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_stun_add_lifetime(&msg, 0);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_stun_finalize(&msg, NULL);
    ASSERT_EQ(rc, RTC_OK);

    rtc_stun_msg_t parsed;
    rc = rtc_stun_parse(&parsed, msg.buf, msg.buf_len);
    ASSERT_EQ(rc, RTC_OK);

    uint32_t lifetime;
    rc = rtc_stun_get_lifetime(&parsed, &lifetime);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(lifetime, 0);
    printf("    Refresh(lifetime=0) request built/parsed OK\n");
}

TEST(turn_find_attr_missing) {
    rtc_stun_msg_t msg;
    rtc_stun_build_request(&msg, STUN_METHOD_ALLOCATE, NULL, NULL);
    rtc_stun_finalize(&msg, NULL);

    rtc_stun_msg_t parsed;
    rtc_stun_parse(&parsed, msg.buf, msg.buf_len);

    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(&parsed, STUN_ATTR_LIFETIME, &alen);
    ASSERT(val == NULL);
    printf("    missing attribute correctly returns NULL\n");
}

int main(void) {
    printf("========================================\n");
    printf("  TURN Unit Tests\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(turn_allocate_request);
    RUN_TEST(turn_ipv6_relay_address_and_truncation);
    RUN_TEST(turn_stream_fragmentation_padding_and_bounds);
    RUN_TEST(turn_channel_bind_request);
    RUN_TEST(turn_channel_data_frame);
    RUN_TEST(turn_channel_data_invalid);
    RUN_TEST(turn_long_term_credential);
    RUN_TEST(turn_create_perm_request);
    RUN_TEST(turn_refresh_zero);
    RUN_TEST(turn_find_attr_missing);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
