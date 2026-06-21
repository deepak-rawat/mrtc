/*
 * Interceptor chain tests (rtc_interceptor).
 */
#include "rtc/rtc_interceptor.h"
#include "test_harness.h"

#include <stdlib.h>

typedef struct {
    int rtcp_count;
    int tick_count;
    int destroy_count;
    uint8_t last_pt;
    uint8_t last_fmt;
    int order_slot; /* which record index recorded the most recent dispatch */
} rec_t;

typedef struct {
    rtc_interceptor_t base;
    rec_t *rec;
    int id;
    int *order_log; /* shared dispatch-order log */
    int *order_count;
} test_it_t;

static void t_rtcp(rtc_interceptor_t *it, uint8_t pt, uint8_t fmt, const uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    test_it_t *t = (test_it_t *)it;
    t->rec->rtcp_count++;
    t->rec->last_pt = pt;
    t->rec->last_fmt = fmt;
    if (t->order_log && t->order_count)
        t->order_log[(*t->order_count)++] = t->id;
}

static void t_tick(rtc_interceptor_t *it, uint64_t now_ms) {
    (void)now_ms;
    ((test_it_t *)it)->rec->tick_count++;
}

static void t_destroy(rtc_interceptor_t *it) {
    test_it_t *t = (test_it_t *)it;
    t->rec->destroy_count++;
    free(t);
}

static const rtc_interceptor_ops_t t_ops = {
    .name = "test",
    .on_rtcp = t_rtcp,
    .on_tick = t_tick,
    .destroy = t_destroy,
};

static rtc_interceptor_t *make_test_it(rec_t *rec, int id, int *order_log, int *order_count) {
    test_it_t *t = (test_it_t *)calloc(1, sizeof(*t));
    t->base.ops = &t_ops;
    t->rec = rec;
    t->id = id;
    t->order_log = order_log;
    t->order_count = order_count;
    return &t->base;
}

TEST(chain_dispatch_and_tick) {
    rtc_interceptor_chain_t chain;
    rtc_interceptor_chain_init(&chain);

    rec_t r1 = {0}, r2 = {0};
    int order_log[8] = {0};
    int order_count = 0;
    ASSERT_EQ(rtc_interceptor_chain_add(&chain, make_test_it(&r1, 1, order_log, &order_count)),
              RTC_OK);
    ASSERT_EQ(rtc_interceptor_chain_add(&chain, make_test_it(&r2, 2, order_log, &order_count)),
              RTC_OK);

    uint8_t buf[8] = {0x80, 206, 0, 1, 0, 0, 0, 0};
    rtc_interceptor_chain_on_rtcp(&chain, 206, 1, buf, sizeof(buf));
    ASSERT_EQ(r1.rtcp_count, 1);
    ASSERT_EQ(r2.rtcp_count, 1);
    ASSERT_EQ((int)r1.last_pt, 206);
    ASSERT_EQ((int)r1.last_fmt, 1);

    /* Dispatched in insertion order. */
    ASSERT_EQ(order_count, 2);
    ASSERT_EQ(order_log[0], 1);
    ASSERT_EQ(order_log[1], 2);

    rtc_interceptor_chain_tick(&chain, 12345);
    ASSERT_EQ(r1.tick_count, 1);
    ASSERT_EQ(r2.tick_count, 1);

    rtc_interceptor_chain_close(&chain);
    ASSERT_EQ(r1.destroy_count, 1);
    ASSERT_EQ(r2.destroy_count, 1);
    ASSERT_EQ(chain.count, 0);
}

TEST(chain_full_rejects_and_keeps_ownership) {
    rtc_interceptor_chain_t chain;
    rtc_interceptor_chain_init(&chain);

    rec_t rec = {0};
    for (int i = 0; i < RTC_INTERCEPTOR_CHAIN_MAX; i++)
        ASSERT_EQ(rtc_interceptor_chain_add(&chain, make_test_it(&rec, i, NULL, NULL)), RTC_OK);

    rtc_interceptor_t *extra = make_test_it(&rec, 99, NULL, NULL);
    ASSERT(rtc_interceptor_chain_add(&chain, extra) != RTC_OK);
    /* On failure the caller still owns it. */
    extra->ops->destroy(extra);

    rtc_interceptor_chain_close(&chain);
    ASSERT_EQ(rec.destroy_count, RTC_INTERCEPTOR_CHAIN_MAX + 1);
}

TEST(chain_null_safe) {
    rtc_interceptor_chain_on_rtcp(NULL, 200, 0, NULL, 0);
    rtc_interceptor_chain_tick(NULL, 0);
    rtc_interceptor_chain_close(NULL);
    ASSERT(rtc_interceptor_chain_add(NULL, NULL) != RTC_OK);
}

int main(void) {
    printf("=== rtc_interceptor tests ===\n");
    RUN_TEST(chain_dispatch_and_tick);
    RUN_TEST(chain_full_rejects_and_keeps_ownership);
    RUN_TEST(chain_null_safe);
    TEST_SUMMARY();
}
