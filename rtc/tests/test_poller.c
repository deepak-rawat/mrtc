/*
 * rtc_poller tests.
 *
 * Covers:
 *   1. Init/close lifecycle
 *   2. Add + remove sockets
 *   3. Wait with no readiness times out
 *   4. Wait reports the correct fd when a packet arrives
 *   5. rtc_poller_wake unblocks a wait() in another thread
 *   6. Coalesced wakes (two wake() calls produce >=1 unblocks, do not deadlock)
 *   7. Max-events capacity is respected
 *   8. Wake events do not appear in events[]
 */
#include "rtc_poller.h"
#include "rtc/rtc.h"
#include "test_harness.h"

#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

/* Helper: create a bound UDP socket on loopback, return its addr. */
static rtc_socket_t make_udp_loopback(rtc_addr_t *addr_out) {
    rtc_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == RTC_INVALID_SOCKET)
        return RTC_INVALID_SOCKET;

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        rtc_close_socket(s);
        return RTC_INVALID_SOCKET;
    }
    if (rtc_set_nonblocking(s) != RTC_OK) {
        rtc_close_socket(s);
        return RTC_INVALID_SOCKET;
    }
    if (addr_out) {
        addr_out->len = sizeof(struct sockaddr_in);
        if (getsockname(s, (struct sockaddr *)&addr_out->addr, &addr_out->len) != 0) {
            rtc_close_socket(s);
            return RTC_INVALID_SOCKET;
        }
    }
    return s;
}

TEST(poller_init_close) {
    rtc_poller_t p;
    int rc = rtc_poller_init(&p);
    ASSERT_EQ(rc, RTC_OK);
    rtc_poller_close(&p);
    /* Double-close must not crash. */
    rtc_poller_close(&p);
    printf("    init/close OK\n");
}

TEST(poller_add_remove) {
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    rtc_socket_t s = make_udp_loopback(NULL);
    ASSERT(s != RTC_INVALID_SOCKET);

    ASSERT_EQ(rtc_poller_add(&p, s), RTC_OK);
    ASSERT_EQ(rtc_poller_remove(&p, s), RTC_OK);

    rtc_close_socket(s);
    rtc_poller_close(&p);
    printf("    add/remove OK\n");
}

TEST(poller_wait_timeout) {
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    rtc_socket_t s = make_udp_loopback(NULL);
    ASSERT(s != RTC_INVALID_SOCKET);
    ASSERT_EQ(rtc_poller_add(&p, s), RTC_OK);

    rtc_poller_event_t evs[RTC_POLLER_MAX_EVENTS];
    uint64_t t0 = rtc_time_ms();
    int n = rtc_poller_wait(&p, evs, RTC_POLLER_MAX_EVENTS, 50);
    uint64_t elapsed = rtc_time_ms() - t0;

    ASSERT_EQ(n, 0);
    /* Allow generous slack for CI; we only care that it actually slept. */
    ASSERT(elapsed >= 40);
    printf("    wait timeout returned 0 after %llu ms\n", (unsigned long long)elapsed);

    rtc_poller_remove(&p, s);
    rtc_close_socket(s);
    rtc_poller_close(&p);
}

TEST(poller_wait_reports_ready_fd) {
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    rtc_addr_t addr;
    rtc_socket_t s = make_udp_loopback(&addr);
    ASSERT(s != RTC_INVALID_SOCKET);
    ASSERT_EQ(rtc_poller_add(&p, s), RTC_OK);

    /* Send a packet to ourselves. */
    rtc_socket_t sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT(sender != RTC_INVALID_SOCKET);
    const char *msg = "hi";
    int sent = sendto(sender, msg, 2, 0, (const struct sockaddr *)&addr.addr, addr.len);
    ASSERT_EQ(sent, 2);

    rtc_poller_event_t evs[RTC_POLLER_MAX_EVENTS];
    int n = rtc_poller_wait(&p, evs, RTC_POLLER_MAX_EVENTS, 2000);
    ASSERT(n >= 1);

    /* Find our socket in the event list. */
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (evs[i].fd == s) {
            ASSERT(evs[i].events & RTC_POLLER_EV_READ);
            found = true;
        }
    }
    ASSERT(found);

    /* Drain to avoid leaking the queued datagram (not strictly needed). */
    char buf[16];
    (void)recv(s, buf, sizeof(buf), 0);

    rtc_close_socket(sender);
    rtc_poller_remove(&p, s);
    rtc_close_socket(s);
    rtc_poller_close(&p);
    printf("    wait reported ready fd correctly\n");
}

struct wake_thread_arg {
    rtc_poller_t *p;
    _Atomic int returned;
    _Atomic int n_events;
    uint64_t elapsed_ms;
};

static void *wake_wait_thread(void *arg) {
    struct wake_thread_arg *a = (struct wake_thread_arg *)arg;
    rtc_poller_event_t evs[RTC_POLLER_MAX_EVENTS];
    uint64_t t0 = rtc_time_ms();
    int n = rtc_poller_wait(a->p, evs, RTC_POLLER_MAX_EVENTS, 5000);
    a->elapsed_ms = rtc_time_ms() - t0;
    a->n_events = n;
    atomic_store(&a->returned, 1);
    return NULL;
}

TEST(poller_wake_unblocks_wait) {
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    /* No sockets added: only the wake event can unblock the wait. */
    struct wake_thread_arg a = {0};
    a.p = &p;
    rtc_thread_t th;
    ASSERT_EQ(rtc_thread_create(&th, wake_wait_thread, &a), RTC_OK);

    /* Give the wait() a chance to actually block, then wake. */
    SLEEP_MS(100);
    ASSERT(atomic_load(&a.returned) == 0);
    ASSERT_EQ(rtc_poller_wake(&p), RTC_OK);

    rtc_thread_join(&th);
    ASSERT_EQ(atomic_load(&a.returned), 1);
    ASSERT_EQ(atomic_load(&a.n_events), 0); /* wake events are invisible */
    ASSERT(a.elapsed_ms < 1000);
    printf("    wake unblocked wait in %llu ms, n_events=0 (wake hidden)\n",
           (unsigned long long)a.elapsed_ms);

    rtc_poller_close(&p);
}

TEST(poller_wake_coalesce) {
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    /* Many wakes before wait(): should not deadlock or error, and wait()
     * should return promptly with no socket events. */
    for (int i = 0; i < 50; i++)
        ASSERT_EQ(rtc_poller_wake(&p), RTC_OK);

    rtc_poller_event_t evs[RTC_POLLER_MAX_EVENTS];
    int n = rtc_poller_wait(&p, evs, RTC_POLLER_MAX_EVENTS, 1000);
    ASSERT_EQ(n, 0);

    /* Subsequent wait with no further wake should time out. */
    uint64_t t0 = rtc_time_ms();
    n = rtc_poller_wait(&p, evs, RTC_POLLER_MAX_EVENTS, 100);
    uint64_t elapsed = rtc_time_ms() - t0;
    ASSERT_EQ(n, 0);
    ASSERT(elapsed >= 80);
    printf("    50 wakes coalesced; later wait timed out in %llu ms\n",
           (unsigned long long)elapsed);

    rtc_poller_close(&p);
}

TEST(poller_wake_during_wait_with_socket) {
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    rtc_socket_t s = make_udp_loopback(NULL);
    ASSERT(s != RTC_INVALID_SOCKET);
    ASSERT_EQ(rtc_poller_add(&p, s), RTC_OK);

    struct wake_thread_arg a = {0};
    a.p = &p;
    rtc_thread_t th;
    ASSERT_EQ(rtc_thread_create(&th, wake_wait_thread, &a), RTC_OK);

    SLEEP_MS(100);
    ASSERT(atomic_load(&a.returned) == 0);
    ASSERT_EQ(rtc_poller_wake(&p), RTC_OK);

    rtc_thread_join(&th);
    ASSERT_EQ(atomic_load(&a.returned), 1);
    ASSERT_EQ(atomic_load(&a.n_events), 0);
    ASSERT(a.elapsed_ms < 1000);
    printf("    wake works alongside registered sockets\n");

    rtc_poller_remove(&p, s);
    rtc_close_socket(s);
    rtc_poller_close(&p);
}

TEST(poller_max_events_capacity) {
    /* The MAX_EVENTS bump means we must accept many concurrent fds. We
     * don't test the exact cap (would need many fds and IDs) — just
     * confirm a reasonable number can be added/removed without error. */
    rtc_poller_t p;
    ASSERT_EQ(rtc_poller_init(&p), RTC_OK);

    rtc_socket_t socks[16];
    for (int i = 0; i < 16; i++) {
        socks[i] = make_udp_loopback(NULL);
        ASSERT(socks[i] != RTC_INVALID_SOCKET);
        int rc = rtc_poller_add(&p, socks[i]);
        ASSERT_EQ(rc, RTC_OK);
    }
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(rtc_poller_remove(&p, socks[i]), RTC_OK);
        rtc_close_socket(socks[i]);
    }
    rtc_poller_close(&p);
    printf("    16 fds add/remove OK (MAX_EVENTS=%d)\n", RTC_POLLER_MAX_EVENTS);
}

int main(void) {
    printf("========================================\n");
    printf("  Poller Tests\n");
    printf("========================================\n\n");

    rtc_init();

    RUN_TEST(poller_init_close);
    RUN_TEST(poller_add_remove);
    RUN_TEST(poller_wait_timeout);
    RUN_TEST(poller_wait_reports_ready_fd);
    RUN_TEST(poller_wake_unblocks_wait);
    RUN_TEST(poller_wake_coalesce);
    RUN_TEST(poller_wake_during_wait_with_socket);
    RUN_TEST(poller_max_events_capacity);

    rtc_cleanup();
    TEST_SUMMARY();
}
