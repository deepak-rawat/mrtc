/*
 * Tests for meeting management logic (no network).
 */
#include "test_harness.h"
#include "meeting.h"
#include "signaling/signaling_msg.h"
#include <rtc/rtc_common.h>
#include <string.h>
#include <stdlib.h>

#define MAX_SENT 64
static struct {
    char *msgs[MAX_SENT];
    void *targets[MAX_SENT];
    int count;
} g_sent;

static void mock_send(meeting_peer_t *peer, const char *json, void *user) {
    (void)user;
    if (g_sent.count < MAX_SENT) {
        g_sent.msgs[g_sent.count] = strdup(json);
        g_sent.targets[g_sent.count] = peer;
        g_sent.count++;
    }
}

static void clear_sent(void) {
    for (int i = 0; i < g_sent.count; i++)
        free(g_sent.msgs[i]);
    memset(&g_sent, 0, sizeof(g_sent));
}

static int count_msgs_to(meeting_peer_t *peer) {
    int n = 0;
    for (int i = 0; i < g_sent.count; i++)
        if (g_sent.targets[i] == peer)
            n++;
    return n;
}

static char *find_msg_to(meeting_peer_t *peer, const char *type_str) {
    for (int i = 0; i < g_sent.count; i++) {
        if (g_sent.targets[i] == peer && strstr(g_sent.msgs[i], type_str))
            return g_sent.msgs[i];
    }
    return NULL;
}

TEST(meeting_create_destroy) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);

    meeting_peer_t *p = meeting_peer_create((void *)1);
    ASSERT(p != NULL);
    ASSERT(strlen(p->id) > 0);
    ASSERT_EQ(p->meeting_idx, -1);

    meeting_peer_destroy(&mgr, p);
    printf("    create/destroy OK\n");
    clear_sent();
}

TEST(meeting_join_leave) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);
    clear_sent();

    meeting_peer_t *a = meeting_peer_create((void *)1);
    meeting_peer_t *b = meeting_peer_create((void *)2);
    meeting_peer_t *c = meeting_peer_create((void *)3);

    /* A joins */
    ASSERT_EQ(meeting_join(&mgr, a, "test"), 0);
    ASSERT(a->meeting_idx >= 0);
    /* A should receive "joined" with empty peer list */
    ASSERT(find_msg_to(a, "\"joined\"") != NULL);

    clear_sent();

    /* B joins */
    ASSERT_EQ(meeting_join(&mgr, b, "test"), 0);
    /* B gets "joined" with A in peer list */
    char *b_joined = find_msg_to(b, "\"joined\"");
    ASSERT(b_joined != NULL);
    ASSERT(strstr(b_joined, a->id) != NULL);
    /* A gets "peer_joined" with B's id */
    char *a_notif = find_msg_to(a, "\"peer_joined\"");
    ASSERT(a_notif != NULL);
    ASSERT(strstr(a_notif, b->id) != NULL);

    clear_sent();

    /* C joins */
    ASSERT_EQ(meeting_join(&mgr, c, "test"), 0);

    clear_sent();

    /* B leaves */
    meeting_leave(&mgr, b);
    ASSERT_EQ(b->meeting_idx, -1);
    /* A and C should get "peer_left" */
    ASSERT(find_msg_to(a, "\"peer_left\"") != NULL);
    ASSERT(find_msg_to(c, "\"peer_left\"") != NULL);

    meeting_peer_destroy(&mgr, a);
    meeting_peer_destroy(&mgr, b);
    meeting_peer_destroy(&mgr, c);
    clear_sent();
    printf("    join/leave with 3 peers OK\n");
}

TEST(meeting_full) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);
    clear_sent();

    meeting_peer_t *peers[MEETING_MAX_PEERS + 1];
    for (int i = 0; i <= MEETING_MAX_PEERS; i++)
        peers[i] = meeting_peer_create((void *)(uintptr_t)(i + 1));

    /* Join 8 peers */
    for (int i = 0; i < MEETING_MAX_PEERS; i++)
        ASSERT_EQ(meeting_join(&mgr, peers[i], "full"), 0);

    clear_sent();

    /* 9th peer should fail */
    ASSERT_EQ(meeting_join(&mgr, peers[MEETING_MAX_PEERS], "full"), -1);
    /* Should receive error */
    ASSERT(find_msg_to(peers[MEETING_MAX_PEERS], "\"error\"") != NULL);
    printf("    9th peer correctly rejected\n");

    for (int i = 0; i <= MEETING_MAX_PEERS; i++)
        meeting_peer_destroy(&mgr, peers[i]);
    clear_sent();
}

TEST(meeting_relay_message) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);
    clear_sent();

    meeting_peer_t *a = meeting_peer_create((void *)1);
    meeting_peer_t *b = meeting_peer_create((void *)2);
    meeting_peer_t *c = meeting_peer_create((void *)3);

    meeting_join(&mgr, a, "relay");
    meeting_join(&mgr, b, "relay");
    meeting_join(&mgr, c, "relay");
    clear_sent();

    /* A sends offer to B (only B should receive) */
    char *offer = sig_msg_build_relay_offer(a->id, "test-sdp");
    ASSERT_EQ(meeting_route_message(&mgr, a, b->id, offer), 0);
    free(offer);

    ASSERT_EQ(count_msgs_to(b), 1);
    ASSERT_EQ(count_msgs_to(c), 0); /* C gets nothing */
    ASSERT_EQ(count_msgs_to(a), 0); /* A gets nothing */
    printf("    message routed to correct peer only\n");

    meeting_peer_destroy(&mgr, a);
    meeting_peer_destroy(&mgr, b);
    meeting_peer_destroy(&mgr, c);
    clear_sent();
}

TEST(meeting_broadcast_join_leave) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);
    clear_sent();

    meeting_peer_t *a = meeting_peer_create((void *)1);
    meeting_peer_t *b = meeting_peer_create((void *)2);
    meeting_join(&mgr, a, "bcast");
    meeting_join(&mgr, b, "bcast");
    clear_sent();

    meeting_peer_t *c = meeting_peer_create((void *)3);
    meeting_join(&mgr, c, "bcast");

    /* Both A and B should get peer_joined for C */
    ASSERT(find_msg_to(a, "\"peer_joined\"") != NULL);
    ASSERT(find_msg_to(b, "\"peer_joined\"") != NULL);

    clear_sent();

    /* C leaves - both A and B notified */
    meeting_leave(&mgr, c);
    ASSERT(find_msg_to(a, "\"peer_left\"") != NULL);
    ASSERT(find_msg_to(b, "\"peer_left\"") != NULL);

    meeting_peer_destroy(&mgr, a);
    meeting_peer_destroy(&mgr, b);
    meeting_peer_destroy(&mgr, c);
    clear_sent();
    printf("    broadcast join/leave OK\n");
}

TEST(meeting_disconnect_cleanup) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);
    clear_sent();

    meeting_peer_t *a = meeting_peer_create((void *)1);
    meeting_peer_t *b = meeting_peer_create((void *)2);
    meeting_join(&mgr, a, "dc");
    meeting_join(&mgr, b, "dc");
    clear_sent();

    /* Simulate WS close - destroy peer B */
    meeting_peer_destroy(&mgr, b);
    /* A should get peer_left */
    ASSERT(find_msg_to(a, "\"peer_left\"") != NULL);

    meeting_peer_destroy(&mgr, a);
    clear_sent();
    printf("    disconnect cleanup OK\n");
}

TEST(meeting_separate_rooms) {
    meeting_manager_t mgr;
    meeting_manager_init(&mgr, mock_send, NULL);
    clear_sent();

    meeting_peer_t *a = meeting_peer_create((void *)1);
    meeting_peer_t *b = meeting_peer_create((void *)2);
    meeting_join(&mgr, a, "room1");
    meeting_join(&mgr, b, "room2");
    clear_sent();

    /* Routing from A to B should fail (different meetings) */
    ASSERT_EQ(meeting_route_message(&mgr, a, b->id, "{}"), -1);
    printf("    separate meetings isolated OK\n");

    meeting_peer_destroy(&mgr, a);
    meeting_peer_destroy(&mgr, b);
    clear_sent();
}

int main(void) {
    printf("========================================\n");
    printf("  Signaling Meeting Tests\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(meeting_create_destroy);
    RUN_TEST(meeting_join_leave);
    RUN_TEST(meeting_full);
    RUN_TEST(meeting_relay_message);
    RUN_TEST(meeting_broadcast_join_leave);
    RUN_TEST(meeting_disconnect_cleanup);
    RUN_TEST(meeting_separate_rooms);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
