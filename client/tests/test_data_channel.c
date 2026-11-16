#include <rtc/rtc_client.h>
#include "rtc_data_channel_internal.h"
#include "test_harness.h"

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

typedef struct packet {
    struct packet *next;
    rtc_dc_manager_t *destination;
    size_t len;
    uint8_t bytes[];
} packet_t;

typedef struct {
    rtc_dc_manager_t a, b;
    packet_t *head, *tail;
    bool drop_data;
    unsigned dropped;
    rtc_data_channel_t *remote;
    uint8_t received[RTC_DC_MAX_MSG_SIZE];
    size_t received_len;
    unsigned messages;
} pair_t;

typedef struct {
    pair_t *pair;
    rtc_dc_manager_t *destination;
} link_t;

static int send_packet(const uint8_t *data, size_t len, void *user) {
    link_t *link = user;
    pair_t *pair = link->pair;
    if (pair->drop_data && len > 12 && data[12] == 0) {
        pair->drop_data = false;
        pair->dropped++;
        return RTC_OK;
    }
    packet_t *packet = malloc(sizeof(*packet) + len);
    if (!packet)
        return RTC_ERR_NOMEM;
    packet->next = NULL;
    packet->destination = link->destination;
    packet->len = len;
    memcpy(packet->bytes, data, len);
    if (pair->tail)
        pair->tail->next = packet;
    else
        pair->head = packet;
    pair->tail = packet;
    return RTC_OK;
}

static void on_message(const uint8_t *data, size_t len, void *user) {
    pair_t *pair = user;
    if (len <= sizeof(pair->received)) {
        memcpy(pair->received, data, len);
        pair->received_len = len;
        pair->messages++;
    }
}

static void on_channel(rtc_data_channel_t *channel, void *user) {
    pair_t *pair = user;
    pair->remote = channel;
    rtc_data_channel_on_message(channel, on_message, pair);
}

static void on_low(void *user) {
    unsigned *count = user;
    (*count)++;
}

static bool pump_until(pair_t *pair, rtc_data_channel_t *channel, unsigned messages,
                       int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    do {
        unsigned budget = 10000;
        while (pair->head && budget--) {
            packet_t *packet = pair->head;
            pair->head = packet->next;
            if (!pair->head)
                pair->tail = NULL;
            rtc_dc_manager_recv(packet->destination, packet->bytes, packet->len);
            free(packet);
        }
        rtc_dc_manager_tick(&pair->a, 0);
        rtc_dc_manager_tick(&pair->b, 0);
        if ((!channel || rtc_data_channel_state(channel) == RTC_DC_OPEN) &&
            pair->messages >= messages)
            return true;
        SLEEP_MS(5);
    } while (rtc_time_ms() < deadline);
    return false;
}

static void close_pair(pair_t *pair) {
    rtc_dc_manager_close(&pair->a);
    rtc_dc_manager_close(&pair->b);
    while (pair->head) {
        packet_t *packet = pair->head;
        pair->head = packet->next;
        free(packet);
    }
}

TEST(sctp_dcep_loss_fragmentation_and_close) {
    pair_t pair = {0};
    link_t ab = {&pair, &pair.b}, ba = {&pair, &pair.a};
    ASSERT_EQ(rtc_dc_manager_init(&pair.a, send_packet, &ab), RTC_OK);
    ASSERT_EQ(rtc_dc_manager_init(&pair.b, send_packet, &ba), RTC_OK);
    rtc_dc_manager_on_channel(&pair.b, on_channel, &pair);
    rtc_data_channel_t *channel = rtc_dc_manager_create_channel(&pair.a, "reliable", NULL);
    ASSERT(channel != NULL);
    ASSERT(rtc_data_channel_send_text(channel, "not open") != RTC_OK);
    ASSERT_EQ(rtc_dc_manager_connect(&pair.a, true), RTC_OK);
    ASSERT_EQ(rtc_dc_manager_connect(&pair.b, false), RTC_OK);
    ASSERT(pump_until(&pair, channel, 0, 5000));
    ASSERT(pair.remote != NULL);
    ASSERT_STR_EQ(rtc_data_channel_label(pair.remote), "reliable");
    ASSERT_EQ(rtc_data_channel_id(channel) % 2, 0);
    uint8_t payload[48000];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 31);
    pair.drop_data = true;
    ASSERT_EQ(rtc_data_channel_send(channel, payload, sizeof(payload)), RTC_OK);
    ASSERT(pump_until(&pair, channel, 1, 10000));
    ASSERT_EQ(pair.dropped, 1);
    ASSERT_EQ(pair.received_len, sizeof(payload));
    ASSERT_MEM_EQ(pair.received, payload, sizeof(payload));
    ASSERT_EQ(rtc_data_channel_bytes_sent(channel), sizeof(payload));
    ASSERT_EQ(rtc_data_channel_bytes_received(pair.remote), sizeof(payload));
    ASSERT_EQ(rtc_data_channel_send_text(channel, ""), RTC_OK);
    ASSERT(pump_until(&pair, channel, 2, 3000));
    ASSERT_EQ(pair.received_len, 0);
    unsigned low_events = 0, queued = 0;
    rtc_data_channel_on_buffered_amount_low(channel, on_low, &low_events);
    while (!rtc_data_channel_buffered_amount(channel) && queued < 64) {
        ASSERT_EQ(rtc_data_channel_send(channel, payload, sizeof(payload)), RTC_OK);
        queued++;
    }
    ASSERT(rtc_data_channel_buffered_amount(channel) > 0);
    ASSERT_EQ(low_events, 0);
    ASSERT(pump_until(&pair, channel, 2 + queued, 10000));
    ASSERT_EQ(rtc_data_channel_buffered_amount(channel), 0);
    ASSERT(low_events > 0);
    rtc_data_channel_close(channel);
    uint64_t deadline = rtc_time_ms() + 5000;
    while (rtc_time_ms() < deadline && rtc_data_channel_state(pair.remote) != RTC_DC_CLOSED) {
        (void)pump_until(&pair, NULL, 0, 10);
        SLEEP_MS(5);
    }
    ASSERT_EQ(rtc_data_channel_state(pair.remote), RTC_DC_CLOSED);
    close_pair(&pair);
}

TEST(sctp_negotiated_options_and_validation) {
    pair_t pair = {0};
    link_t ab = {&pair, &pair.b}, ba = {&pair, &pair.a};
    ASSERT_EQ(rtc_dc_manager_init(&pair.a, send_packet, &ab), RTC_OK);
    ASSERT_EQ(rtc_dc_manager_init(&pair.b, send_packet, &ba), RTC_OK);
    rtc_data_channel_init_t opts = {.ordered = false,
                                    .max_retransmits = 0,
                                    .max_packet_life = -1,
                                    .negotiated = true,
                                    .id = 0x1234,
                                    .protocol = "telemetry"};
    rtc_data_channel_t *a = rtc_dc_manager_create_channel(&pair.a, "negotiated", &opts);
    rtc_data_channel_t *b = rtc_dc_manager_create_channel(&pair.b, "negotiated", &opts);
    ASSERT(a && b);
    ASSERT(rtc_dc_manager_create_channel(&pair.a, "duplicate", &opts) == NULL);
    opts.max_packet_life = 10;
    ASSERT(rtc_dc_manager_create_channel(&pair.a, "conflicting reliability", &opts) == NULL);
    opts.max_packet_life = -1;
    opts.id = -1;
    ASSERT(rtc_dc_manager_create_channel(&pair.a, "missing negotiated id", &opts) == NULL);
    rtc_data_channel_on_message(b, on_message, &pair);
    ASSERT_EQ(rtc_dc_manager_connect(&pair.a, true), RTC_OK);
    ASSERT_EQ(rtc_dc_manager_connect(&pair.b, false), RTC_OK);
    ASSERT(pump_until(&pair, a, 0, 5000));
    ASSERT_EQ(rtc_data_channel_state(b), RTC_DC_OPEN);
    ASSERT_EQ(rtc_data_channel_id(a), 0x1234);
    pair.drop_data = true;
    ASSERT_EQ(rtc_data_channel_send_text(a, "must not retransmit"), RTC_OK);
    (void)pump_until(&pair, NULL, 0, 10);
    ASSERT_EQ(rtc_data_channel_send_text(a, "unordered partial reliability"), RTC_OK);
    ASSERT(pump_until(&pair, a, 1, 3000));
    ASSERT_EQ(pair.received_len, strlen("unordered partial reliability"));
    ASSERT_MEM_EQ(pair.received, "unordered partial reliability", pair.received_len);
    ASSERT(!pump_until(&pair, a, 2, 4500));
    ASSERT_EQ(pair.dropped, 1);
    ASSERT_EQ(rtc_u32_map_len(&pair.b.channels), 1);
    opts.id = 0x1236;
    opts.max_retransmits = -1;
    opts.max_packet_life = 10;
    rtc_data_channel_t *ttl_a = rtc_dc_manager_create_channel(&pair.a, "ttl", &opts);
    rtc_data_channel_t *ttl_b = rtc_dc_manager_create_channel(&pair.b, "ttl", &opts);
    ASSERT(ttl_a && ttl_b);
    rtc_data_channel_on_message(ttl_b, on_message, &pair);
    pair.drop_data = true;
    ASSERT_EQ(rtc_data_channel_send_text(ttl_a, "expired"), RTC_OK);
    (void)pump_until(&pair, NULL, 0, 10);
    SLEEP_MS(30);
    ASSERT_EQ(rtc_data_channel_send_text(ttl_a, "fresh"), RTC_OK);
    ASSERT(pump_until(&pair, ttl_a, 2, 3000));
    ASSERT_EQ(pair.received_len, 5);
    ASSERT_MEM_EQ(pair.received, "fresh", 5);
    ASSERT(!pump_until(&pair, ttl_a, 3, 4500));
    ASSERT_EQ(pair.dropped, 2);
    unsigned close_events = 0;
    rtc_data_channel_on_close(a, on_low, &close_events);
    close_pair(&pair);
    ASSERT_EQ(close_events, 1);
}

TEST(sctp_dtls_server_allocates_odd_dcep_stream) {
    pair_t pair = {0};
    link_t ab = {&pair, &pair.b}, ba = {&pair, &pair.a};
    ASSERT_EQ(rtc_dc_manager_init(&pair.a, send_packet, &ab), RTC_OK);
    ASSERT_EQ(rtc_dc_manager_init(&pair.b, send_packet, &ba), RTC_OK);
    rtc_dc_manager_on_channel(&pair.b, on_channel, &pair);
    rtc_data_channel_t *a = rtc_dc_manager_create_channel(&pair.a, "server channel", NULL);
    ASSERT(a != NULL);
    ASSERT_EQ(rtc_dc_manager_connect(&pair.a, false), RTC_OK);
    ASSERT_EQ(rtc_dc_manager_connect(&pair.b, true), RTC_OK);
    ASSERT(pump_until(&pair, a, 0, 5000));
    ASSERT_EQ(rtc_data_channel_id(a) % 2, 1);
    ASSERT(pair.remote != NULL);
    close_pair(&pair);
}

int main(void) {
    if (rtc_client_init() != RTC_OK)
        return 1;
    RUN_TEST(sctp_dcep_loss_fragmentation_and_close);
    RUN_TEST(sctp_negotiated_options_and_validation);
    RUN_TEST(sctp_dtls_server_allocates_odd_dcep_stream);
    rtc_client_cleanup();
    TEST_SUMMARY();
}
