/*
 * Unit tests for the RTP/RTCP media session (rtc_media_session).
 *
 * Exercises SSRC -> send-stream routing for NACK / PLI feedback with a NULL
 * transport / worker (SR routing, the RR loss feed, and the emission timer
 * require a live transport and are covered end-to-end by the client peer test).
 */
#include "rtc/rtc_rtcp.h"
#include "rtc/rtc_media_session.h"
#include "rtc/rtc_rtp_ext.h"
#include "rtc/rtc_rtp_stream.h"
#include "test_harness.h"

#include <stdlib.h>

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

    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s1), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s2), RTC_OK);

    uint8_t buf[256];
    size_t len = 0;
    uint16_t lost[] = {1001, 1002};
    ASSERT_EQ(rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xABCD, ssrc1, lost, 2), RTC_OK);

    rtc_media_session_handle_rtcp(&session, buf, len);
    ASSERT_EQ(r1.nack_calls, 1);
    ASSERT_EQ(r1.last_count, 2);
    ASSERT_EQ((int)r1.last_seq0, 1001);
    ASSERT_EQ(r2.nack_calls, 0);

    rtc_media_session_close(&session);
    rtc_rtp_send_stream_destroy(s1);
    rtc_rtp_send_stream_destroy(s2);
}

TEST(pli_routes_to_named_sender) {
    fb_rec_t r1 = {0};
    fb_rec_t r2 = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r1);
    rtc_rtp_send_stream_t *s2 = make_stream(&r2);
    uint32_t ssrc2 = rtc_rtp_send_stream_ssrc(s2);

    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s1), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s2), RTC_OK);

    uint8_t buf[64];
    size_t len = 0;
    ASSERT_EQ(rtc_rtcp_build_pli(buf, sizeof(buf), &len, 0xABCD, ssrc2), RTC_OK);

    rtc_media_session_handle_rtcp(&session, buf, len);
    ASSERT_EQ(r2.pli_calls, 1);
    ASSERT_EQ(r1.pli_calls, 0);

    rtc_media_session_close(&session);
    rtc_rtp_send_stream_destroy(s1);
    rtc_rtp_send_stream_destroy(s2);
}

TEST(unknown_ssrc_dropped) {
    fb_rec_t r1 = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r1);
    uint32_t ssrc1 = rtc_rtp_send_stream_ssrc(s1);

    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s1), RTC_OK);

    uint8_t buf[256];
    size_t len = 0;
    uint16_t lost[] = {7};

    /* Unknown media SSRC: nothing routed. */
    ASSERT_EQ(rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xABCD, ssrc1 ^ 0x55, lost, 1), RTC_OK);
    rtc_media_session_handle_rtcp(&session, buf, len);
    ASSERT_EQ(r1.nack_calls, 0);

    rtc_media_session_close(&session);
    rtc_rtp_send_stream_destroy(s1);
}

TEST(compound_rtcp_dispatches_all_subpackets) {
    fb_rec_t r1 = {0};
    fb_rec_t r2 = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r1);
    rtc_rtp_send_stream_t *s2 = make_stream(&r2);
    uint32_t ssrc1 = rtc_rtp_send_stream_ssrc(s1);
    uint32_t ssrc2 = rtc_rtp_send_stream_ssrc(s2);

    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s1), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s2), RTC_OK);

    /* One datagram carrying NACK(s1) followed by PLI(s2). The previous
     * single-packet dispatch processed only the first sub-packet. */
    uint8_t buf[256];
    size_t total = 0;
    size_t len = 0;
    uint16_t lost[] = {2001, 2002};
    ASSERT_EQ(rtc_rtcp_build_nack(buf, sizeof(buf), &len, 0xABCD, ssrc1, lost, 2), RTC_OK);
    total += len;
    ASSERT_EQ(rtc_rtcp_build_pli(buf + total, sizeof(buf) - total, &len, 0xABCD, ssrc2), RTC_OK);
    total += len;

    rtc_media_session_handle_rtcp(&session, buf, total);

    ASSERT_EQ(r1.nack_calls, 1);
    ASSERT_EQ(r1.last_count, 2);
    ASSERT_EQ((int)r1.last_seq0, 2001);
    ASSERT_EQ(r2.pli_calls, 1);
    ASSERT_EQ(r1.pli_calls, 0);
    ASSERT_EQ(r2.nack_calls, 0);

    rtc_media_session_close(&session);
    rtc_rtp_send_stream_destroy(s1);
    rtc_rtp_send_stream_destroy(s2);
}

/* A custom interceptor an application might append (e.g. stats / REMB). */
typedef struct {
    rtc_interceptor_t base;
    int *rtcp_seen;
    int *destroyed;
} count_it_t;

static void count_on_rtcp(rtc_interceptor_t *it, uint8_t pt, uint8_t fmt, const uint8_t *buf,
                          size_t len) {
    (void)pt;
    (void)fmt;
    (void)buf;
    (void)len;
    (*((count_it_t *)it)->rtcp_seen)++;
}

static void count_destroy(rtc_interceptor_t *it) {
    (*((count_it_t *)it)->destroyed)++;
    free(it);
}

static const rtc_interceptor_ops_t count_ops = {
    .name = "count",
    .on_rtcp = count_on_rtcp,
    .destroy = count_destroy,
};

TEST(custom_interceptor_sees_rtcp_and_is_freed) {
    fb_rec_t r = {0};
    rtc_rtp_send_stream_t *s1 = make_stream(&r);
    uint32_t ssrc1 = rtc_rtp_send_stream_ssrc(s1);

    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_sender(&session, s1), RTC_OK);

    int rtcp_seen = 0;
    int destroyed = 0;
    count_it_t *ci = (count_it_t *)calloc(1, sizeof(*ci));
    ci->base.ops = &count_ops;
    ci->rtcp_seen = &rtcp_seen;
    ci->destroyed = &destroyed;
    ASSERT_EQ(rtc_media_session_add_interceptor(&session, &ci->base), RTC_OK);

    /* A PLI still routes to the built-in PLI interceptor, and the custom one
     * observes it too. */
    uint8_t buf[64];
    size_t len = 0;
    ASSERT_EQ(rtc_rtcp_build_pli(buf, sizeof(buf), &len, 0xABCD, ssrc1), RTC_OK);
    rtc_media_session_handle_rtcp(&session, buf, len);

    ASSERT_EQ(rtcp_seen, 1);
    ASSERT_EQ(r.pli_calls, 1);

    rtc_media_session_close(&session);
    ASSERT_EQ(destroyed, 1);

    rtc_rtp_send_stream_destroy(s1);
}

static void noop_frame(const uint8_t *p, size_t l, uint16_t s, uint32_t t, uint32_t ssrc, bool m,
                       void *u) {
    (void)p;
    (void)l;
    (void)s;
    (void)t;
    (void)ssrc;
    (void)m;
    (void)u;
}

static rtc_rtp_recv_stream_t *make_recv(uint8_t pt, const char *mid) {
    rtc_rtp_recv_stream_config_t cfg = {.payload_type = pt, .clock_rate = 90000, .local_ssrc = 1};
    rtc_rtp_recv_stream_t *r = rtc_rtp_recv_stream_create(&cfg);
    rtc_rtp_recv_stream_set_active(r, true);
    rtc_rtp_recv_stream_on_frame(r, noop_frame, NULL);
    if (mid)
        rtc_rtp_recv_stream_set_mid(r, mid);
    return r;
}

TEST(resolve_routes_same_pt_by_mid) {
    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    rtc_rtp_recv_stream_t *r0 = make_recv(96, "0");
    rtc_rtp_recv_stream_t *r1 = make_recv(96, "1");
    ASSERT_EQ(rtc_media_session_add_receiver(&session, r0), RTC_OK);
    ASSERT_EQ(rtc_media_session_add_receiver(&session, r1), RTC_OK);
    rtc_media_session_set_mid_ext_id(&session, 4);

    /* PT 96 + MID="1" must route to r1, even though r0 also has PT 96. */
    rtc_rtp_ext_t exts[1];
    rtc_rtp_ext_make_string(&exts[0], 4, "1");
    const uint8_t payload[] = {0x01};
    rtc_rtp_packet_t pkt, parsed;
    ASSERT_EQ(rtc_rtp_build_with_ext(&pkt, 96, 1, 0, 0xAAAA0001, false, exts, 1, payload, 1),
              RTC_OK);
    ASSERT_EQ(rtc_rtp_parse(&parsed, pkt.buf, pkt.buf_len), RTC_OK);
    ASSERT(rtc_media_session_resolve(&session, &parsed) == r1);
    ASSERT_EQ(rtc_rtp_recv_stream_ssrc(r1), 0xAAAA0001u);

    /* MID="0" must route to r0. */
    rtc_rtp_ext_make_string(&exts[0], 4, "0");
    ASSERT_EQ(rtc_rtp_build_with_ext(&pkt, 96, 2, 0, 0xAAAA0000, false, exts, 1, payload, 1),
              RTC_OK);
    ASSERT_EQ(rtc_rtp_parse(&parsed, pkt.buf, pkt.buf_len), RTC_OK);
    ASSERT(rtc_media_session_resolve(&session, &parsed) == r0);

    rtc_media_session_close(&session);
    rtc_rtp_recv_stream_destroy(r0);
    rtc_rtp_recv_stream_destroy(r1);
}

TEST(resolve_falls_back_to_payload_type) {
    rtc_media_session_t session;
    ASSERT_EQ(rtc_media_session_init(&session, NULL, NULL), RTC_OK);
    rtc_rtp_recv_stream_t *r = make_recv(96, "0");
    ASSERT_EQ(rtc_media_session_add_receiver(&session, r), RTC_OK);
    rtc_media_session_set_mid_ext_id(&session, 4);

    /* No MID extension present: fall back to payload-type match. */
    const uint8_t payload[] = {0x01};
    rtc_rtp_packet_t pkt, parsed;
    ASSERT_EQ(rtc_rtp_build(&pkt, 96, 1, 0, 0xBBBB0001, false, payload, 1), RTC_OK);
    ASSERT_EQ(rtc_rtp_parse(&parsed, pkt.buf, pkt.buf_len), RTC_OK);
    ASSERT(rtc_media_session_resolve(&session, &parsed) == r);

    rtc_media_session_close(&session);
    rtc_rtp_recv_stream_destroy(r);
}

int main(void) {
    printf("=== rtc_media_session tests ===\n");
    RUN_TEST(nack_routes_to_named_sender);
    RUN_TEST(pli_routes_to_named_sender);
    RUN_TEST(unknown_ssrc_dropped);
    RUN_TEST(compound_rtcp_dispatches_all_subpackets);
    RUN_TEST(custom_interceptor_sees_rtcp_and_is_freed);
    RUN_TEST(resolve_routes_same_pt_by_mid);
    RUN_TEST(resolve_falls_back_to_payload_type);
    TEST_SUMMARY();
}
