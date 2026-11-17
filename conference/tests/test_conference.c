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

TEST(sfu_media_offer_accepts_conference_stun_settings) {
    const char *servers[] = {"localhost", "127.0.0.1", "stun:127.0.0.1:9", ""};
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        conference_config_t config = {.stun_server = servers[i]};
        conference_t *conference = conference_create(&config);
        ASSERT(conference != NULL);
        conference->sfu_mode = true;
        ASSERT_EQ(conference_add_video_source(conference, "camera", 160, 120, 30, 160), RTC_OK);
        ASSERT_EQ(conference_add_audio_source(conference, "mic", 48000, 1, 32000), RTC_OK);
        conf_peer_t *publisher = alloc_peer(conference, "sfu");
        ASSERT(publisher != NULL);
        rtc_peer_connection_t *pc = create_pc(conference, publisher);
        ASSERT(pc != NULL);
        ASSERT(rtc_peer_connection_create_data_channel(pc, "chat", NULL) != NULL);
        rtc_desc_t offer;
        ASSERT_EQ(rtc_peer_connection_create_offer(pc, &offer), RTC_OK);
        ASSERT(strstr(offer.sdp, "m=video") != NULL);
        ASSERT(strstr(offer.sdp, "m=audio") != NULL);
        ASSERT(strstr(offer.sdp, "m=application") != NULL);
        conference_destroy(conference);
    }
}

TEST(mesh_answer_adopts_remote_mid_order_and_payload_types) {
    conference_config_t config = {0};
    conference_t *c = conference_create(&config);
    ASSERT(c != NULL);
    ASSERT_EQ(conference_add_video_source(c, "camera", 160, 120, 10, 160), RTC_OK);
    ASSERT_EQ(conference_add_audio_source(c, "mic", 48000, 1, 32000), RTC_OK);
    c->v2 = true;
    c->active_epoch = 1;
    c->highest_epoch = 1;
    snprintf(c->my_id, sizeof(c->my_id), "alice");
    participant_add(c, "bob");
    conf_peer_t *peer = alloc_session(c, "bob", "m:1:alice:bob", 1, SIG_SESSION_MESH, false);
    ASSERT(peer != NULL);
    rtc_config_t remote_config = {0};
    rtc_peer_connection_t *remote = rtc_peer_connection_create(&remote_config);
    ASSERT(remote != NULL);
    rtc_codec_t opus = {
        .payload_type = 109, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_codec_t vp8 = {.payload_type = 102, .mime_type = "video/VP8", .clock_rate = 90000};
    ASSERT(rtc_peer_connection_add_track(remote, RTC_KIND_AUDIO, &opus) != NULL);
    ASSERT(rtc_peer_connection_add_track(remote, RTC_KIND_VIDEO, &vp8) != NULL);
    ASSERT(rtc_peer_connection_create_data_channel(remote, "chat", NULL) != NULL);
    rtc_desc_t offer;
    ASSERT_EQ(rtc_peer_connection_create_offer(remote, &offer), RTC_OK);
    rtc_sdp_t offered = {0};
    ASSERT_EQ(rtc_sdp_parse(&offered, offer.sdp, offer.sdp_len), RTC_OK);
    for (int i = 0; i < offered.media_count; i++) {
        rtc_sdp_media_t *media = &offered.media[i];
        for (int j = 0; j < media->extmap_count; j++)
            if (strstr(media->extmaps[j].uri, "transport-wide-cc"))
                media->extmaps[j].id = 3;
        if (media->media_type == RTC_MEDIA_VIDEO)
            ASSERT_EQ(rtc_sdp_media_add_extmap(media, 5,
                                               "http://www.webrtc.org/experiments/rtp-hdrext/"
                                               "playout-delay"),
                      RTC_OK);
    }
    offered.type = RTC_SDP_OFFER;
    ASSERT_EQ(rtc_sdp_generate(&offered), RTC_OK);
    memcpy(offer.sdp, offered.raw, offered.raw_len + 1);
    offer.sdp_len = offered.raw_len;
    rtc_sdp_close(&offered);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(remote, &offer), RTC_OK);
    /* No signaling client is bound: SDP succeeds, then forwarding fails. */
    ASSERT_EQ(accept_offer(c, peer, offer.sdp), RTC_ERR_INVALID);
    const rtc_desc_t *answer = rtc_peer_connection_local_desc(peer->pc);
    ASSERT(answer != NULL);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(remote, answer), RTC_OK);
    ASSERT(peer->video_sender != NULL && peer->audio_sender != NULL);
    ASSERT_EQ(rtc_rtp_sender_get_codec(peer->video_sender)->payload_type, 102);
    ASSERT_EQ(rtc_rtp_sender_get_codec(peer->audio_sender)->payload_type, 109);
    rtc_sdp_t answered = {0};
    ASSERT_EQ(rtc_sdp_parse(&answered, answer->sdp, answer->sdp_len), RTC_OK);
    for (int i = 0; i < answered.media_count; i++)
        for (int j = 0; j < answered.media[i].extmap_count; j++)
            if (strstr(answered.media[i].extmaps[j].uri, "transport-wide-cc"))
                ASSERT_EQ(answered.media[i].extmaps[j].id, 3);
    rtc_sdp_close(&answered);
    rtc_peer_connection_t *original = peer->pc;
    ASSERT_EQ(rtc_peer_connection_restart_ice(remote), RTC_OK);
    rtc_desc_t restart;
    ASSERT_EQ(rtc_peer_connection_create_offer(remote, &restart), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(remote, &restart), RTC_OK);
    sig_msg_t renegotiate = {.type = SIG_MSG_OFFER, .version = 2, .epoch = 1};
    snprintf(renegotiate.from, sizeof(renegotiate.from), "bob");
    snprintf(renegotiate.to, sizeof(renegotiate.to), "alice");
    snprintf(renegotiate.session_id, sizeof(renegotiate.session_id), "m:1:alice:bob");
    snprintf(renegotiate.sdp, sizeof(renegotiate.sdp), "%s", restart.sdp);
    sig_on_event(&renegotiate, c);
    ASSERT(peer->pc == original);
    ASSERT_STR_EQ(rtc_peer_connection_remote_desc(peer->pc)->sdp, restart.sdp);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(remote, rtc_peer_connection_local_desc(peer->pc)),
              RTC_OK);
    rtc_peer_connection_close(remote);
    rtc_peer_connection_destroy(remote);
    conference_destroy(c);
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
    bool versioned;
    uint64_t next_id;
} echo_t;

static void echo_request(const uint8_t *data, size_t len, void *user) {
    echo_t *echo = user;
    if (echo->versioned) {
        char peer[SIG_MAX_PEER_ID];
        uint64_t id;
        if (sig_dc_read(data, len, peer, &id, &data, &len) != 0)
            return;
    }
    if (len == 7 && memcmp(data, "request", len) == 0) {
        if (echo->versioned) {
            uint8_t wire[64];
            size_t wire_len;
            if (sig_dc_write(wire, sizeof(wire), &wire_len, "bob", ++echo->next_id,
                             (const uint8_t *)"from-worker", 11) == 0)
                rtc_data_channel_send(echo->channel, wire, wire_len);
        } else {
            rtc_data_channel_send(echo->channel, (const uint8_t *)"from-worker", 11);
        }
    }
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

static void run_send_leave_race(bool versioned) {
    send_race_t race = {0};
    conference_config_t config = {.callbacks = {.on_message = reply_from_callback},
                                  .user_data = &race};
    conference_t *conference = conference_create(&config);
    ASSERT(conference != NULL);
    race.conference = conference;
    if (versioned) {
        conference->v2 = true;
        snprintf(conference->my_id, sizeof(conference->my_id), "alice");
        participant_add(conference, "bob");
    }
    for (int generation = 0; generation < 16; generation++) {
        conf_peer_t *peer = alloc_peer(conference, "bob");
        ASSERT(peer != NULL);
        rtc_config_t first = {.stun_server = "127.0.0.1", .stun_port = 9};
        rtc_config_t second = {.stun_server = "127.0.0.1", .stun_port = 10};
        peer->pc = rtc_peer_connection_create(&first);
        rtc_peer_connection_t *remote = rtc_peer_connection_create(&second);
        ASSERT(peer->pc && remote);
        echo_t echo = {.versioned = versioned, .next_id = (uint64_t)generation * 1000};
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

TEST(data_channel_send_and_callback_reply_race_peer_leave) {
    run_send_leave_race(false);
}

TEST(versioned_data_channel_send_and_callback_reply_race_peer_leave) {
    run_send_leave_race(true);
}

typedef struct {
    unsigned video;
    unsigned audio;
    unsigned messages;
    unsigned departures;
    unsigned connected;
} migration_counts_t;

static void migration_video(const char *id, const char *label, const video_frame_t *frame,
                            void *user) {
    (void)id;
    (void)label;
    (void)frame;
    ((migration_counts_t *)user)->video++;
}

static void migration_audio(const char *id, const char *label, const audio_frame_t *frame,
                            void *user) {
    (void)id;
    (void)label;
    (void)frame;
    ((migration_counts_t *)user)->audio++;
}

static void migration_message(const char *id, const uint8_t *data, size_t len, void *user) {
    ASSERT_STR_EQ(id, "bob");
    ASSERT_EQ(len, 4);
    ASSERT(memcmp(data, "test", 4) == 0);
    ((migration_counts_t *)user)->messages++;
}

static void migration_departure(const char *id, void *user) {
    (void)id;
    ((migration_counts_t *)user)->departures++;
}

static void migration_connected(const char *id, void *user) {
    ASSERT_STR_EQ(id, "bob");
    ((migration_counts_t *)user)->connected++;
}

static conference_t *migration_conference(migration_counts_t *counts) {
    conference_config_t config = {.callbacks = {.on_remote_video = migration_video,
                                                .on_remote_audio = migration_audio,
                                                .on_message = migration_message,
                                                .on_connected = migration_connected,
                                                .on_peer_left = migration_departure},
                                  .user_data = counts};
    conference_t *c = conference_create(&config);
    if (!c)
        return NULL;
    c->v2 = true;
    c->active_epoch = 1;
    c->highest_epoch = 1;
    snprintf(c->my_id, sizeof(c->my_id), "alice");
    participant_add(c, "bob");
    return c;
}

TEST(staged_graph_commit_preserves_participants_sources_and_selects_media_once) {
    migration_counts_t counts = {0};
    conference_t *c = migration_conference(&counts);
    ASSERT(c != NULL);
    ASSERT_EQ(conference_add_audio_source(c, "mic", 48000, 1, 32000), RTC_OK);
    media_send_stream_t *encoder = c->sources[0].stream;
    conf_peer_t *old = alloc_session(c, "bob", "m:1:alice:bob", 1, SIG_SESSION_MESH, false);
    conf_peer_t *pending = alloc_session(c, "bob", "s:2:alice:bob", 2, SIG_SESSION_SUBSCRIBE, true);
    ASSERT(old && pending && old != pending);
    on_conn_state(RTC_CONNECTION_CONNECTED, old);
    on_conn_state(RTC_CONNECTION_CONNECTED, old);
    on_conn_state(RTC_CONNECTION_CONNECTED, pending);
    ASSERT_EQ(counts.connected, 1);
    c->pending_epoch = 2;
    c->pending_sfu = true;
    c->ready_sent = true;
    audio_frame_t audio = {0};
    video_frame_t video = {0};
    pipeline_on_video("bob", "camera", &video, old);
    pipeline_on_audio("bob", "mic", &audio, old);
    pipeline_on_video("bob", "camera", &video, pending);
    pipeline_on_audio("bob", "mic", &audio, pending);
    ASSERT_EQ(counts.video, 1);
    ASSERT_EQ(counts.audio, 1);
    sig_msg_t commit = {.type = SIG_MSG_TOPOLOGY_COMMIT, .version = 2, .epoch = 2};
    snprintf(commit.mode, sizeof(commit.mode), "sfu");
    sig_on_event(&commit, c);
    ASSERT_EQ(c->active_epoch, 2);
    ASSERT(!old->active);
    ASSERT(pending->active);
    pipeline_on_audio("bob", "mic", &audio, old);
    pipeline_on_video("bob", "camera", &video, pending);
    pipeline_on_audio("bob", "mic", &audio, pending);
    ASSERT_EQ(counts.video, 2);
    ASSERT_EQ(counts.audio, 2);
    ASSERT_EQ(counts.departures, 0);
    ASSERT_EQ(counts.connected, 1);
    ASSERT_EQ(conference_get_peer_count(c), 1);
    ASSERT(c->sources[0].stream == encoder);
    conference_source_stats_t stats;
    ASSERT_EQ(conference_get_source_stats(c, "bob", &stats), RTC_OK);
    ASSERT_EQ(stats.video_frames, 2);
    ASSERT_EQ(stats.audio_frames, 2);
    conference_destroy(c);
}

TEST(staged_graph_abort_and_stale_events_preserve_active_graph) {
    migration_counts_t counts = {0};
    conference_t *c = migration_conference(&counts);
    ASSERT(c != NULL);
    conf_peer_t *old = alloc_session(c, "bob", "m:1:alice:bob", 1, SIG_SESSION_MESH, false);
    conf_peer_t *pending = alloc_session(c, "bob", "s:2:alice:bob", 2, SIG_SESSION_SUBSCRIBE, true);
    ASSERT(old && pending);
    c->pending_epoch = 2;
    c->highest_epoch = 2;
    c->pending_sfu = true;
    sig_msg_t abort = {.type = SIG_MSG_TOPOLOGY_ABORT, .version = 2, .epoch = 2};
    snprintf(abort.mode, sizeof(abort.mode), "sfu");
    snprintf(abort.reason, sizeof(abort.reason), "test cancellation");
    sig_on_event(&abort, c);
    ASSERT(old->active);
    ASSERT(!pending->active);
    ASSERT_EQ(c->active_epoch, 1);
    sig_msg_t stale = {.type = SIG_MSG_TOPOLOGY_COMMIT, .version = 2, .epoch = 2};
    sig_on_event(&stale, c);
    stale.type = SIG_MSG_OFFER;
    snprintf(stale.session_id, sizeof(stale.session_id), "s:2:alice:bob");
    snprintf(stale.from, sizeof(stale.from), "bob");
    sig_on_event(&stale, c);
    ASSERT_EQ(c->peer_count, 1);
    stale.type = SIG_MSG_TOPOLOGY_PREPARE;
    stale.peer_count = 1;
    snprintf(stale.mode, sizeof(stale.mode), "sfu");
    snprintf(stale.peers[0], sizeof(stale.peers[0]), "bob");
    sig_on_event(&stale, c);
    ASSERT_EQ(c->pending_epoch, 0);
    ASSERT_EQ(counts.departures, 0);
    conference_destroy(c);
}

TEST(pending_readiness_requires_transport_and_recent_media_not_muted_frames) {
    migration_counts_t counts = {0};
    conference_t *c = migration_conference(&counts);
    ASSERT(c != NULL);
    conf_peer_t *old = alloc_session(c, "bob", "m:1:alice:bob", 1, SIG_SESSION_MESH, false);
    conf_peer_t *publisher = alloc_session(c, "sfu", "p:2:alice", 2, SIG_SESSION_PUBLISH, true);
    ASSERT(old && publisher);
    c->pending_sfu = true;
    ASSERT(!graph_ready_locked(c, 2));
    publisher->connected = true;
    publisher->dc_open = true;
    /* A data-only participant needs no subscription or decoded frames. */
    ASSERT(graph_ready_locked(c, 2));
    old->last_video_ms = rtc_time_ms();
    ASSERT(!graph_ready_locked(c, 2));
    conf_peer_t *subscription =
        alloc_session(c, "bob", "s:2:alice:bob", 2, SIG_SESSION_SUBSCRIBE, true);
    ASSERT(subscription != NULL);
    subscription->connected = true;
    ASSERT(!graph_ready_locked(c, 2));
    subscription->video_frames = 1;
    ASSERT(graph_ready_locked(c, 2));
    old->last_audio_ms = rtc_time_ms();
    ASSERT(!graph_ready_locked(c, 2));
    /* A muted source must not stall migration indefinitely. */
    old->last_audio_ms = rtc_time_ms() - 2000;
    ASSERT(graph_ready_locked(c, 2));
    conference_destroy(c);
}

TEST(versioned_data_deduplicates_both_graphs_and_rejects_malformed_envelopes) {
    migration_counts_t counts = {0};
    conference_t *c = migration_conference(&counts);
    ASSERT(c != NULL);
    conf_peer_t *old = alloc_session(c, "bob", "m:1:alice:bob", 1, SIG_SESSION_MESH, false);
    conf_peer_t *pending = alloc_session(c, "bob", "s:2:alice:bob", 2, SIG_SESSION_SUBSCRIBE, true);
    ASSERT(old && pending);
    uint8_t wire[64];
    size_t len;
    ASSERT_EQ(sig_dc_write(wire, sizeof(wire), &len, "bob", 1, (const uint8_t *)"test", 4), 0);
    on_dc_message(wire, len, old);
    on_dc_message(wire, len, pending);
    ASSERT_EQ(counts.messages, 1);
    ASSERT_EQ(sig_dc_write(wire, sizeof(wire), &len, "bob", 2, (const uint8_t *)"test", 4), 0);
    on_dc_message(wire, len, pending);
    on_dc_message(wire, len, old);
    ASSERT_EQ(counts.messages, 2);
    on_dc_message((const uint8_t *)"test", 4, old);
    ASSERT_EQ(counts.messages, 2);
    conference_destroy(c);
}

int main(void) {
    rtc_client_init();
    RUN_TEST(peer_slots_reused_across_churn);
    RUN_TEST(peer_slots_survive_holes_and_duplicate_ids);
    RUN_TEST(disconnect_removes_remote_participants);
    RUN_TEST(sfu_no_source_offer_is_data_only);
    RUN_TEST(sfu_media_offer_accepts_conference_stun_settings);
    RUN_TEST(mesh_answer_adopts_remote_mid_order_and_payload_types);
    RUN_TEST(data_channel_send_and_callback_reply_race_peer_leave);
    RUN_TEST(versioned_data_channel_send_and_callback_reply_race_peer_leave);
    RUN_TEST(staged_graph_commit_preserves_participants_sources_and_selects_media_once);
    RUN_TEST(staged_graph_abort_and_stale_events_preserve_active_graph);
    RUN_TEST(pending_readiness_requires_transport_and_recent_media_not_muted_frames);
    RUN_TEST(versioned_data_deduplicates_both_graphs_and_rejects_malformed_envelopes);
    rtc_client_cleanup();
    TEST_SUMMARY();
}
