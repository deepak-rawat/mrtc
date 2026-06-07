/*
 * test_sfu_transport.c - Logical SFU transport skeleton tests.
 */
#include <rtc/rtc.h>

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

static int send_udp(rtc_listener_t *listener, const uint8_t *data, size_t len) {
    rtc_addr_t dest;
    int rc = listener_loopback_addr(listener, &dest);
    if (rc != RTC_OK)
        return rc;
    rtc_socket_t sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == RTC_INVALID_SOCKET)
        return RTC_ERR_SOCKET;
    int sent = sendto(sender, (const char *)data, (int)len, 0,
                      (const struct sockaddr *)&dest.addr, dest.len);
    rtc_close_socket(sender);
    return sent == (int)len ? RTC_OK : RTC_ERR_SOCKET;
}

static bool wait_for_transport_packets(rtc_transport_t *transport, uint64_t target,
                                       int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.packets_received >= target) {
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

TEST(transport_create_ice_params) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);

    rtc_transport_t *transport = rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                                       .listener = listener,
                                                                       .ice_mode = RTC_ICE_MODE_LITE,
                                                                       .enable_twcc = true,
                                                                   });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    ASSERT(ice.username_fragment[0] != '\0');
    ASSERT(ice.password[0] != '\0');
    ASSERT_EQ(ice.mode, RTC_ICE_MODE_LITE);

    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(transport, &stats), RTC_OK);
    ASSERT(!stats.closed);

    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_receives_stun_by_ufrag) {
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

    char username[64];
    snprintf(username, sizeof(username), "%s:remote", ice.username_fragment);
    rtc_stun_msg_t req;
    ASSERT_EQ(rtc_stun_build_binding_request(&req, username, NULL, 1234, true, 99, true), RTC_OK);
    ASSERT_EQ(send_udp(listener, req.buf, req.buf_len), RTC_OK);
    ASSERT(wait_for_transport_packets(transport, 1, 1000));

    rtc_transport_close(transport);
    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(transport, &stats), RTC_OK);
    ASSERT(stats.closed);

    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(router_rejects_closed_create) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);

    rtc_router_close(router);
    ASSERT(rtc_router_create_transport(router, &(rtc_transport_config_t){.listener = listener}) ==
           NULL);

    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

int main(void) {
    rtc_init();
    RUN_TEST(transport_create_ice_params);
    RUN_TEST(transport_receives_stun_by_ufrag);
    RUN_TEST(router_rejects_closed_create);
    rtc_cleanup();
    TEST_SUMMARY();
}