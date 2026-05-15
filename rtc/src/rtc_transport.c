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
/*  Timer helpers                                                      */
/* ------------------------------------------------------------------ */

/* Compute timeout_ms for the poller based on nearest timer deadline.
 * Must be called with mutex held. Returns -1 if no timers. */
static int transport_next_timeout_ms(rtc_transport_t *t) {
    uint64_t earliest = UINT64_MAX;
    for (int i = 0; i < RTC_TRANSPORT_MAX_TIMERS; i++) {
        if (t->timers[i].active && t->timers[i].deadline_ms < earliest)
            earliest = t->timers[i].deadline_ms;
    }
    if (earliest == UINT64_MAX)
        return 100; /* default poll interval */

    uint64_t now = rtc_time_ms();
    if (now >= earliest)
        return 0;

    uint64_t diff = earliest - now;
    return (diff > (uint64_t)INT32_MAX) ? INT32_MAX : (int)diff;
}

/* Fire any expired timers. Must be called with mutex held.
 * Collects callbacks first, then fires outside the lock. */
static void transport_fire_timers(rtc_transport_t *t) {
    uint64_t now = rtc_time_ms();

    /* Collect expired timers (up to max) */
    rtc_timer_fn fns[RTC_TRANSPORT_MAX_TIMERS];
    void *users[RTC_TRANSPORT_MAX_TIMERS];
    int count = 0;

    rtc_mutex_lock(&t->mutex);
    for (int i = 0; i < RTC_TRANSPORT_MAX_TIMERS; i++) {
        if (t->timers[i].active && now >= t->timers[i].deadline_ms) {
            fns[count] = t->timers[i].fn;
            users[count] = t->timers[i].user;
            count++;
            t->timers[i].active = false;
        }
    }
    rtc_mutex_unlock(&t->mutex);

    /* Fire callbacks outside the lock */
    for (int i = 0; i < count; i++) {
        fns[i](users[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Transport thread                                                   */
/* ------------------------------------------------------------------ */
static void *transport_thread_fn(void *arg) {
    rtc_transport_t *t = (rtc_transport_t *)arg;
    uint8_t buf[RTC_TRANSPORT_BUF_SIZE];

    while (t->running) {
        /* Compute timeout from nearest timer */
        rtc_mutex_lock(&t->mutex);
        int timeout = transport_next_timeout_ms(t);
        rtc_mutex_unlock(&t->mutex);

        /* Wait for socket readability or timeout */
        int n = rtc_poller_wait(&t->poller, timeout);

        if (n > 0) {
            /* Read packet */
            struct sockaddr_storage from_store;
            socklen_t fromlen = sizeof(from_store);

            ssize_t len = recvfrom(t->sock, (char *)buf, sizeof(buf), 0,
                                   (struct sockaddr *)&from_store, &fromlen);
            if (len > 0) {
                rtc_addr_t from;
                memcpy(&from.addr, &from_store, fromlen);
                from.len = fromlen;

                rtc_pkt_type_t type = transport_classify(buf, (size_t)len);

                /* Dispatch to callback */
                rtc_mutex_lock(&t->recv_mutex);
                rtc_transport_recv_fn fn = t->on_recv;
                void *user = t->recv_user;
                rtc_mutex_unlock(&t->recv_mutex);

                if (fn) {
                    fn(type, buf, (size_t)len, &from, user);
                }
            }
        }

        /* Fire expired timers */
        transport_fire_timers(t);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int rtc_transport_init(rtc_transport_t *t) {
    memset(t, 0, sizeof(*t));
    t->sock = RTC_INVALID_SOCKET;

    /* Create UDP socket */
    t->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->sock == RTC_INVALID_SOCKET) {
        RTC_LOG_ERR("Transport: failed to create socket");
        return RTC_ERR_SOCKET;
    }

    /* Bind to any available port */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;
    if (bind(t->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        RTC_LOG_ERR("Transport: bind failed");
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
        return RTC_ERR_SOCKET;
    }

    /* Store local address */
    t->local_addr.len = sizeof(struct sockaddr_in);
    if (getsockname(t->sock, (struct sockaddr *)&t->local_addr.addr, &t->local_addr.len) != 0) {
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
        return RTC_ERR_SOCKET;
    }

    /* Set non-blocking for use with poller */
    int rc = rtc_set_nonblocking(t->sock);
    if (rc != RTC_OK) {
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
        return rc;
    }

    /* Init poller and register socket */
    rc = rtc_poller_init(&t->poller);
    if (rc != RTC_OK) {
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
        return rc;
    }

    rc = rtc_poller_add(&t->poller, t->sock);
    if (rc != RTC_OK) {
        rtc_poller_close(&t->poller);
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
        return rc;
    }

    /* Init synchronization */
    rtc_mutex_init(&t->mutex);
    rtc_mutex_init(&t->recv_mutex);

    /* Start background thread */
    t->running = true;
    rc = rtc_thread_create(&t->thread, transport_thread_fn, t);
    if (rc != RTC_OK) {
        t->running = false;
        rtc_poller_close(&t->poller);
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
        rtc_mutex_destroy(&t->mutex);
        rtc_mutex_destroy(&t->recv_mutex);
        return rc;
    }

    RTC_LOG_INFO("Transport: initialized (socket bound, thread started)");
    return RTC_OK;
}

int rtc_transport_send(rtc_transport_t *t, const uint8_t *data, size_t len,
                       const rtc_addr_t *dest) {
    if (t->sock == RTC_INVALID_SOCKET)
        return RTC_ERR_SOCKET;

    ssize_t sent = sendto(t->sock, (const char *)data, (int)len, 0,
                          (const struct sockaddr *)&dest->addr, dest->len);
    return (sent > 0) ? RTC_OK : RTC_ERR_SOCKET;
}

int rtc_transport_send_to_remote(rtc_transport_t *t, const uint8_t *data, size_t len) {
    if (!t->remote_addr_set)
        return RTC_ERR_INVALID;
    return rtc_transport_send(t, data, len, &t->remote_addr);
}

void rtc_transport_set_recv_callback(rtc_transport_t *t, rtc_transport_recv_fn fn, void *user) {
    rtc_mutex_lock(&t->recv_mutex);
    t->on_recv = fn;
    t->recv_user = user;
    rtc_mutex_unlock(&t->recv_mutex);
}

void rtc_transport_set_remote(rtc_transport_t *t, const rtc_addr_t *addr) {
    rtc_mutex_lock(&t->mutex);
    t->remote_addr = *addr;
    t->remote_addr_set = true;
    rtc_mutex_unlock(&t->mutex);
}

rtc_timer_id_t rtc_transport_add_timer(rtc_transport_t *t, uint64_t deadline_ms, rtc_timer_fn fn,
                                       void *user) {
    rtc_mutex_lock(&t->mutex);
    for (int i = 0; i < RTC_TRANSPORT_MAX_TIMERS; i++) {
        if (!t->timers[i].active) {
            t->timers[i].deadline_ms = deadline_ms;
            t->timers[i].fn = fn;
            t->timers[i].user = user;
            t->timers[i].active = true;
            rtc_mutex_unlock(&t->mutex);
            return i;
        }
    }
    rtc_mutex_unlock(&t->mutex);
    RTC_LOG_WARN("Transport: no free timer slots");
    return -1;
}

void rtc_transport_cancel_timer(rtc_transport_t *t, rtc_timer_id_t id) {
    if (id < 0 || id >= RTC_TRANSPORT_MAX_TIMERS)
        return;
    rtc_mutex_lock(&t->mutex);
    t->timers[id].active = false;
    rtc_mutex_unlock(&t->mutex);
}

rtc_socket_t rtc_transport_get_socket(const rtc_transport_t *t) {
    return t->sock;
}

int rtc_transport_get_local_addr(rtc_transport_t *t, rtc_addr_t *out) {
    *out = t->local_addr;
    return RTC_OK;
}

void rtc_transport_close(rtc_transport_t *t) {
    if (!t->running && t->sock == RTC_INVALID_SOCKET)
        return;

    /* Signal thread to stop */
    t->running = false;

    /* Join thread */
    rtc_thread_join(&t->thread);

    /* Cleanup */
    rtc_poller_close(&t->poller);

    if (t->sock != RTC_INVALID_SOCKET) {
        rtc_close_socket(t->sock);
        t->sock = RTC_INVALID_SOCKET;
    }

    rtc_mutex_destroy(&t->mutex);
    rtc_mutex_destroy(&t->recv_mutex);

    RTC_LOG_INFO("Transport: closed");
}
