/*
 * Unit tests for the SSRC -> consumer RTP demultiplexer (rtc_rtp_demux).
 */
#include "rtc_rtp_demux.h"
#include "test_harness.h"

typedef struct {
    int calls;
    uint32_t last_ssrc;
    void *last_user;
} sink_rec_t;

static sink_rec_t g_sink;

static void test_sink(const rtc_rtp_packet_t *pkt, void *user) {
    g_sink.calls++;
    g_sink.last_ssrc = pkt->header.ssrc;
    g_sink.last_user = user;
}

/* Resolver: match by payload type. Returns a fixed token per PT. */
static int g_obj_a;
static int g_obj_b;
static int g_resolve_calls;

static void *test_resolve(const rtc_rtp_packet_t *pkt, void *user) {
    (void)user;
    g_resolve_calls++;
    if (pkt->header.payload_type == 96)
        return &g_obj_a;
    if (pkt->header.payload_type == 97)
        return &g_obj_b;
    return NULL;
}

static void make_pkt(rtc_rtp_packet_t *pkt, uint32_t ssrc, uint8_t pt) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.ssrc = ssrc;
    pkt->header.payload_type = pt;
}

TEST(bind_get_unbind) {
    rtc_rtp_demux_t d;
    ASSERT_EQ(rtc_rtp_demux_init(&d, test_sink, NULL, NULL), RTC_OK);

    ASSERT(rtc_rtp_demux_get(&d, 0x1111) == NULL);
    ASSERT_EQ(rtc_rtp_demux_bind(&d, 0x1111, &g_obj_a), RTC_OK);
    ASSERT(rtc_rtp_demux_get(&d, 0x1111) == &g_obj_a);

    /* rebind overwrites */
    ASSERT_EQ(rtc_rtp_demux_bind(&d, 0x1111, &g_obj_b), RTC_OK);
    ASSERT(rtc_rtp_demux_get(&d, 0x1111) == &g_obj_b);

    rtc_rtp_demux_unbind(&d, 0x1111);
    ASSERT(rtc_rtp_demux_get(&d, 0x1111) == NULL);

    rtc_rtp_demux_close(&d);
}

TEST(dispatch_bound) {
    rtc_rtp_demux_t d;
    ASSERT_EQ(rtc_rtp_demux_init(&d, test_sink, NULL, NULL), RTC_OK);
    ASSERT_EQ(rtc_rtp_demux_bind(&d, 0xCAFE, &g_obj_a), RTC_OK);

    memset(&g_sink, 0, sizeof(g_sink));
    rtc_rtp_packet_t pkt;
    make_pkt(&pkt, 0xCAFE, 96);
    ASSERT(rtc_rtp_demux_dispatch(&d, &pkt) == true);
    ASSERT_EQ(g_sink.calls, 1);
    ASSERT_EQ((int)g_sink.last_ssrc, (int)0xCAFE);
    ASSERT(g_sink.last_user == &g_obj_a);

    /* Unknown SSRC, no resolver -> dropped. */
    make_pkt(&pkt, 0xBEEF, 96);
    ASSERT(rtc_rtp_demux_dispatch(&d, &pkt) == false);
    ASSERT_EQ(g_sink.calls, 1);

    rtc_rtp_demux_close(&d);
}

TEST(dispatch_resolver_autobind) {
    rtc_rtp_demux_t d;
    ASSERT_EQ(rtc_rtp_demux_init(&d, test_sink, test_resolve, NULL), RTC_OK);

    memset(&g_sink, 0, sizeof(g_sink));
    g_resolve_calls = 0;

    /* First packet for SSRC 0x1A2B (PT 96) resolves to obj_a and binds. */
    rtc_rtp_packet_t pkt;
    make_pkt(&pkt, 0x1A2B, 96);
    ASSERT(rtc_rtp_demux_dispatch(&d, &pkt) == true);
    ASSERT_EQ(g_resolve_calls, 1);
    ASSERT(g_sink.last_user == &g_obj_a);
    ASSERT(rtc_rtp_demux_get(&d, 0x1A2B) == &g_obj_a);

    /* Second packet for the same SSRC is O(1): resolver not consulted again. */
    ASSERT(rtc_rtp_demux_dispatch(&d, &pkt) == true);
    ASSERT_EQ(g_resolve_calls, 1);
    ASSERT_EQ(g_sink.calls, 2);

    /* Unknown PT resolves to NULL -> dropped, no bind. */
    make_pkt(&pkt, 0x3C4D, 100);
    ASSERT(rtc_rtp_demux_dispatch(&d, &pkt) == false);
    ASSERT(rtc_rtp_demux_get(&d, 0x3C4D) == NULL);

    rtc_rtp_demux_close(&d);
}

TEST(invalid_args) {
    rtc_rtp_demux_t d;
    ASSERT_EQ(rtc_rtp_demux_init(&d, NULL, NULL, NULL), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_rtp_demux_init(&d, test_sink, NULL, NULL), RTC_OK);
    /* binding NULL user is rejected */
    ASSERT_EQ(rtc_rtp_demux_bind(&d, 0x10, NULL), RTC_ERR_INVALID);
    rtc_rtp_demux_close(&d);
    /* close is idempotent */
    rtc_rtp_demux_close(&d);
}

int main(void) {
    printf("=== rtc_rtp_demux tests ===\n");
    RUN_TEST(bind_get_unbind);
    RUN_TEST(dispatch_bound);
    RUN_TEST(dispatch_resolver_autobind);
    RUN_TEST(invalid_args);
    TEST_SUMMARY();
}
