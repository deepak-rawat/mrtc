#include "meeting.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>

static unsigned prepares, commits, aborts, retires;
static sig_msg_t last;

static void capture(meeting_peer_t *peer, const char *json, void *user) {
    (void)peer;
    (void)user;
    if (sig_msg_parse(&last, json, strlen(json)) != 0)
        return;
    prepares += last.type == SIG_MSG_TOPOLOGY_PREPARE;
    commits += last.type == SIG_MSG_TOPOLOGY_COMMIT;
    aborts += last.type == SIG_MSG_TOPOLOGY_ABORT;
}

static void retire(const char *room, const char *peer, uint32_t epoch, void *user) {
    (void)room;
    (void)peer;
    (void)epoch;
    (void)user;
    retires++;
}

static void setup(meeting_manager_t *manager) {
    meeting_manager_init(manager, capture, NULL);
    manager->policy = MEETING_MODE_AUTO;
    manager->downgrade_delay_ms = 250;
    manager->transition_timeout_ms = 100;
    manager->retire_fn = retire;
    prepares = commits = aborts = retires = 0;
}

static int join(meeting_manager_t *manager, meeting_peer_t *peer, const char *room, uint64_t now) {
    sig_msg_t message = {
        .type = SIG_MSG_JOIN, .version = 2, .can_mesh = true, .can_sfu = true, .can_switch = true};
    snprintf(message.meeting, sizeof(message.meeting), "%s", room);
    return meeting_join_message(manager, peer, &message, now);
}

static int respond(meeting_manager_t *manager, meeting_peer_t *peer, sig_msg_type_t type,
                   uint64_t now) {
    meeting_t *room = &manager->meetings[peer->meeting_idx];
    sig_msg_t message = {.type = type, .version = 2, .epoch = room->pending_epoch};
    snprintf(message.mode, sizeof(message.mode), "%s",
             room->desired_mode == MEETING_MODE_MESH ? "mesh" : "sfu");
    return meeting_handle_topology(manager, peer, &message, now);
}

TEST(independent_rooms_upgrade_and_delayed_downgrade) {
    meeting_manager_t manager;
    setup(&manager);
    uint64_t now = rtc_time_ms();
    meeting_peer_t *a = meeting_peer_create(NULL), *b = meeting_peer_create(NULL),
                   *c = meeting_peer_create(NULL), *d = meeting_peer_create(NULL);
    ASSERT_EQ(join(&manager, a, "one", now), 0);
    ASSERT_EQ(join(&manager, b, "one", now), 0);
    ASSERT_EQ(join(&manager, d, "two", now), 0);
    meeting_t *room = &manager.meetings[a->meeting_idx];
    meeting_t *other = &manager.meetings[d->meeting_idx];
    ASSERT_EQ(room->mode, MEETING_MODE_MESH);
    ASSERT_EQ(prepares, 0);
    ASSERT_EQ(join(&manager, c, "one", now), 0);
    ASSERT_EQ(room->phase, MEETING_PREPARING);
    ASSERT_EQ(room->pending_epoch, 2);
    ASSERT_EQ(prepares, 3);
    ASSERT_EQ(other->phase, MEETING_STABLE);
    ASSERT_EQ(other->epoch, 1);
    ASSERT_EQ(respond(&manager, a, SIG_MSG_TOPOLOGY_READY, now), 0);
    ASSERT_EQ(respond(&manager, b, SIG_MSG_TOPOLOGY_READY, now), 0);
    ASSERT_EQ(commits, 0);
    ASSERT_EQ(respond(&manager, c, SIG_MSG_TOPOLOGY_READY, now), 0);
    ASSERT_EQ(commits, 3);
    ASSERT_EQ(room->mode, MEETING_MODE_SFU);
    ASSERT_EQ(room->phase, MEETING_COMMITTING);
    ASSERT_EQ(respond(&manager, a, SIG_MSG_TOPOLOGY_ACK, now), 0);
    ASSERT_EQ(respond(&manager, b, SIG_MSG_TOPOLOGY_ACK, now), 0);
    ASSERT_EQ(respond(&manager, c, SIG_MSG_TOPOLOGY_ACK, now), 0);
    ASSERT_EQ(room->phase, MEETING_STABLE);
    ASSERT_EQ(retires, 3);
    meeting_leave(&manager, c);
    ASSERT_EQ(room->phase, MEETING_STABLE);
    uint64_t due = room->downgrade_at_ms;
    ASSERT(due > 0);
    meeting_tick(&manager, due - 1);
    ASSERT_EQ(room->phase, MEETING_STABLE);
    meeting_tick(&manager, due);
    ASSERT_EQ(room->phase, MEETING_PREPARING);
    ASSERT_EQ(room->desired_mode, MEETING_MODE_MESH);
    ASSERT_EQ(room->pending_epoch, 3);
    ASSERT_EQ(respond(&manager, a, SIG_MSG_TOPOLOGY_READY, due), 0);
    ASSERT_EQ(respond(&manager, b, SIG_MSG_TOPOLOGY_READY, due), 0);
    ASSERT_EQ(respond(&manager, a, SIG_MSG_TOPOLOGY_ACK, due), 0);
    ASSERT_EQ(respond(&manager, b, SIG_MSG_TOPOLOGY_ACK, due), 0);
    ASSERT_EQ(room->mode, MEETING_MODE_MESH);
    ASSERT_EQ(room->epoch, 3);
    meeting_peer_destroy(&manager, a);
    meeting_peer_destroy(&manager, b);
    meeting_peer_destroy(&manager, c);
    meeting_peer_destroy(&manager, d);
    meeting_manager_close(&manager);
}

TEST(failure_timeout_join_and_stale_sessions) {
    meeting_manager_t manager;
    setup(&manager);
    uint64_t now = rtc_time_ms();
    meeting_peer_t *a = meeting_peer_create(NULL), *b = meeting_peer_create(NULL),
                   *c = meeting_peer_create(NULL), *d = meeting_peer_create(NULL);
    ASSERT_EQ(join(&manager, a, "room", now), 0);
    ASSERT_EQ(join(&manager, b, "room", now), 0);
    ASSERT_EQ(join(&manager, c, "room", now), 0);
    meeting_t *room = &manager.meetings[a->meeting_idx];
    uint32_t old_pending = room->pending_epoch;
    ASSERT_EQ(join(&manager, d, "room", now), 0);
    ASSERT_EQ(aborts, 3);
    ASSERT(room->pending_epoch > old_pending);
    sig_msg_t stale = {.type = SIG_MSG_CANDIDATE, .version = 2, .epoch = old_pending, .to = "sfu"};
    ASSERT_EQ(sig_session_id(stale.session_id, SIG_SESSION_PUBLISH, stale.epoch, a->id, NULL), 0);
    ASSERT_EQ(meeting_validate_session(&manager, a, &stale), -1);
    stale.epoch = room->pending_epoch;
    ASSERT_EQ(sig_session_id(stale.session_id, SIG_SESSION_PUBLISH, stale.epoch, b->id, NULL), 0);
    ASSERT_EQ(meeting_validate_session(&manager, a, &stale), -1);
    ASSERT_EQ(sig_session_id(stale.session_id, SIG_SESSION_PUBLISH, stale.epoch, a->id, NULL), 0);
    ASSERT_EQ(meeting_validate_session(&manager, a, &stale), MEETING_MODE_SFU);
    ASSERT_EQ(respond(&manager, a, SIG_MSG_TOPOLOGY_READY, now), 0);
    meeting_tick(&manager, now + 101);
    ASSERT_EQ(room->phase, MEETING_STABLE);
    ASSERT_EQ(room->mode, MEETING_MODE_MESH);
    ASSERT_EQ(room->epoch, 1);
    unsigned count = prepares;
    meeting_tick(&manager, now + 100000);
    ASSERT_EQ(prepares, count);
    sig_msg_t request = {.type = SIG_MSG_TOPOLOGY_REQUEST,
                         .version = 2,
                         .epoch = 1,
                         .mode = "sfu",
                         .reason = "ICE failed"};
    ASSERT_EQ(meeting_handle_topology(&manager, a, &request, now), 0);
    ASSERT(room->mesh_failed);
    ASSERT_EQ(room->phase, MEETING_PREPARING);
    ASSERT_EQ(respond(&manager, a, SIG_MSG_TOPOLOGY_ABORT, now), 0);
    ASSERT_EQ(room->mode, MEETING_MODE_MESH);
    ASSERT_EQ(meeting_handle_topology(&manager, a, &request, now), -1);
    meeting_peer_destroy(&manager, d);
    meeting_peer_destroy(&manager, c);
    meeting_peer_destroy(&manager, b);
    meeting_peer_destroy(&manager, a);
    meeting_manager_close(&manager);
}

TEST(forced_modes_and_legacy_policy) {
    for (int mode = MEETING_MODE_MESH; mode <= MEETING_MODE_SFU; mode++) {
        meeting_manager_t manager;
        setup(&manager);
        manager.policy = (meeting_mode_t)mode;
        meeting_peer_t *peers[3];
        for (int i = 0; i < 3; i++) {
            peers[i] = meeting_peer_create(NULL);
            ASSERT_EQ(meeting_join(&manager, peers[i], "fixed"), 0);
        }
        meeting_t *room = &manager.meetings[peers[0]->meeting_idx];
        ASSERT_EQ(room->mode, (meeting_mode_t)mode);
        ASSERT_EQ(room->phase, MEETING_STABLE);
        ASSERT_EQ(prepares, 0);
        meeting_peer_t *version2 = meeting_peer_create(NULL);
        ASSERT_EQ(join(&manager, version2, "fixed", rtc_time_ms()), -1);
        ASSERT_EQ(version2->meeting_idx, -1);
        meeting_peer_destroy(&manager, version2);
        for (int i = 0; i < 3; i++)
            meeting_peer_destroy(&manager, peers[i]);
        meeting_manager_close(&manager);
    }
    meeting_manager_t manager;
    setup(&manager);
    meeting_peer_t *peer = meeting_peer_create(NULL);
    ASSERT_EQ(meeting_join(&manager, peer, "auto"), -1);
    ASSERT_EQ(peer->meeting_idx, -1);
    sig_msg_t join_message = {
        .type = SIG_MSG_JOIN, .version = 2, .meeting = "auto", .can_mesh = true, .can_sfu = true};
    ASSERT_EQ(meeting_join_message(&manager, peer, &join_message, rtc_time_ms()), -1);
    meeting_peer_destroy(&manager, peer);
    meeting_manager_close(&manager);
}

TEST(commit_joins_departures_and_ack_timeout) {
    meeting_manager_t manager;
    setup(&manager);
    uint64_t now = rtc_time_ms();
    meeting_peer_t *peers[4];
    for (int i = 0; i < 4; i++)
        peers[i] = meeting_peer_create(NULL);
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(join(&manager, peers[i], "commit", now), 0);
    meeting_t *room = &manager.meetings[peers[0]->meeting_idx];
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(respond(&manager, peers[i], SIG_MSG_TOPOLOGY_READY, now), 0);
    ASSERT_EQ(room->phase, MEETING_COMMITTING);
    ASSERT_EQ(join(&manager, peers[3], "commit", now), 0);
    ASSERT(peers[3]->ack);
    ASSERT_EQ(peers[3]->version, 2);
    ASSERT_EQ(respond(&manager, peers[0], SIG_MSG_TOPOLOGY_ACK, now), 0);
    meeting_leave(&manager, peers[2]);
    ASSERT_EQ(room->phase, MEETING_COMMITTING);
    meeting_tick(&manager, room->deadline_ms);
    ASSERT(peers[1]->protocol_failed);
    ASSERT(!peers[0]->protocol_failed);
    ASSERT(!peers[3]->protocol_failed);
    ASSERT_EQ(room->mode, MEETING_MODE_SFU);
    ASSERT_EQ(room->epoch, 2);
    ASSERT_EQ(room->phase, MEETING_STABLE);
    for (int i = 0; i < 4; i++)
        meeting_peer_destroy(&manager, peers[i]);
    meeting_manager_close(&manager);
}

TEST(rejoin_rejects_previous_call_session) {
    meeting_manager_t manager;
    setup(&manager);
    manager.policy = MEETING_MODE_SFU;
    uint64_t now = rtc_time_ms();
    meeting_peer_t *peer = meeting_peer_create(NULL);
    ASSERT_EQ(join(&manager, peer, "old", now), 0);
    sig_msg_t previous = {.type = SIG_MSG_CANDIDATE, .version = 2, .epoch = 1, .to = "sfu"};
    ASSERT_EQ(sig_session_id(previous.session_id, SIG_SESSION_PUBLISH, 1, peer->id, NULL), 0);
    char previous_id[MEETING_PEER_ID_LEN];
    snprintf(previous_id, sizeof(previous_id), "%s", peer->id);
    ASSERT_EQ(join(&manager, peer, "new", now), 0);
    ASSERT(strcmp(previous_id, peer->id) != 0);
    ASSERT_EQ(meeting_validate_session(&manager, peer, &previous), -1);
    ASSERT_EQ(sig_session_id(previous.session_id, SIG_SESSION_PUBLISH, 1, peer->id, NULL), 0);
    ASSERT_EQ(meeting_validate_session(&manager, peer, &previous), MEETING_MODE_SFU);
    meeting_peer_destroy(&manager, peer);
    meeting_manager_close(&manager);
}

int main(void) {
    RUN_TEST(independent_rooms_upgrade_and_delayed_downgrade);
    RUN_TEST(failure_timeout_join_and_stale_sessions);
    RUN_TEST(forced_modes_and_legacy_policy);
    RUN_TEST(commit_joins_departures_and_ack_timeout);
    RUN_TEST(rejoin_rejects_previous_call_session);
    TEST_SUMMARY();
    return _test_fail_count ? 1 : 0;
}
