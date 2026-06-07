/*
 * test_listener.c - RTC listener lifecycle tests.
 */
#include <rtc/rtc.h>

#include "test_harness.h"

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

int main(void) {
    rtc_init();
    RUN_TEST(listener_create_candidate);
    RUN_TEST(listener_close_stats);
    RUN_TEST(listener_invalid_args);
    rtc_cleanup();
    TEST_SUMMARY();
}