/*
 * test_listener.c - RTC listener lifecycle tests.
 */
#include <rtc/rtc.h>

#include "rtc_listener_internal.h"
#include "rtc_stun.h"
#include "test_harness.h"

#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

static rtc_mutex_t g_route_mutex;
static rtc_cond_t g_route_cond;
static int g_route_count;
static rtc_pkt_type_t g_route_type;
static int g_route_user_value;

static void route_callback(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                           const rtc_addr_t *from, void *user) {
    (void)data;
    (void)len;
    (void)from;
    int value = *(int *)user;
    rtc_mutex_lock(&g_route_mutex);
    g_route_type = type;
    g_route_user_value = value;
    g_route_count++;
    rtc_cond_signal(&g_route_cond);
    rtc_mutex_unlock(&g_route_mutex);
}

static void reset_route_state(void) {
    rtc_mutex_lock(&g_route_mutex);
    g_route_count = 0;
    g_route_type = RTC_PKT_UNKNOWN;
    g_route_user_value = 0;
    rtc_mutex_unlock(&g_route_mutex);
}

static bool wait_for_route(int target, int timeout_ms) {
    rtc_mutex_lock(&g_route_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_route_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_route_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_route_cond, &g_route_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_route_mutex);
    return true;
}

static int listener_loopback_addr(rtc_listener_t *listener, rtc_addr_t *out) {
    rtc_addr_t local;
    int rc = rtc_listener_get_local_addr(listener, &local);
    if (rc != RTC_OK)
        return rc;
    uint16_t port = ntohs(((struct sockaddr_in *)&local.addr)->sin_port);
    return rtc_addr_from_string(out, "127.0.0.1", port);
}

static rtc_socket_t make_bound_sender(rtc_addr_t *local_out) {
    rtc_socket_t sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == RTC_INVALID_SOCKET)
        return RTC_INVALID_SOCKET;

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    if (bind(sender, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        rtc_close_socket(sender);
        return RTC_INVALID_SOCKET;
    }

    if (local_out) {
        local_out->len = sizeof(struct sockaddr_in);
        if (getsockname(sender, (struct sockaddr *)&local_out->addr, &local_out->len) != 0) {
            rtc_close_socket(sender);
            return RTC_INVALID_SOCKET;
        }
    }
    return sender;
}

static int send_to_listener(rtc_listener_t *listener, const uint8_t *data, size_t len,
                            rtc_socket_t sender) {
    rtc_addr_t dest;
    int rc = listener_loopback_addr(listener, &dest);
    if (rc != RTC_OK)
        return rc;
    int sent = sendto(sender, (const char *)data, (int)len, 0,
                      (const struct sockaddr *)&dest.addr, dest.len);
    return sent == (int)len ? RTC_OK : RTC_ERR_SOCKET;
}

TEST(listener_create_candidate) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);

    rtc_listener_t *listener = rtc_listener_create(worker, &(rtc_listener_config_t){
                                                               .announced_ip = "203.0.113.10",
                                                               .enable_udp = true,
                                                           });
    ASSERT(listener != NULL);

    rtc_addr_t local;
    ASSERT_EQ(rtc_listener_get_local_addr(listener, &local), RTC_OK);
    char local_ip[64];
    uint16_t local_port = 0;
    ASSERT_EQ(rtc_addr_to_string(&local, local_ip, sizeof(local_ip), &local_port), RTC_OK);
    ASSERT(local_port != 0);

    int count = 0;
    ASSERT_EQ(rtc_listener_get_candidates(listener, NULL, &count), RTC_OK);
    ASSERT_EQ(count, 1);

    rtc_transport_candidate_t candidates[1];
    count = 1;
    ASSERT_EQ(rtc_listener_get_candidates(listener, candidates, &count), RTC_OK);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(candidates[0].address, "203.0.113.10");
    ASSERT_STR_EQ(candidates[0].protocol, "udp");
    ASSERT_EQ(candidates[0].port, local_port);
    ASSERT_EQ(candidates[0].type, RTC_TRANSPORT_CANDIDATE_HOST);

    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(listener_close_stats) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);

    rtc_listener_stats_t stats;
    ASSERT_EQ(rtc_listener_get_stats(listener, &stats), RTC_OK);
    ASSERT(!stats.closed);

    rtc_listener_close(listener);
    rtc_listener_close(listener);
    ASSERT_EQ(rtc_listener_get_stats(listener, &stats), RTC_OK);
    ASSERT(stats.closed);

    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(listener_invalid_args) {
    rtc_listener_stats_t stats;
    ASSERT_EQ(rtc_listener_get_stats(NULL, &stats), RTC_ERR_INVALID);

    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    ASSERT(rtc_listener_create(NULL, NULL) == NULL);
    ASSERT(rtc_listener_create(worker, &(rtc_listener_config_t){.enable_tcp = true}) == NULL);
    rtc_worker_destroy(worker);
}

TEST(listener_routes_stun_by_ufrag) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);

    int route_user = 11;
    ASSERT_EQ(rtc_listener_register_ufrag(listener, "local", route_callback, &route_user), RTC_OK);
    reset_route_state();

    rtc_socket_t sender = make_bound_sender(NULL);
    ASSERT(sender != RTC_INVALID_SOCKET);
    rtc_stun_msg_t req;
    ASSERT_EQ(rtc_stun_build_binding_request(&req, "local:remote", NULL, 1234, true, 99, true),
              RTC_OK);
    ASSERT_EQ(send_to_listener(listener, req.buf, req.buf_len, sender), RTC_OK);
    ASSERT(wait_for_route(1, 1000));
    ASSERT_EQ(g_route_type, RTC_PKT_STUN);
    ASSERT_EQ(g_route_user_value, 11);

    rtc_close_socket(sender);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(listener_routes_stun_by_transaction) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);

    rtc_stun_msg_t req;
    ASSERT_EQ(rtc_stun_build_binding_request(&req, NULL, NULL, 0, false, 0, true), RTC_OK);
    req.buf[0] = 0x01;
    req.buf[1] = 0x01;

    int route_user = 22;
    ASSERT_EQ(rtc_listener_register_stun_txn(listener, req.txn_id, route_callback, &route_user),
              RTC_OK);
    reset_route_state();

    rtc_socket_t sender = make_bound_sender(NULL);
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT_EQ(send_to_listener(listener, req.buf, req.buf_len, sender), RTC_OK);
    ASSERT(wait_for_route(1, 1000));
    ASSERT_EQ(g_route_type, RTC_PKT_STUN);
    ASSERT_EQ(g_route_user_value, 22);

    rtc_close_socket(sender);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(listener_routes_rtp_by_tuple) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);

    rtc_addr_t sender_addr;
    rtc_socket_t sender = make_bound_sender(&sender_addr);
    ASSERT(sender != RTC_INVALID_SOCKET);

    int route_user = 33;
    ASSERT_EQ(rtc_listener_register_tuple(listener, &sender_addr, route_callback, &route_user),
              RTC_OK);
    reset_route_state();

    const uint8_t rtp[] = {0x80, 0x60, 0x00, 0x01, 0, 0, 0, 1, 0x12, 0x34, 0x56, 0x78};
    ASSERT_EQ(send_to_listener(listener, rtp, sizeof(rtp), sender), RTC_OK);
    ASSERT(wait_for_route(1, 1000));
    ASSERT_EQ(g_route_type, RTC_PKT_RTP);
    ASSERT_EQ(g_route_user_value, 33);

    rtc_close_socket(sender);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

int main(void) {
    rtc_init();
    rtc_mutex_init(&g_route_mutex);
    rtc_cond_init(&g_route_cond);

    RUN_TEST(listener_create_candidate);
    RUN_TEST(listener_close_stats);
    RUN_TEST(listener_invalid_args);
    RUN_TEST(listener_routes_stun_by_ufrag);
    RUN_TEST(listener_routes_stun_by_transaction);
    RUN_TEST(listener_routes_rtp_by_tuple);

    rtc_cond_destroy(&g_route_cond);
    rtc_mutex_destroy(&g_route_mutex);
    rtc_cleanup();
    TEST_SUMMARY();
}