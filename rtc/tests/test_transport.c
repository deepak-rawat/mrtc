/*
 * test_transport.c - Transport layer tests.
 *
 * Tests:
 *   1. Init/close lifecycle: socket created, thread runs, close joins
 *   2. Packet send/recv: send UDP packet to transport, verify callback fires
 *   3. Packet classification: STUN, DTLS, RTP correctly classified
 *   4. Timer: add timer, verify it fires after deadline
 *   5. Timer cancel: add timer, cancel it, verify it doesn't fire
 *   6. Send to remote: set remote address, send via transport
 */
#include <rtc/rtc.h>
#include "rtc_transport.h"
#include "test_harness.h"

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ------------------------------------------------------------------ */
/*  Shared test state for callbacks                                    */
/* ------------------------------------------------------------------ */
static volatile int g_recv_count;
static volatile rtc_pkt_type_t g_recv_type;
static volatile size_t g_recv_len;
static uint8_t g_recv_buf[256];
static rtc_mutex_t g_mutex;
static rtc_cond_t g_cond;

static void test_recv_callback(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                               const rtc_addr_t *from, void *user) {
    (void)from;
    (void)user;
    rtc_mutex_lock(&g_mutex);
    g_recv_type = type;
    g_recv_len = len;
    if (len <= sizeof(g_recv_buf))
        memcpy(g_recv_buf, data, len);
    g_recv_count++;
    rtc_cond_signal(&g_cond);
    rtc_mutex_unlock(&g_mutex);
}

/* Wait for recv_count to reach target, with timeout */
static bool wait_for_recv(int target, int timeout_ms) {
    rtc_mutex_lock(&g_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_recv_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_cond, &g_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_mutex);
    return true;
}

static volatile int g_timer_count;

static void test_timer_callback(void *user) {
    (void)user;
    rtc_mutex_lock(&g_mutex);
    g_timer_count++;
    rtc_cond_signal(&g_cond);
    rtc_mutex_unlock(&g_mutex);
}

static bool wait_for_timer(int target, int timeout_ms) {
    rtc_mutex_lock(&g_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_timer_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_cond, &g_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_mutex);
    return true;
}

/* Helper: get the transport's local address for sending to self */
static void get_loopback_addr(rtc_transport_t *t, rtc_addr_t *out) {
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(rtc_transport_get_socket(t), (struct sockaddr *)&local, &len);

    memset(out, 0, sizeof(*out));
    struct sockaddr_in *s = (struct sockaddr_in *)&out->addr;
    s->sin_family = AF_INET;
    s->sin_port = local.sin_port;
    s->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    out->len = sizeof(struct sockaddr_in);
}

/* ------------------------------------------------------------------ */
/*  Test: init and close lifecycle                                     */
/* ------------------------------------------------------------------ */
TEST(transport_init_close) {
    rtc_transport_t t;
    int rc = rtc_transport_init(&t);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(rtc_transport_get_socket(&t) != RTC_INVALID_SOCKET);
    ASSERT(t.running);

    /* Verify socket is bound (has a port) */
    rtc_addr_t addr;
    rc = rtc_transport_get_local_addr(&t, &addr);
    ASSERT_EQ(rc, RTC_OK);

    struct sockaddr_in *sin = (struct sockaddr_in *)&addr.addr;
    ASSERT(ntohs(sin->sin_port) > 0);

    printf("    transport bound to port %u\n", ntohs(sin->sin_port));

    rtc_transport_close(&t);
    ASSERT(!t.running);

    printf("    init -> close lifecycle OK\n");
}

/* ------------------------------------------------------------------ */
/*  Test: send packet to transport, verify callback fires              */
/* ------------------------------------------------------------------ */
TEST(transport_recv_packet) {
    rtc_transport_t t;
    int rc = rtc_transport_init(&t);
    ASSERT_EQ(rc, RTC_OK);

    g_recv_count = 0;
    rtc_transport_set_recv_callback(&t, test_recv_callback, NULL);

    /* Send a STUN-like packet (first byte 0x00) to transport's own port */
    rtc_addr_t self;
    get_loopback_addr(&t, &self);

    uint8_t stun_pkt[20];
    memset(stun_pkt, 0, sizeof(stun_pkt));
    stun_pkt[4] = 0x21;
    stun_pkt[5] = 0x12; /* magic cookie */
    stun_pkt[6] = 0xA4;
    stun_pkt[7] = 0x42;

    rc = rtc_transport_send(&t, stun_pkt, sizeof(stun_pkt), &self);
    ASSERT_EQ(rc, RTC_OK);

    /* Wait for callback */
    bool got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_STUN);
    ASSERT_EQ((int)g_recv_len, 20);

    printf("    STUN packet received and classified correctly\n");
    rtc_transport_close(&t);
}

/* ------------------------------------------------------------------ */
/*  Test: packet classification for different types                    */
/* ------------------------------------------------------------------ */
TEST(transport_classify_types) {
    rtc_transport_t t;
    int rc = rtc_transport_init(&t);
    ASSERT_EQ(rc, RTC_OK);
    rtc_transport_set_recv_callback(&t, test_recv_callback, NULL);

    rtc_addr_t self;
    get_loopback_addr(&t, &self);

    /* DTLS packet (first byte = 22 = ChangeCipherSpec) */
    g_recv_count = 0;
    uint8_t dtls_pkt[13];
    memset(dtls_pkt, 0, sizeof(dtls_pkt));
    dtls_pkt[0] = 22; /* DTLS content type: Handshake */
    rc = rtc_transport_send(&t, dtls_pkt, sizeof(dtls_pkt), &self);
    ASSERT_EQ(rc, RTC_OK);

    bool got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_DTLS);
    printf("    DTLS packet classified correctly\n");

    /* RTP packet (first byte = 0x80 = version 2) */
    g_recv_count = 0;
    uint8_t rtp_pkt[12];
    memset(rtp_pkt, 0, sizeof(rtp_pkt));
    rtp_pkt[0] = 0x80; /* RTP version 2 */
    rc = rtc_transport_send(&t, rtp_pkt, sizeof(rtp_pkt), &self);
    ASSERT_EQ(rc, RTC_OK);

    got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_RTP);
    printf("    RTP packet classified correctly\n");

    rtc_transport_close(&t);
}

/* ------------------------------------------------------------------ */
/*  Test: timer fires after deadline                                   */
/* ------------------------------------------------------------------ */
TEST(transport_timer_fires) {
    rtc_transport_t t;
    int rc = rtc_transport_init(&t);
    ASSERT_EQ(rc, RTC_OK);

    g_timer_count = 0;
    uint64_t now = rtc_time_ms();
    rtc_timer_id_t id = rtc_transport_add_timer(&t, now + 100, test_timer_callback, NULL);
    ASSERT(id >= 0);

    /* Wait for timer (should fire within ~200ms) */
    bool fired = wait_for_timer(1, 500);
    ASSERT(fired);
    ASSERT_EQ(g_timer_count, 1);

    printf("    timer fired after ~100ms\n");
    rtc_transport_close(&t);
}

/* ------------------------------------------------------------------ */
/*  Test: cancelled timer doesn't fire                                 */
/* ------------------------------------------------------------------ */
TEST(transport_timer_cancel) {
    rtc_transport_t t;
    int rc = rtc_transport_init(&t);
    ASSERT_EQ(rc, RTC_OK);

    g_timer_count = 0;
    uint64_t now = rtc_time_ms();
    rtc_timer_id_t id = rtc_transport_add_timer(&t, now + 100, test_timer_callback, NULL);
    ASSERT(id >= 0);

    /* Cancel immediately */
    rtc_transport_cancel_timer(&t, id);

    /* Wait longer than the timer would have fired */
    SLEEP_MS(300);
    ASSERT_EQ(g_timer_count, 0);

    printf("    cancelled timer did not fire\n");
    rtc_transport_close(&t);
}

/* ------------------------------------------------------------------ */
/*  Test: send to remote via transport_send_to_remote                  */
/* ------------------------------------------------------------------ */
TEST(transport_send_to_remote) {
    rtc_transport_t sender, receiver;
    int rc = rtc_transport_init(&sender);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_transport_init(&receiver);
    ASSERT_EQ(rc, RTC_OK);

    g_recv_count = 0;
    rtc_transport_set_recv_callback(&receiver, test_recv_callback, NULL);

    /* Get receiver's loopback address */
    rtc_addr_t recv_addr;
    get_loopback_addr(&receiver, &recv_addr);

    /* Set as sender's remote */
    rtc_transport_set_remote(&sender, &recv_addr);

    /* Send RTP-like packet */
    uint8_t rtp_pkt[20];
    memset(rtp_pkt, 0, sizeof(rtp_pkt));
    rtp_pkt[0] = 0x80; /* RTP version 2 */
    rc = rtc_transport_send_to_remote(&sender, rtp_pkt, sizeof(rtp_pkt));
    ASSERT_EQ(rc, RTC_OK);

    bool got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_RTP);
    ASSERT_EQ((int)g_recv_len, 20);

    printf("    sender -> receiver via send_to_remote OK\n");

    rtc_transport_close(&sender);
    rtc_transport_close(&receiver);
}

/* ------------------------------------------------------------------ */
/*  Test: RTCP classified separately from RTP                          */
/* ------------------------------------------------------------------ */
TEST(transport_classify_rtcp) {
    rtc_transport_t t;
    int rc = rtc_transport_init(&t);
    ASSERT_EQ(rc, RTC_OK);
    rtc_transport_set_recv_callback(&t, test_recv_callback, NULL);

    rtc_addr_t self;
    get_loopback_addr(&t, &self);

    /* RTCP Sender Report: first byte 0x80 (V=2, P=0, RC=0), second byte 200 (SR) */
    g_recv_count = 0;
    uint8_t sr_pkt[28];
    memset(sr_pkt, 0, sizeof(sr_pkt));
    sr_pkt[0] = 0x80; /* version=2 */
    sr_pkt[1] = 200;  /* PT = SR */
    sr_pkt[2] = 0x00;
    sr_pkt[3] = 0x06; /* length = 6 words */
    rc = rtc_transport_send(&t, sr_pkt, sizeof(sr_pkt), &self);
    ASSERT_EQ(rc, RTC_OK);

    bool got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_RTCP);
    printf("    RTCP SR classified as RTC_PKT_RTCP\n");

    /* RTCP Receiver Report: byte[1] = 201 */
    g_recv_count = 0;
    uint8_t rr_pkt[8];
    memset(rr_pkt, 0, sizeof(rr_pkt));
    rr_pkt[0] = 0x80;
    rr_pkt[1] = 201; /* PT = RR */
    rr_pkt[2] = 0x00;
    rr_pkt[3] = 0x01;
    rc = rtc_transport_send(&t, rr_pkt, sizeof(rr_pkt), &self);
    ASSERT_EQ(rc, RTC_OK);

    got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_RTCP);
    printf("    RTCP RR classified as RTC_PKT_RTCP\n");

    /* Regular RTP (byte[1] = 96, PT < 200) should still be RTP */
    g_recv_count = 0;
    uint8_t rtp_pkt[12];
    memset(rtp_pkt, 0, sizeof(rtp_pkt));
    rtp_pkt[0] = 0x80; /* version=2 */
    rtp_pkt[1] = 96;   /* PT = 96 (dynamic) */
    rc = rtc_transport_send(&t, rtp_pkt, sizeof(rtp_pkt), &self);
    ASSERT_EQ(rc, RTC_OK);

    got = wait_for_recv(1, 2000);
    ASSERT(got);
    ASSERT_EQ((int)g_recv_type, (int)RTC_PKT_RTP);
    printf("    RTP with PT=96 still classified as RTP\n");

    rtc_transport_close(&t);
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Transport Layer Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);
    rtc_mutex_init(&g_mutex);
    rtc_cond_init(&g_cond);

    RUN_TEST(transport_init_close);
    RUN_TEST(transport_recv_packet);
    RUN_TEST(transport_classify_types);
    RUN_TEST(transport_timer_fires);
    RUN_TEST(transport_timer_cancel);
    RUN_TEST(transport_send_to_remote);
    RUN_TEST(transport_classify_rtcp);

    rtc_cond_destroy(&g_cond);
    rtc_mutex_destroy(&g_mutex);
    rtc_cleanup();
    TEST_SUMMARY();
}
