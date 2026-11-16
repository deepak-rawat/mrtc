#include "../src/conference.c"
#include "test_harness.h"

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

TEST(peer_slots_reused_across_churn) {
    conference_config_t config = {0};
    conference_t *conference = conference_create(&config);
    ASSERT(conference != NULL);
    for (int generation = 0; generation < 120; generation++) {
        char id[32];
        snprintf(id, sizeof(id), "peer-%d", generation);
        conf_peer_t *peer = alloc_peer(conference, id);
        ASSERT(peer != NULL);
        ASSERT_EQ(conference_get_peer_count(conference), 1);
        ASSERT(find_peer(conference, id) == peer);
        destroy_peer(conference, peer);
        ASSERT(find_peer(conference, id) == NULL);
        ASSERT_EQ(conference_get_peer_count(conference), 0);
    }
    conference_destroy(conference);
}

TEST(peer_slots_survive_holes_and_duplicate_ids) {
    conference_config_t config = {0};
    conference_t *conference = conference_create(&config);
    ASSERT(conference != NULL);
    conf_peer_t *alice = alloc_peer(conference, "alice");
    conf_peer_t *bob = alloc_peer(conference, "bob");
    ASSERT(alice != NULL && bob != NULL);
    ASSERT(alloc_peer(conference, "bob") == bob);
    destroy_peer(conference, alice);
    conf_peer_t *charlie = alloc_peer(conference, "charlie");
    ASSERT(charlie == alice);
    ASSERT(find_peer(conference, "bob") == bob);
    ASSERT_EQ(conference_get_peer_count(conference), 2);
    conference_leave(conference);
    ASSERT_EQ(conference_get_peer_count(conference), 0);
    conference_destroy(conference);
}

static void count_departure(const char *peer_id, void *user) {
    ASSERT(strcmp(peer_id, "sfu") != 0);
    (*(int *)user)++;
}

TEST(disconnect_removes_remote_participants) {
    int departures = 0;
    conference_config_t config = {.callbacks = {.on_peer_left = count_departure},
                                  .user_data = &departures};
    conference_t *conference = conference_create(&config);
    ASSERT(conference != NULL);
    conference->sfu_mode = true;
    ASSERT(alloc_peer(conference, "sfu") != NULL);
    ASSERT(alloc_peer(conference, "alice") != NULL);
    ASSERT(alloc_peer(conference, "bob") != NULL);
    sig_on_disconnected(conference);
    ASSERT_EQ(conference_get_peer_count(conference), 0);
    ASSERT_EQ(departures, 2);
    conference_destroy(conference);
}

TEST(sfu_no_source_offer_is_data_only) {
    conference_config_t config = {0};
    conference_t *conference = conference_create(&config);
    ASSERT(conference != NULL);
    conference->sfu_mode = true;
    conf_peer_t *publisher = alloc_peer(conference, "sfu");
    ASSERT(publisher != NULL);
    rtc_peer_connection_t *pc = create_pc(conference, publisher);
    ASSERT(pc != NULL);
    ASSERT(create_pc(conference, publisher) == pc);
    ASSERT(rtc_peer_connection_create_data_channel(pc, "chat", NULL) != NULL);
    rtc_desc_t offer;
    ASSERT_EQ(rtc_peer_connection_create_offer(pc, &offer), RTC_OK);
    rtc_sdp_t sdp = {0};
    ASSERT_EQ(rtc_sdp_parse(&sdp, offer.sdp, offer.sdp_len), RTC_OK);
    ASSERT_EQ(sdp.media_count, 1);
    ASSERT_EQ(sdp.media[0].media_type, RTC_MEDIA_APPLICATION);
    rtc_sdp_close(&sdp);
    conference_destroy(conference);
}

typedef struct {
    conference_t *conference;
    _Atomic bool stop;
    _Atomic unsigned sent;
    _Atomic unsigned replies;
    _Atomic unsigned errors;
} send_race_t;

typedef struct {
    rtc_data_channel_t *channel;
} echo_t;

static void echo_request(const uint8_t *data, size_t len, void *user) {
    echo_t *echo = user;
    if (len == 7 && memcmp(data, "request", len) == 0)
        rtc_data_channel_send(echo->channel, (const uint8_t *)"from-worker", 11);
}

static void echo_channel(rtc_data_channel_t *channel, void *user) {
    echo_t *echo = user;
    echo->channel = channel;
    rtc_data_channel_on_message(channel, echo_request, echo);
}

static void reply_from_callback(const char *peer, const uint8_t *data, size_t len, void *user) {
    send_race_t *race = user;
    if (len != 11 || memcmp(data, "from-worker", len) != 0)
        return;
    int rc = conference_send_message(race->conference, peer, (const uint8_t *)"reply", 5);
    if (rc == RTC_OK)
        atomic_fetch_add(&race->replies, 1);
    else if (rc != RTC_ERR_INVALID && rc != RTC_ERR_NOMEM)
        atomic_fetch_add(&race->errors, 1);
}

static void *send_during_leave(void *user) {
    send_race_t *race = user;
    while (!atomic_load(&race->stop)) {
        int rc = conference_send_message(race->conference, "bob", (const uint8_t *)"request", 7);
        if (rc == RTC_OK)
            atomic_fetch_add(&race->sent, 1);
        else if (rc != RTC_ERR_INVALID && rc != RTC_ERR_NOMEM)
            atomic_fetch_add(&race->errors, 1);
        SLEEP_MS(1);
    }
    return NULL;
}

TEST(data_channel_send_and_callback_reply_race_peer_leave) {
    send_race_t race = {0};
    conference_config_t config = {.callbacks = {.on_message = reply_from_callback},
                                  .user_data = &race};
    conference_t *conference = conference_create(&config);
    ASSERT(conference != NULL);
    race.conference = conference;
    for (int generation = 0; generation < 16; generation++) {
        conf_peer_t *peer = alloc_peer(conference, "bob");
        ASSERT(peer != NULL);
        rtc_config_t first = {.stun_server = "127.0.0.1", .stun_port = 9};
        rtc_config_t second = {.stun_server = "127.0.0.1", .stun_port = 10};
        peer->pc = rtc_peer_connection_create(&first);
        rtc_peer_connection_t *remote = rtc_peer_connection_create(&second);
        ASSERT(peer->pc && remote);
        echo_t echo = {0};
        rtc_peer_connection_on_data_channel(remote, echo_channel, &echo);
        rtc_data_channel_t *channel =
            rtc_peer_connection_create_data_channel(peer->pc, "leave-race", NULL);
        ASSERT(channel != NULL);
        peer->dc = channel;
        rtc_data_channel_on_open(channel, on_dc_open, peer);
        rtc_data_channel_on_message(channel, on_dc_message, peer);
        rtc_desc_t offer, answer;
        ASSERT_EQ(rtc_peer_connection_create_offer(peer->pc, &offer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_set_local_desc(peer->pc, &offer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_set_remote_desc(remote, &offer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_create_answer(remote, &answer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_set_local_desc(remote, &answer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_set_remote_desc(peer->pc, &answer), RTC_OK);
        uint64_t deadline = rtc_time_ms() + 3000;
        while (!peer->dc_open && rtc_time_ms() < deadline)
            SLEEP_MS(1);
        ASSERT(peer->dc_open);
        atomic_store(&race.stop, false);
        unsigned sent = atomic_load(&race.sent);
        unsigned replied = atomic_load(&race.replies);
        rtc_thread_t sender;
        ASSERT_EQ(rtc_thread_create(&sender, send_during_leave, &race), RTC_OK);
        deadline = rtc_time_ms() + 3000;
        while ((atomic_load(&race.sent) < sent + 20 || atomic_load(&race.replies) <= replied) &&
               rtc_time_ms() < deadline)
            SLEEP_MS(1);
        destroy_peer(conference, peer);
        atomic_store(&race.stop, true);
        ASSERT_EQ(rtc_thread_join(&sender), RTC_OK);
        ASSERT(atomic_load(&race.sent) >= sent + 20);
        ASSERT(atomic_load(&race.replies) > replied);
        ASSERT_EQ(atomic_load(&race.errors), 0);
        ASSERT_EQ(peer->in_flight_sends, 0);
        rtc_peer_connection_close(remote);
        rtc_peer_connection_destroy(remote);
    }
    conference_destroy(conference);
}

int main(void) {
    rtc_client_init();
    RUN_TEST(peer_slots_reused_across_churn);
    RUN_TEST(peer_slots_survive_holes_and_duplicate_ids);
    RUN_TEST(disconnect_removes_remote_participants);
    RUN_TEST(sfu_no_source_offer_is_data_only);
    RUN_TEST(data_channel_send_and_callback_reply_race_peer_leave);
    rtc_client_cleanup();
    TEST_SUMMARY();
}
