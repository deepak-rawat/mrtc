/*
 * test_sfu_media.c - SFU producer/consumer skeleton tests.
 */
#include <rtc/rtc.h>

#include "test_harness.h"

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_router_t *router;
    rtc_transport_t *transport;
} test_env_t;

static bool make_env(test_env_t *env) {
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
                                                                  .ice_mode = RTC_ICE_MODE_LITE,
                                                              });
    return env->transport != NULL;
}

static void close_env(test_env_t *env) {
    rtc_transport_destroy(env->transport);
    rtc_router_destroy(env->router);
    rtc_listener_destroy(env->listener);
    rtc_worker_destroy(env->worker);
}

static rtc_producer_options_t video_producer_options(void) {
    rtc_producer_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.kind = RTC_MEDIA_KIND_VIDEO;
    opts.label = "camera";
    opts.rtp.ssrc = 0x12345678;
    opts.rtp.codec_count = 1;
    opts.rtp.codecs[0].kind = RTC_MEDIA_KIND_VIDEO;
    opts.rtp.codecs[0].payload_type = 96;
    memcpy(opts.rtp.codecs[0].mime_type, "video/VP8", sizeof("video/VP8"));
    opts.rtp.codecs[0].clock_rate = 90000;
    return opts;
}

TEST(producer_create_stats) {
    test_env_t env;
    ASSERT(make_env(&env));

    rtc_producer_options_t opts = video_producer_options();
    rtc_producer_t *producer = rtc_transport_produce(env.transport, &opts);
    ASSERT(producer != NULL);
    ASSERT(rtc_producer_id(producer)[0] != '\0');

    rtc_producer_stats_t stats;
    ASSERT_EQ(rtc_producer_get_stats(producer, &stats), RTC_OK);
    ASSERT(!stats.closed);
    ASSERT_EQ(stats.kind, RTC_MEDIA_KIND_VIDEO);

    rtc_producer_close(producer);
    ASSERT_EQ(rtc_producer_get_stats(producer, &stats), RTC_OK);
    ASSERT(stats.closed);

    rtc_producer_destroy(producer);
    close_env(&env);
}

TEST(consumer_create_pause_resume) {
    test_env_t env;
    ASSERT(make_env(&env));

    rtc_producer_options_t opts = video_producer_options();
    rtc_producer_t *producer = rtc_transport_produce(env.transport, &opts);
    ASSERT(producer != NULL);

    rtc_consumer_t *consumer = rtc_transport_consume(env.transport, &(rtc_consumer_options_t){
                                                                        .producer = producer,
                                                                        .paused = true,
                                                                    });
    ASSERT(consumer != NULL);
    ASSERT(rtc_consumer_id(consumer)[0] != '\0');

    rtc_consumer_stats_t stats;
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(!stats.closed);
    ASSERT(stats.paused);
    ASSERT_EQ(stats.kind, RTC_MEDIA_KIND_VIDEO);

    rtc_rtp_parameters_t rtp;
    ASSERT_EQ(rtc_consumer_get_rtp_parameters(consumer, &rtp), RTC_OK);
    ASSERT_EQ(rtp.codec_count, 1);
    ASSERT_EQ(rtp.codecs[0].payload_type, 96);
    ASSERT_STR_EQ(rtp.codecs[0].mime_type, "video/VP8");
    ASSERT(rtp.ssrc != 0);
    ASSERT(rtp.ssrc != opts.rtp.ssrc);
    ASSERT(rtp.mid[0] != '\0');

    rtc_consumer_resume(consumer);
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(!stats.paused);
    rtc_consumer_pause(consumer);
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(stats.paused);

    rtc_consumer_destroy(consumer);
    rtc_producer_destroy(producer);
    close_env(&env);
}

TEST(media_rejects_closed_inputs) {
    test_env_t env;
    ASSERT(make_env(&env));

    rtc_producer_options_t opts = video_producer_options();
    rtc_transport_close(env.transport);
    ASSERT(rtc_transport_produce(env.transport, &opts) == NULL);

    close_env(&env);
}

int main(void) {
    rtc_init();
    RUN_TEST(producer_create_stats);
    RUN_TEST(consumer_create_pause_resume);
    RUN_TEST(media_rejects_closed_inputs);
    rtc_cleanup();
    TEST_SUMMARY();
}