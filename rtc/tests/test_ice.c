/*
 * ICE component tests.
 *
 * Tests:
 *   1. Init agent: verify credentials generated
 *   2. Gather candidates: verify host candidates found
 *   3. Set remote credentials and add remote candidates
 *   4. Two agents connect to each other (loopback e2e)
 *   5. Send data over connected transport
 */
#include <rtc/rtc.h>
#include "rtc_ice.h"
#include "rtc_packet_io.h"
#include "test_harness.h"
#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ------------------------------------------------------------------ */
/*  Test: init creates credentials                                     */
/* ------------------------------------------------------------------ */
TEST(ice_init) {
    rtc_packet_io_t transport;
    int rc = rtc_packet_io_init(&transport, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);

    rtc_ice_agent_t agent;
    rc = rtc_ice_init(&agent, &transport, NULL, 0);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(agent.state, ICE_STATE_NEW);

    /* Credentials should be non-empty */
    ASSERT(strlen(agent.ufrag) > 0);
    ASSERT(strlen(agent.pwd) > 0);

    printf("    ufrag=%s pwd=%s\n", agent.ufrag, agent.pwd);

    rtc_ice_close(&agent);
    ASSERT_EQ(agent.state, ICE_STATE_CLOSED);
    rtc_packet_io_close(&transport);
}

/* ------------------------------------------------------------------ */
/*  Test: gather finds at least one host candidate                     */
/* ------------------------------------------------------------------ */
TEST(ice_gather_host) {
    rtc_packet_io_t transport;
    rtc_packet_io_init(&transport, NULL, NULL);

    rtc_ice_agent_t agent;
    rtc_ice_init(&agent, &transport, NULL, 0);

    int rc = rtc_ice_gather(&agent);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(agent.local_candidate_count > 0);

    printf("    gathered %d candidate(s):\n", agent.local_candidate_count);
    for (int i = 0; i < agent.local_candidate_count; i++) {
        rtc_ice_candidate_t *c = &agent.local_candidates[i];
        char ip[64];
        uint16_t port;
        rtc_addr_to_string(&c->addr, ip, sizeof(ip), &port);
        printf("      [%d] %s %s:%u pri=%u\n", i, c->type == ICE_CANDIDATE_HOST ? "host " : "srflx",
               ip, port, c->priority);
    }

    rtc_ice_close(&agent);
    rtc_packet_io_close(&transport);
}

/* ------------------------------------------------------------------ */
/*  Test: set remote credentials                                       */
/* ------------------------------------------------------------------ */
TEST(ice_remote_credentials) {
    rtc_packet_io_t transport;
    rtc_packet_io_init(&transport, NULL, NULL);

    rtc_ice_agent_t agent;
    rtc_ice_init(&agent, &transport, NULL, 0);

    int rc = rtc_ice_set_remote_credentials(&agent, "rUfrag", "rPassword12345678901");
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_STR_EQ(agent.remote_ufrag, "rUfrag");
    ASSERT_STR_EQ(agent.remote_pwd, "rPassword12345678901");

    /* Add a remote candidate */
    rtc_ice_candidate_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.type = ICE_CANDIDATE_HOST;
    cand.component = 1;
    cand.priority = 2130706431;
    rtc_addr_from_string(&cand.addr, "10.0.0.1", 5000);

    rc = rtc_ice_add_remote_candidate(&agent, &cand);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(agent.remote_candidate_count, 1);

    printf("    remote credentials set, 1 candidate added\n");

    rtc_ice_close(&agent);
    rtc_packet_io_close(&transport);
}

/* ------------------------------------------------------------------ */
/*  Shared callback state for transport-based ICE tests                */
/* ------------------------------------------------------------------ */
static _Atomic int g_stun_recv_count;
static uint8_t g_stun_recv_buf[2048];
static _Atomic size_t g_stun_recv_len;
static rtc_addr_t g_stun_recv_from;
static rtc_mutex_t g_ice_mutex;
static rtc_cond_t g_ice_cond;
static _Atomic int g_data_recv_count;
static uint8_t g_data_recv_buf[2048];
static _Atomic size_t g_data_recv_len;

/* Callback that forwards STUN to ICE and captures other packets */
static rtc_ice_agent_t *g_stun_agent; /* agent to forward STUN to */

static void ice_test_recv_cb(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                             const rtc_addr_t *from, void *user) {
    (void)user;
    rtc_mutex_lock(&g_ice_mutex);
    rtc_ice_agent_t *agent = g_stun_agent;
    rtc_mutex_unlock(&g_ice_mutex);
    if (type == RTC_PKT_STUN && agent) {
        rtc_ice_handle_stun(agent, data, len, from);
    } else {
        rtc_mutex_lock(&g_ice_mutex);
        if (len <= sizeof(g_data_recv_buf)) {
            memcpy(g_data_recv_buf, data, len);
            g_data_recv_len = len;
        }
        g_data_recv_count++;
        rtc_cond_signal(&g_ice_cond);
        rtc_mutex_unlock(&g_ice_mutex);
    }
}

/* Callback that only captures STUN responses (for Alice) */
static void ice_test_stun_capture_cb(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                     const rtc_addr_t *from, void *user) {
    (void)user;
    (void)from;
    if (type == RTC_PKT_STUN) {
        rtc_mutex_lock(&g_ice_mutex);
        if (len <= sizeof(g_stun_recv_buf)) {
            memcpy(g_stun_recv_buf, data, len);
            g_stun_recv_len = len;
        }
        g_stun_recv_count++;
        rtc_cond_signal(&g_ice_cond);
        rtc_mutex_unlock(&g_ice_mutex);
    }
}

/* Data recv callback */
static void ice_test_data_capture_cb(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                     const rtc_addr_t *from, void *user) {
    (void)type;
    (void)from;
    (void)user;
    rtc_mutex_lock(&g_ice_mutex);
    if (len <= sizeof(g_data_recv_buf)) {
        memcpy(g_data_recv_buf, data, len);
        g_data_recv_len = len;
    }
    g_data_recv_count++;
    rtc_cond_signal(&g_ice_cond);
    rtc_mutex_unlock(&g_ice_mutex);
}

static bool wait_for_stun(int target, int timeout_ms) {
    rtc_mutex_lock(&g_ice_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_stun_recv_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_ice_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_ice_cond, &g_ice_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_ice_mutex);
    return true;
}

static bool wait_for_data(int target, int timeout_ms) {
    rtc_mutex_lock(&g_ice_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_data_recv_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_ice_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_ice_cond, &g_ice_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_ice_mutex);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Test: two agents connect to each other on localhost                */
/* ------------------------------------------------------------------ */
TEST(ice_two_agents_connect) {
    rtc_packet_io_t transport_a, transport_b;
    rtc_ice_agent_t alice, bob;

    /*
     * Install recv callbacks at init time:
     *   transport_a captures binding responses for Alice.
     *   transport_b dispatches to Bob's ICE via g_stun_agent (set below).
     * g_stun_agent is NULL initially; no packets arrive until we send.
     */
    g_stun_recv_count = 0;
    int rc = rtc_packet_io_init(&transport_a, ice_test_stun_capture_cb, NULL);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_packet_io_init(&transport_b, ice_test_recv_cb, NULL);
    ASSERT_EQ(rc, RTC_OK);

    rc = rtc_ice_init(&alice, &transport_a, NULL, 0);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_ice_init(&bob, &transport_b, NULL, 0);
    ASSERT_EQ(rc, RTC_OK);

    alice.controlling = true;
    bob.controlling = false;

    rc = rtc_ice_gather(&alice);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_ice_gather(&bob);
    ASSERT_EQ(rc, RTC_OK);

    printf("    alice: %d candidates, ufrag=%s\n", alice.local_candidate_count, alice.ufrag);
    printf("    bob:   %d candidates, ufrag=%s\n", bob.local_candidate_count, bob.ufrag);

    rtc_ice_set_remote_credentials(&alice, bob.ufrag, bob.pwd);
    rtc_ice_set_remote_credentials(&bob, alice.ufrag, alice.pwd);

    for (int i = 0; i < bob.local_candidate_count; i++) {
        if (bob.local_candidates[i].type == ICE_CANDIDATE_HOST)
            rtc_ice_add_remote_candidate(&alice, &bob.local_candidates[i]);
    }
    for (int i = 0; i < alice.local_candidate_count; i++) {
        if (alice.local_candidates[i].type == ICE_CANDIDATE_HOST)
            rtc_ice_add_remote_candidate(&bob, &alice.local_candidates[i]);
    }

    ASSERT(alice.remote_candidate_count > 0);
    ASSERT(bob.remote_candidate_count > 0);

    /*
     * Bob's ICE is the STUN handler. Publish it now that the agents
     * are initialized; transport_b's callback (installed at init) will
     * pick it up when Alice's binding request arrives.
     */
    rtc_mutex_lock(&g_ice_mutex);
    g_stun_agent = &bob;
    rtc_mutex_unlock(&g_ice_mutex);

    /* Build and send STUN request from Alice to Bob */
    char username_a[ICE_UFRAG_LEN * 2 + 2];
    snprintf(username_a, sizeof(username_a), "%s:%s", bob.ufrag, alice.ufrag);

    rtc_stun_msg_t req;
    rc = rtc_stun_build_binding_request(&req, username_a, bob.pwd, 2130706431, true,
                                        alice.tie_breaker, true);
    ASSERT_EQ(rc, RTC_OK);

    rtc_addr_t bob_addr = bob.local_candidates[0].addr;
    rc = rtc_packet_io_send(&transport_a, req.buf, req.buf_len, &bob_addr);
    ASSERT_EQ(rc, RTC_OK);

    /* Wait for Alice to receive the binding response via her transport callback */
    bool got = wait_for_stun(1, 2000);
    ASSERT(got);

    /* Verify it's a STUN Binding Response */
    uint16_t msg_type = ((uint16_t)g_stun_recv_buf[0] << 8) | g_stun_recv_buf[1];
    ASSERT_EQ(msg_type, STUN_BINDING_RESPONSE);

    /* Transaction ID should match */
    ASSERT_MEM_EQ(g_stun_recv_buf + 8, req.txn_id, STUN_TXN_ID_SIZE);

    printf("    Alice -> Bob STUN check: request sent, response received\n");

    rtc_packet_io_close(&transport_a);
    rtc_packet_io_close(&transport_b);
    rtc_mutex_lock(&g_ice_mutex);
    g_stun_agent = NULL;
    rtc_mutex_unlock(&g_ice_mutex);
    rtc_ice_close(&alice);
    rtc_ice_close(&bob);
}

/* ------------------------------------------------------------------ */
/*  Test: data transfer via transport after manual connection setup    */
/* ------------------------------------------------------------------ */
TEST(ice_data_transfer) {
    rtc_packet_io_t transport_a, transport_b;
    rtc_ice_agent_t alice, bob;

    g_data_recv_count = 0;
    rtc_packet_io_init(&transport_a, ice_test_data_capture_cb, NULL);
    rtc_packet_io_init(&transport_b, ice_test_data_capture_cb, NULL);
    rtc_ice_init(&alice, &transport_a, NULL, 0);
    rtc_ice_init(&bob, &transport_b, NULL, 0);
    rtc_ice_gather(&alice);
    rtc_ice_gather(&bob);

    /* Manually set up "connected" state */
    rtc_packet_io_set_remote(&transport_a, &bob.local_candidates[0].addr);
    alice.selected_remote = bob.local_candidates[0].addr;
    alice.state = ICE_STATE_CONNECTED;

    rtc_packet_io_set_remote(&transport_b, &alice.local_candidates[0].addr);
    bob.selected_remote = alice.local_candidates[0].addr;
    bob.state = ICE_STATE_CONNECTED;

    /* Alice sends data to Bob via transport */
    const char *msg = "Hello over ICE!";
    int rc = rtc_packet_io_send_to_remote(&transport_a, (const uint8_t *)msg, strlen(msg));
    ASSERT_EQ(rc, RTC_OK);

    /* Wait for Bob to receive via transport callback */
    bool got = wait_for_data(1, 2000);
    ASSERT(got);
    ASSERT_EQ(g_data_recv_len, strlen(msg));
    ASSERT_MEM_EQ(g_data_recv_buf, msg, strlen(msg));

    printf("    Alice -> Bob: \"%s\" (%zu bytes)\n", msg, g_data_recv_len);

    /* Bob sends back to Alice */
    g_data_recv_count = 0;

    const char *reply = "Reply from Bob!";
    rc = rtc_packet_io_send_to_remote(&transport_b, (const uint8_t *)reply, strlen(reply));
    ASSERT_EQ(rc, RTC_OK);

    got = wait_for_data(1, 2000);
    ASSERT(got);
    ASSERT_EQ(g_data_recv_len, strlen(reply));
    ASSERT_MEM_EQ(g_data_recv_buf, reply, strlen(reply));

    printf("    Bob -> Alice: \"%s\" (%zu bytes)\n", reply, g_data_recv_len);

    rtc_packet_io_close(&transport_a);
    rtc_packet_io_close(&transport_b);
    rtc_ice_close(&alice);
    rtc_ice_close(&bob);
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  ICE Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);
    rtc_mutex_init(&g_ice_mutex);
    rtc_cond_init(&g_ice_cond);

    RUN_TEST(ice_init);
    RUN_TEST(ice_gather_host);
    RUN_TEST(ice_remote_credentials);
    RUN_TEST(ice_two_agents_connect);
    RUN_TEST(ice_data_transfer);

    rtc_cond_destroy(&g_ice_cond);
    rtc_mutex_destroy(&g_ice_mutex);
    rtc_cleanup();
    TEST_SUMMARY();
}
