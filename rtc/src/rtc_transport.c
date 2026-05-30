/*
 * rtc_transport.c - Transport layer with threaded event loop.
 *
 * Owns the UDP socket, runs a background poller thread,
 * classifies incoming packets per RFC 7983, and dispatches
 * to registered callbacks.
 */
#include "rtc_transport.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Packet classification per RFC 7983                                 */
/* ------------------------------------------------------------------ */
static rtc_pkt_type_t transport_classify(const uint8_t *data, size_t len) {
    if (len == 0)
        return RTC_PKT_UNKNOWN;

    uint8_t b = data[0];
    if (b <= 3)
        return RTC_PKT_STUN;
    if (b >= 64 && b <= 79)
        return RTC_PKT_CHANNEL_DATA; /* TURN */
    if (b >= 20 && b <= 63)
        return RTC_PKT_DTLS;
    if (b >= 128 && b <= 191) {
        /* Distinguish RTCP (PT 200-206 in byte[1]) from RTP */
        if (len >= 2 && data[1] >= 200 && data[1] <= 206)
            return RTC_PKT_RTCP;
        return RTC_PKT_RTP;
    }
    return RTC_PKT_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/*  Timer subsystem                                                    */
/* ------------------------------------------------------------------ */
/* Self-contained: operates on a caller-provided rtc_timer_t[] array,
 * mutex, and optional HWM counter. Transport composes one but the code
 * has no other transport coupling. */

/* Compute next poller timeout (ms) from the nearest active deadline.
 * Caller must hold the mutex. Returns a default poll interval when no
 * timers are scheduled. */
static int timer_next_timeout_ms(const rtc_timer_t *timers, int max) {
    uint64_t earliest = UINT64_MAX;
    for (int i = 0; i < max; i++) {
        if (timers[i].active && timers[i].deadline_ms < earliest)
            earliest = timers[i].deadline_ms;
    }
    if (earliest == UINT64_MAX)
        return 100; /* default poll interval when no timers */

    uint64_t now = rtc_time_ms();
    if (now >= earliest)
        return 0;

    uint64_t diff = earliest - now;
    return (diff > (uint64_t)INT32_MAX) ? INT32_MAX : (int)diff;
}

/* Fire any expired timers. Collects under the lock, then invokes callbacks
 * outside it so user code cannot deadlock against the timer subsystem. */
static void timer_fire_expired(rtc_timer_t *timers, int max, rtc_mutex_t *mutex) {
    uint64_t now = rtc_time_ms();

    rtc_timer_fn fns[RTC_TRANSPORT_MAX_TIMERS];
    void *users[RTC_TRANSPORT_MAX_TIMERS];
    int count = 0;

    rtc_mutex_lock(mutex);
    for (int i = 0; i < max; i++) {
        if (timers[i].active && now >= timers[i].deadline_ms) {
            fns[count] = timers[i].fn;
            users[count] = timers[i].user;
            count++;
            timers[i].active = false;
        }
    }
    rtc_mutex_unlock(mutex);

    for (int i = 0; i < count; i++)
        fns[i](users[i]);
}

/* Allocate a timer slot. Returns slot index >= 0 or -1 if all are in use.
 * Updates *hwm with the new high-water mark of slots in use. */
static rtc_timer_id_t timer_add(rtc_timer_t *timers, int max, rtc_mutex_t *mutex, int *hwm,
                                uint64_t deadline_ms, rtc_timer_fn fn, void *user) {
    rtc_mutex_lock(mutex);
    int free_slot = -1;
    int in_use = 0;
    for (int i = 0; i < max; i++) {
        if (timers[i].active) {
            in_use++;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        rtc_mutex_unlock(mutex);
        return -1;
    }
    timers[free_slot].deadline_ms = deadline_ms;
    timers[free_slot].fn = fn;
    timers[free_slot].user = user;
    timers[free_slot].active = true;
    in_use++;
    if (hwm && in_use > *hwm)
        *hwm = in_use;
    rtc_mutex_unlock(mutex);
    return free_slot;
}

static void timer_cancel(rtc_timer_t *timers, int max, rtc_mutex_t *mutex, rtc_timer_id_t id) {
    if (id < 0 || id >= max)
        return;
    rtc_mutex_lock(mutex);
    timers[id].active = false;
    rtc_mutex_unlock(mutex);
}

/* ------------------------------------------------------------------ */
/*  Transport thread                                                   */
/* ------------------------------------------------------------------ */
static void *transport_thread_fn(void *arg) {
    rtc_transport_t *t = (rtc_transport_t *)arg;
    uint8_t buf[RTC_TRANSPORT_BUF_SIZE];

    while (t->running) {
        rtc_mutex_lock(&t->mutex);
        int timeout = timer_next_timeout_ms(t->timers, RTC_TRANSPORT_MAX_TIMERS);
        rtc_mutex_unlock(&t->mutex);

        int n = rtc_poller_wait(&t->poller, timeout);

        if (n > 0) {
            rtc_socket_t s = atomic_load_explicit(&t->sock, memory_order_acquire);
            for (int i = 0; i < RTC_TRANSPORT_RECV_BATCH; i++) {
                struct sockaddr_storage from_store;
                socklen_t fromlen = sizeof(from_store);

                ssize_t len = recvfrom(s, (char *)buf, sizeof(buf), 0,
                                       (struct sockaddr *)&from_store, &fromlen);
                if (len <= 0)
                    break; /* EWOULDBLOCK or error — socket drained */

                /* Truncation heuristic: a perfectly-full buffer is
                 * suspicious since RTC_TRANSPORT_BUF_SIZE is sized for
                 * jumbo frames. Real WebRTC traffic never reaches this. */
                if ((size_t)len == sizeof(buf))
                    RTC_LOG_WARN("Transport: recvfrom filled buffer (%zd bytes) — "
                                 "possible packet truncation", len);

                rtc_addr_t from;
                memcpy(&from.addr, &from_store, fromlen);
                from.len = fromlen;

                rtc_pkt_type_t type = transport_classify(buf, (size_t)len);

                if (t->on_recv) {
                    t->on_recv(type, buf, (size_t)len, &from, t->recv_user);
                }
            }
        }

        timer_fire_expired(t->timers, RTC_TRANSPORT_MAX_TIMERS, &t->mutex);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int rtc_transport_init(rtc_transport_t *t, rtc_transport_recv_fn on_recv, void *user) {
    memset(t, 0, sizeof(*t));
    atomic_store_explicit(&t->sock, RTC_INVALID_SOCKET, memory_order_relaxed);

    /* Create UDP socket */
    rtc_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == RTC_INVALID_SOCKET) {
        RTC_LOG_ERR("Transport: failed to create socket");
        return RTC_ERR_SOCKET;
    }

    /* Bind to any available port */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        RTC_LOG_ERR("Transport: bind failed");
        rtc_close_socket(s);
        return RTC_ERR_SOCKET;
    }

    /* Store local address. Pass the full storage size so future IPv6
     * sockets get the correct length written back by getsockname. */
    t->local_addr.len = sizeof(t->local_addr.addr);
    if (getsockname(s, (struct sockaddr *)&t->local_addr.addr, &t->local_addr.len) != 0) {
        rtc_close_socket(s);
        return RTC_ERR_SOCKET;
    }

    /* Set non-blocking for use with poller */
    int rc = rtc_set_nonblocking(s);
    if (rc != RTC_OK) {
        rtc_close_socket(s);
        return rc;
    }

    /* Request larger kernel socket buffers to absorb video bursts and
     * avoid drops between drain iterations. The kernel may clamp; we log
     * the actual value so misconfiguration is visible. */
    int wanted = RTC_TRANSPORT_SOCKBUF_BYTES;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&wanted, sizeof(wanted));
    (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&wanted, sizeof(wanted));
    int got_rcv = 0, got_snd = 0;
    socklen_t optlen = sizeof(int);
    (void)getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&got_rcv, &optlen);
    optlen = sizeof(int);
    (void)getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&got_snd, &optlen);
    if (got_rcv > 0 && got_rcv < wanted)
        RTC_LOG_WARN("Transport: SO_RCVBUF clamped to %d (requested %d)", got_rcv, wanted);
    if (got_snd > 0 && got_snd < wanted)
        RTC_LOG_WARN("Transport: SO_SNDBUF clamped to %d (requested %d)", got_snd, wanted);

    /* Init poller and register socket */
    rc = rtc_poller_init(&t->poller);
    if (rc != RTC_OK) {
        rtc_close_socket(s);
        return rc;
    }

    rc = rtc_poller_add(&t->poller, s);
    if (rc != RTC_OK) {
        rtc_poller_close(&t->poller);
        rtc_close_socket(s);
        return rc;
    }

    /* Init synchronization */
    rtc_mutex_init(&t->mutex);

    /* Wire up callback before the thread starts so no packet is missed. */
    t->on_recv = on_recv;
    t->recv_user = user;

    /* Publish socket and running flag, then start background thread. */
    atomic_store_explicit(&t->sock, s, memory_order_release);
    atomic_store_explicit(&t->running, true, memory_order_release);

    rc = rtc_thread_create(&t->thread, transport_thread_fn, t);
    if (rc != RTC_OK) {
        atomic_store_explicit(&t->running, false, memory_order_relaxed);
        atomic_store_explicit(&t->sock, RTC_INVALID_SOCKET, memory_order_relaxed);
        rtc_poller_close(&t->poller);
        rtc_close_socket(s);
        rtc_mutex_destroy(&t->mutex);
        return rc;
    }

    char ipbuf[64];
    uint16_t port = 0;
    if (rtc_addr_to_string(&t->local_addr, ipbuf, sizeof(ipbuf), &port) == RTC_OK)
        RTC_LOG_INFO("Transport: bound %s:%u (rcvbuf=%d sndbuf=%d)", ipbuf, port, got_rcv,
                     got_snd);
    else
        RTC_LOG_INFO("Transport: initialized (rcvbuf=%d sndbuf=%d)", got_rcv, got_snd);
    return RTC_OK;
}

int rtc_transport_send(rtc_transport_t *t, const uint8_t *data, size_t len,
                       const rtc_addr_t *dest) {
    /* Snapshot fd: lets a send racing with close return an error instead
     * of using a freed/reused descriptor. Caller is still expected to
     * stop sending before/around close — this just hardens the boundary. */
    rtc_socket_t s = atomic_load_explicit(&t->sock, memory_order_acquire);
    if (s == RTC_INVALID_SOCKET)
        return RTC_ERR_SOCKET;

    ssize_t sent = sendto(s, (const char *)data, (int)len, 0,
                          (const struct sockaddr *)&dest->addr, dest->len);
    return (sent > 0) ? RTC_OK : RTC_ERR_SOCKET;
}

int rtc_transport_send_to_remote(rtc_transport_t *t, const uint8_t *data, size_t len) {
    /* Acquire-load pairs with the release-store in rtc_transport_set_remote;
     * seeing true guarantees a fully-written remote_addr. */
    if (!atomic_load_explicit(&t->remote_addr_set, memory_order_acquire))
        return RTC_ERR_INVALID;
    return rtc_transport_send(t, data, len, &t->remote_addr);
}

void rtc_transport_set_remote(rtc_transport_t *t, const rtc_addr_t *addr) {
    /* One-shot publication. Write address first, then release-store the
     * flag so any sender observing true also sees the address. */
    t->remote_addr = *addr;
    atomic_store_explicit(&t->remote_addr_set, true, memory_order_release);
}

rtc_timer_id_t rtc_transport_add_timer(rtc_transport_t *t, uint64_t deadline_ms, rtc_timer_fn fn,
                                       void *user) {
    rtc_timer_id_t id = timer_add(t->timers, RTC_TRANSPORT_MAX_TIMERS, &t->mutex,
                                  &t->timer_slot_hwm, deadline_ms, fn, user);
    if (id < 0)
        RTC_LOG_WARN("Transport: no free timer slots (max=%d) — caller will"
                     " silently lose this scheduling; raise RTC_TRANSPORT_MAX_TIMERS",
                     RTC_TRANSPORT_MAX_TIMERS);
    return id;
}

void rtc_transport_cancel_timer(rtc_transport_t *t, rtc_timer_id_t id) {
    timer_cancel(t->timers, RTC_TRANSPORT_MAX_TIMERS, &t->mutex, id);
}

rtc_socket_t rtc_transport_get_socket(const rtc_transport_t *t) {
    return atomic_load_explicit(&t->sock, memory_order_acquire);
}

int rtc_transport_get_local_addr(rtc_transport_t *t, rtc_addr_t *out) {
    *out = t->local_addr;
    return RTC_OK;
}

void rtc_transport_close(rtc_transport_t *t) {
    /* Idempotent: only the thread that flips running true -> false runs
     * the shutdown sequence. Concurrent / repeated close calls no-op. */
    bool was_running = atomic_exchange_explicit(&t->running, false, memory_order_acq_rel);
    if (!was_running)
        return;

    /* Join thread (it observes running==false and exits its loop). */
    rtc_thread_join(&t->thread);

    /* Cleanup */
    rtc_poller_close(&t->poller);

    rtc_socket_t s = atomic_exchange_explicit(&t->sock, RTC_INVALID_SOCKET, memory_order_acq_rel);
    if (s != RTC_INVALID_SOCKET)
        rtc_close_socket(s);

    rtc_mutex_destroy(&t->mutex);

    RTC_LOG_INFO("Transport: closed (timer slot hwm=%d/%d)", t->timer_slot_hwm,
                 RTC_TRANSPORT_MAX_TIMERS);
}
