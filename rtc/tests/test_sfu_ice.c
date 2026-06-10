/*
 * Logical transport ICE tests.
 */
#include <rtc/rtc.h>
#include <rtc/rtc_worker.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_router.h>
#include <rtc/rtc_transport.h>

#include "test_harness.h"

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_router_t *router;
    rtc_transport_t *transport;
} ice_env_t;

static bool make_env(ice_env_t *env, rtc_ice_mode_t mode) {
    memset(env, 0, sizeof(*env));
    env->worker = rtc_worker_create(NULL);
    if (!env->worker)
        return false;
    env->listener = rtc_listener_create(env->worker, NULL);
    if (!env->listener)
        return false;
    env->router = rtc_router_create(env->worker, NULL);
    if (!env->router)
        return false;
    env->transport = rtc_router_create_transport(env->router, &(rtc_transport_config_t){
                                                                  .listener = env->listener,
                                                                  .ice_mode = mode,
                                                              });
    return env->transport != NULL;
}

static void close_env(ice_env_t *env) {
    rtc_transport_destroy(env->transport);
    rtc_router_destroy(env->router);
    rtc_listener_destroy(env->listener);
    rtc_worker_destroy(env->worker);
}

static bool listener_loopback_candidate(rtc_listener_t *listener, rtc_transport_candidate_t *out) {
    rtc_addr_t local;
    if (rtc_listener_get_local_addr(listener, &local) != RTC_OK)
        return false;
    uint16_t port = ntohs(((struct sockaddr_in *)&local.addr)->sin_port);
    memset(out, 0, sizeof(*out));
    memcpy(out->foundation, "H0", sizeof("H0"));
    memcpy(out->address, "127.0.0.1", sizeof("127.0.0.1"));
    memcpy(out->protocol, "udp", sizeof("udp"));
    out->port = port;
    out->type = RTC_TRANSPORT_CANDIDATE_HOST;
    return true;
}

static bool wait_for_selected(rtc_transport_t *transport, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK && stats.selected_tuple_valid)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_dtls_connected(rtc_transport_t *transport, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED && stats.srtp_ready)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

TEST(full_transport_connects_to_lite_transport) {
    ice_env_t full;
    ice_env_t lite;
    ASSERT(make_env(&full, RTC_ICE_MODE_FULL));
    ASSERT(make_env(&lite, RTC_ICE_MODE_LITE));

    rtc_dtls_parameters_t full_dtls;
    rtc_dtls_parameters_t lite_dtls;
    ASSERT_EQ(rtc_transport_get_dtls_parameters(full.transport, &full_dtls), RTC_OK);
    ASSERT_EQ(rtc_transport_get_dtls_parameters(lite.transport, &lite_dtls), RTC_OK);
    ASSERT_EQ(full_dtls.role, RTC_TRANSPORT_DTLS_ROLE_CLIENT);
    ASSERT_EQ(lite_dtls.role, RTC_TRANSPORT_DTLS_ROLE_SERVER);

    rtc_ice_parameters_t lite_ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(lite.transport, &lite_ice), RTC_OK);
    rtc_transport_candidate_t lite_candidate;
    ASSERT(listener_loopback_candidate(lite.listener, &lite_candidate));

    ASSERT_EQ(rtc_transport_set_remote_ice_parameters(full.transport, &lite_ice), RTC_OK);
    ASSERT_EQ(rtc_transport_add_remote_candidate(full.transport, &lite_candidate), RTC_OK);
    ASSERT_EQ(rtc_transport_start_ice(full.transport), RTC_OK);

    ASSERT(wait_for_selected(full.transport, 1000));
    ASSERT(wait_for_selected(lite.transport, 1000));

    ASSERT_EQ(rtc_transport_start_dtls(full.transport), RTC_OK);
    ASSERT(wait_for_dtls_connected(full.transport, 3000));
    ASSERT(wait_for_dtls_connected(lite.transport, 3000));

    close_env(&full);
    close_env(&lite);
}

TEST(full_transport_rejects_missing_remote) {
    ice_env_t full;
    ASSERT(make_env(&full, RTC_ICE_MODE_FULL));
    ASSERT_EQ(rtc_transport_start_ice(full.transport), RTC_ERR_INVALID);
    close_env(&full);
}

int main(void) {
    rtc_init();
    RUN_TEST(full_transport_connects_to_lite_transport);
    RUN_TEST(full_transport_rejects_missing_remote);
    rtc_cleanup();
    TEST_SUMMARY();
}