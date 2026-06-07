/*
 * test_sfu_timers.c - SFU runtime timer integration tests.
 */
#include <rtc/rtc.h>

#include "rtc_dtls.h"
#include "rtc_stun.h"
#include "test_harness.h"

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

static int listener_loopback_addr(rtc_listener_t *listener, rtc_addr_t *out) {
    rtc_addr_t local;
    int rc = rtc_listener_get_local_addr(listener, &local);
    if (rc != RTC_OK)
        return rc;
    uint16_t port = ntohs(((struct sockaddr_in *)&local.addr)->sin_port);
    return rtc_addr_from_string(out, "127.0.0.1", port);
}

static rtc_socket_t make_bound_sender(void) {
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
    if (rtc_set_nonblocking(sender) != RTC_OK) {
        rtc_close_socket(sender);
        return RTC_INVALID_SOCKET;
    }
    return sender;
}

static int send_udp_from(rtc_listener_t *listener, const uint8_t *data, size_t len,
                         rtc_socket_t sender) {
    rtc_addr_t dest;
    int rc = listener_loopback_addr(listener, &dest);
    if (rc != RTC_OK)
        return rc;
    int sent = sendto(sender, (const char *)data, (int)len, 0,
                      (const struct sockaddr *)&dest.addr, dest.len);
    return sent == (int)len ? RTC_OK : RTC_ERR_SOCKET;
}

typedef struct {
    rtc_listener_t *listener;
    rtc_socket_t sender;
} dtls_client_send_ctx_t;

static int dtls_client_send(const uint8_t *data, size_t len, void *user) {
    dtls_client_send_ctx_t *ctx = (dtls_client_send_ctx_t *)user;
    return send_udp_from(ctx->listener, data, len, ctx->sender);
}

static bool recv_stun_response(rtc_socket_t sender, uint8_t *buf, size_t buf_len, size_t *out_len,
                               int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        int n = recv(sender, (char *)buf, (int)buf_len, 0);
        if (n > 0) {
            *out_len = (size_t)n;
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_transport_packets(rtc_transport_t *transport, uint64_t target,
                                       int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.packets_received >= target)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_worker_timers(rtc_worker_t *worker, int target, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_worker_stats_t stats;
        if (rtc_worker_get_stats(worker, &stats) == RTC_OK && stats.timers_pending == target)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool select_transport_tuple(rtc_listener_t *listener, rtc_transport_t *transport,
                                   rtc_socket_t sender, const char *ufrag) {
    char username[64];
    snprintf(username, sizeof(username), "%s:remote", ufrag);
    rtc_stun_msg_t req;
    if (rtc_stun_build_binding_request(&req, username, NULL, 1234, true, 99, true) != RTC_OK)
        return false;
    if (send_udp_from(listener, req.buf, req.buf_len, sender) != RTC_OK)
        return false;
    if (!wait_for_transport_packets(transport, 1, 1000))
        return false;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    if (!recv_stun_response(sender, resp_buf, sizeof(resp_buf), &resp_len, 1000))
        return false;
    rtc_stun_msg_t resp;
    return rtc_stun_parse(&resp, resp_buf, resp_len) == RTC_OK &&
           resp.type == STUN_BINDING_RESPONSE;
}

TEST(logical_transport_uses_worker_timer_for_dtls) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport = rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                                       .listener = listener,
                                                                       .ice_mode = RTC_ICE_MODE_LITE,
                                                                   });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, sender, ice.username_fragment));
    ASSERT(wait_for_worker_timers(worker, 0, 1000));

    dtls_client_send_ctx_t send_ctx = {.listener = listener, .sender = sender};
    rtc_dtls_transport_t client;
    ASSERT_EQ(rtc_dtls_init(&client, RTC_DTLS_ROLE_CLIENT, dtls_client_send, &send_ctx), RTC_OK);
    ASSERT_EQ(rtc_dtls_handshake(&client), RTC_OK);
    ASSERT(wait_for_transport_packets(transport, 2, 1000));
    ASSERT(wait_for_worker_timers(worker, 1, 1000));

    rtc_transport_close(transport);
    ASSERT(wait_for_worker_timers(worker, 0, 1000));

    rtc_dtls_close(&client);
    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

int main(void) {
    rtc_init();
    RUN_TEST(logical_transport_uses_worker_timer_for_dtls);
    rtc_cleanup();
    TEST_SUMMARY();
}