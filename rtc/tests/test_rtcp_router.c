/*
 * Unit tests for the RTCP feedback router (rtc_rtcp_router).
 *
 * Exercises SSRC -> send-stream routing for NACK / PLI feedback with a NULL
 * transport (SR routing + RR loss feed require a live transport and are covered
 * end-to-end by the client peer test).
 */
#include "rtc/rtc_rtcp.h"
#include "rtc/rtc_rtcp_router.h"
#include "rtc/rtc_rtp_stream.h"
#include "test_harness.h"

typedef struct {
    int nack_calls;
    int last_count;
    uint16_t last_seq0;
    int pli_calls;
} fb_rec_t;

static void on_nack(const uint16_t *lost_seqs, int count, void *user) {
    fb_rec_t *rec = (fb_rec_t *)user;
    rec->nack_calls++;
    rec->last_count = count;
    rec->last_seq0 = count > 0 ? lost_seqs[0] : 0;
}

static void on_pli(void *user) {
    fb_rec_t *rec = (fb_rec_t *)user;
    rec->pli_calls++;
}

static rtc_rtp_send_stream_t *make_stream(fb_rec_t *rec) {
    rtc_rtp_send_stream_config_t cfg = {.payload_type = 96, .clock_rate = 90000};
    rtc_rtp_send_stream_t *s = rtc_rtp_send_stream_create(&cfg);
    rtc_rtp_send_stream_on_nack(s, on_nack, rec);
    rtc_rtp_send_stream_on_pli(s, on_pli, rec);
    return s;
}

TEST(nack_routes_to_named_sender) {
    fb_rec_t r1 = {0};
    fb_rec_t r2 = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r1);
    rtc_rtp_send_stream_t *s2 = make_stream(&r2);
    uint32_t ssrc1 = rtc_rtp_send_stream_ssrc(s1);
    uint32_t ssrc2 = rtc_rtp_send_stream_ssrc(s2);

    rtc_rtcp_router_t router;
    ASSERT_EQ(rtc_rtcp_router_init(&router, NULL), RTC_OK);
    ASSERT_EQ(rtc_rtcp_router_add_sender(&router, ssrc1, s1), RTC_OK);
    ASSERT_EQ(rtc_rtcp_router_add_sender(&router, ssrc2, s2), RTC_OK);

    uint8_t buf[256];
    size_t len = 0;
    uint16_t lost[] = {1001, 1002};
    ASSERT_EQ(rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xABCD, ssrc1, lost, 2), RTC_OK);

    rtc_rtcp_router_handle(&router, buf, len);
    ASSERT_EQ(r1.nack_calls, 1);
    ASSERT_EQ(r1.last_count, 2);
    ASSERT_EQ((int)r1.last_seq0, 1001);
    ASSERT_EQ(r2.nack_calls, 0);

    rtc_rtcp_router_close(&router);
    rtc_rtp_send_stream_destroy(s1);
    rtc_rtp_send_stream_destroy(s2);
}

TEST(pli_routes_to_named_sender) {
    fb_rec_t r1 = {0};
    fb_rec_t r2 = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r1);
    rtc_rtp_send_stream_t *s2 = make_stream(&r2);
    uint32_t ssrc2 = rtc_rtp_send_stream_ssrc(s2);

    rtc_rtcp_router_t router;
    ASSERT_EQ(rtc_rtcp_router_init(&router, NULL), RTC_OK);
    ASSERT_EQ(rtc_rtcp_router_add_sender(&router, rtc_rtp_send_stream_ssrc(s1), s1), RTC_OK);
    ASSERT_EQ(rtc_rtcp_router_add_sender(&router, ssrc2, s2), RTC_OK);

    uint8_t buf[64];
    size_t len = 0;
    ASSERT_EQ(rtc_rtcp_build_pli(buf, sizeof(buf), &len, 0xABCD, ssrc2), RTC_OK);

    rtc_rtcp_router_handle(&router, buf, len);
    ASSERT_EQ(r2.pli_calls, 1);
    ASSERT_EQ(r1.pli_calls, 0);

    rtc_rtcp_router_close(&router);
    rtc_rtp_send_stream_destroy(s1);
    rtc_rtp_send_stream_destroy(s2);
}

TEST(unknown_and_removed_ssrc_dropped) {
    fb_rec_t r1 = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r1);
    uint32_t ssrc1 = rtc_rtp_send_stream_ssrc(s1);

    rtc_rtcp_router_t router;
    ASSERT_EQ(rtc_rtcp_router_init(&router, NULL), RTC_OK);
    ASSERT_EQ(rtc_rtcp_router_add_sender(&router, ssrc1, s1), RTC_OK);

    uint8_t buf[256];
    size_t len = 0;
    uint16_t lost[] = {7};

    /* Unknown media SSRC: nothing routed. */
    ASSERT_EQ(rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xABCD, ssrc1 ^ 0x55, lost, 1), RTC_OK);
    rtc_rtcp_router_handle(&router, buf, len);
    ASSERT_EQ(r1.nack_calls, 0);

    /* After removal, the previously-bound SSRC is dropped too. */
    rtc_rtcp_router_remove_sender(&router, ssrc1);
    ASSERT_EQ(rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xABCD, ssrc1, lost, 1), RTC_OK);
    rtc_rtcp_router_handle(&router, buf, len);
    ASSERT_EQ(r1.nack_calls, 0);

    rtc_rtcp_router_close(&router);
    rtc_rtp_send_stream_destroy(s1);
}

int main(void) {
    printf("=== rtc_rtcp_router tests ===\n");
    RUN_TEST(nack_routes_to_named_sender);
    RUN_TEST(pli_routes_to_named_sender);
    RUN_TEST(unknown_and_removed_ssrc_dropped);
    TEST_SUMMARY();
}
