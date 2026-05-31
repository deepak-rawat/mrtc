/*
 * rtc_transport.c - Transport layer with threaded event loop.
 *
 * Owns the UDP socket, runs a background poller thread,
 * classifies incoming packets per RFC 7983, and dispatches
 * to registered callbacks.
 *
 * Networking model:
 *   - The UDP socket is AF_INET6 with IPV6_V6ONLY=0 (dual-stack), so a
 *     single socket accepts both IPv4 and IPv6 traffic. IPv4 senders
 *     appear at the kernel level as IPv4-mapped IPv6 addresses
 *     (::ffff:a.b.c.d); we normalize those back to plain AF_INET before
 *     delivering to callbacks so callers see familiar address shapes.
 *   - On POSIX, recvmsg parses the IPV6_PKTINFO cmsg so we know which
 *     local interface address the packet came in on. send_to_remote
 *     uses sendmsg with a matching cmsg to pin the source IP — fixes
 *     the classic multi-homed reply-from-wrong-interface bug.
 *   - On Windows, source pinning is not yet implemented (WSARecvMsg /
 *     WSASendMsg require GUID-indirected function pointer lookup). The
 *     dual-stack v6 socket still works there; only PKTINFO is gated.
 *     TODO-test: Windows PKTINFO.
 */
#include "rtc_transport.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Address-family normalization (dual-stack helpers)                  */
/* ------------------------------------------------------------------ */

/* Convert an AF_INET6 IPv4-mapped address (::ffff:a.b.c.d) to plain
 * AF_INET in place. No-op for genuine IPv6 or already-AF_INET. */
static void addr_unmap_v4(rtc_addr_t *a) {
    if (a->len < (socklen_t)sizeof(struct sockaddr_in6))
        return;
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&a->addr;
    if (s6->sin6_family != AF_INET6)
        return;
    /* IN6_IS_ADDR_V4MAPPED test: first 80 bits 0, next 16 bits 0xFFFF. */
    const uint8_t *b = s6->sin6_addr.s6_addr;
    bool mapped = true;
    for (int i = 0; i < 10; i++)
        if (b[i] != 0) {
            mapped = false;
            break;
        }
    if (mapped && (b[10] != 0xFF || b[11] != 0xFF))
        mapped = false;
    if (!mapped)
        return;

    uint16_t port = s6->sin6_port;
    uint32_t v4;
    memcpy(&v4, &b[12], 4);

    struct sockaddr_in *s4 = (struct sockaddr_in *)&a->addr;
    memset(s4, 0, sizeof(*s4));
    s4->sin_family = AF_INET;
    s4->sin_port = port;
    s4->sin_addr.s_addr = v4;
    a->len = sizeof(struct sockaddr_in);
}

/* Convert a plain AF_INET address to IPv4-mapped AF_INET6 in place. The
 * transport's socket is AF_INET6 dual-stack, so outbound to a v4 dest
 * goes through the v4-mapped form (kernels reject mixed-family sendto). */
static void addr_map_v4(rtc_addr_t *a) {
    if (a->len < (socklen_t)sizeof(struct sockaddr_in))
        return;
    struct sockaddr_in *s4 = (struct sockaddr_in *)&a->addr;
    if (s4->sin_family != AF_INET)
        return;

    uint16_t port = s4->sin_port;
    uint32_t v4 = s4->sin_addr.s_addr;

    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&a->addr;
    memset(s6, 0, sizeof(*s6));
    s6->sin6_family = AF_INET6;
    s6->sin6_port = port;
    s6->sin6_addr.s6_addr[10] = 0xFF;
    s6->sin6_addr.s6_addr[11] = 0xFF;
    memcpy(&s6->sin6_addr.s6_addr[12], &v4, 4);
    a->len = sizeof(struct sockaddr_in6);
}

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

/* recvmsg + PKTINFO is only available on POSIX in this revision. Windows
 * uses WSARecvMsg which requires GUID-indirected function pointer lookup
 * — left as a TODO. On Windows we fall back to recvfrom and skip the
 * source-pinning step (last_local_valid stays false). */
#ifndef _WIN32
#  define RTC_TRANSPORT_HAS_PKTINFO 1
#else
#  define RTC_TRANSPORT_HAS_PKTINFO 0
#endif

#if RTC_TRANSPORT_HAS_PKTINFO
/* Drain one packet via recvmsg; on success, write the local-iface address
 * into *local_out (true means cmsg present, IPv4-mapped form for v4).
 * Also surfaces SO_RXQ_OVFL on Linux as *kdrops_out (kernel drop count
 * since the last cmsg; 0 elsewhere). */
static ssize_t recv_one_with_pktinfo(rtc_socket_t s, uint8_t *buf, size_t buflen,
                                     struct sockaddr_storage *from, socklen_t *fromlen,
                                     struct in6_addr *local_out, bool *local_valid_out,
                                     uint32_t *kdrops_out) {
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = buflen;

    /* Enough for one IPV6_PKTINFO and one SO_RXQ_OVFL cmsg with slack. */
    uint8_t cbuf[CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(uint32_t))];

    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = from;
    mh.msg_namelen = sizeof(*from);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);

    ssize_t n = recvmsg(s, &mh, 0);
    if (n <= 0)
        return n;

    *fromlen = mh.msg_namelen;
    *local_valid_out = false;
    *kdrops_out = 0;

    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
            const struct in6_pktinfo *pi = (const struct in6_pktinfo *)CMSG_DATA(c);
            *local_out = pi->ipi6_addr;
            *local_valid_out = true;
        }
#  ifdef SO_RXQ_OVFL
        else if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
            uint32_t v;
            memcpy(&v, CMSG_DATA(c), sizeof(v));
            *kdrops_out = v;
        }
#  endif
    }
    return n;
}

/* Drain ICMP errors from MSG_ERRQUEUE so they don't pile up and cause
 * the next sendto to return ECONNREFUSED. Called opportunistically. */
#  ifdef __linux__
static void drain_err_queue(rtc_socket_t s) {
    for (;;) {
        uint8_t buf[256];
        uint8_t cbuf[256];
        struct sockaddr_storage from;
        struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
        struct msghdr mh;
        memset(&mh, 0, sizeof(mh));
        mh.msg_name = &from;
        mh.msg_namelen = sizeof(from);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        ssize_t n = recvmsg(s, &mh, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0)
            break;
        /* Errors logged at DBG only — surfacing here as INFO would be
         * very noisy under packet loss / NAT churn. */
        RTC_LOG_DBG("Transport: drained ICMP error (%zd bytes)", n);
    }
}
#  else
static void drain_err_queue(rtc_socket_t s) {
    (void)s;
}
#  endif
#endif /* RTC_TRANSPORT_HAS_PKTINFO */

static void *transport_thread_fn(void *arg) {
    rtc_transport_t *t = (rtc_transport_t *)arg;
    uint8_t buf[RTC_TRANSPORT_BUF_SIZE];
    rtc_poller_event_t evs[RTC_POLLER_MAX_EVENTS];

    while (t->running) {
        rtc_mutex_lock(&t->timer_mutex);
        int timeout = timer_next_timeout_ms(t->timers, RTC_TRANSPORT_MAX_TIMERS);
        rtc_mutex_unlock(&t->timer_mutex);

        int n = rtc_poller_wait(&t->poller, evs, RTC_POLLER_MAX_EVENTS, timeout);

        rtc_socket_t s = atomic_load_explicit(&t->sock, memory_order_acquire);
        for (int e = 0; e < n; e++) {
            if (evs[e].fd != s || !(evs[e].events & RTC_POLLER_EV_READ))
                continue;
            int drained = 0;
            for (int i = 0; i < RTC_TRANSPORT_RECV_BATCH; i++) {
                struct sockaddr_storage from_store;
                socklen_t fromlen = sizeof(from_store);
                ssize_t len;

#if RTC_TRANSPORT_HAS_PKTINFO
                struct in6_addr local6;
                bool local_valid = false;
                uint32_t kdrops = 0;
                len = recv_one_with_pktinfo(s, buf, sizeof(buf), &from_store, &fromlen, &local6,
                                            &local_valid, &kdrops);
                if (len > 0 && local_valid) {
                    /* Atomic 128-bit store is non-portable; the addr is
                     * only used by senders as a best-effort source pin.
                     * Worst case under race: one send goes out the wrong
                     * iface (same as today). Acceptable. */
                    memcpy(&t->last_local_v6, &local6, sizeof(local6));
                    atomic_store_explicit(&t->last_local_valid, true, memory_order_release);
                }
                if (kdrops) {
                    /* SO_RXQ_OVFL reports the absolute drop counter
                     * since the socket opened; storing it directly is
                     * fine \u2014 monotonic, no overflow concerns at u64. */
                    atomic_store_explicit(&t->recv_kernel_drops, (uint64_t)kdrops,
                                          memory_order_relaxed);
                }
#else
                len = recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from_store,
                               &fromlen);
#endif
                if (len <= 0)
                    break; /* EWOULDBLOCK or error — socket drained */
                drained++;

                atomic_fetch_add_explicit(&t->pkts_recv, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->bytes_recv, (uint64_t)len, memory_order_relaxed);

                /* Truncation heuristic: a perfectly-full buffer is
                 * suspicious since RTC_TRANSPORT_BUF_SIZE is sized for
                 * jumbo frames. Real WebRTC traffic never reaches this. */
                if ((size_t)len == sizeof(buf))
                    RTC_LOG_WARN("Transport: recvfrom filled buffer (%zd bytes) — "
                                 "possible packet truncation",
                                 len);

                rtc_addr_t from;
                memcpy(&from.addr, &from_store, fromlen);
                from.len = fromlen;
                /* Normalize v4-mapped to plain AF_INET so callers see the
                 * address family they expect (ICE / SDP / STUN all key on
                 * AF_INET for IPv4 peers). */
                addr_unmap_v4(&from);

                rtc_pkt_type_t type = transport_classify(buf, (size_t)len);

                if (t->on_recv) {
                    t->on_recv(type, buf, (size_t)len, &from, t->recv_user);
                }
            }
            if (drained == RTC_TRANSPORT_RECV_BATCH)
                atomic_fetch_add_explicit(&t->recv_drain_full, 1, memory_order_relaxed);
        }

        timer_fire_expired(t->timers, RTC_TRANSPORT_MAX_TIMERS, &t->timer_mutex);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int rtc_transport_init(rtc_transport_t *t, rtc_transport_recv_fn on_recv, void *user) {
    memset(t, 0, sizeof(*t));
    atomic_store_explicit(&t->sock, RTC_INVALID_SOCKET, memory_order_relaxed);

    /* Dual-stack UDP socket: one fd accepts both IPv4 and IPv6 traffic.
     * IPv4 senders arrive as IPv4-mapped IPv6 addresses; we unmap them
     * before delivery to callbacks. */
    rtc_socket_t s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == RTC_INVALID_SOCKET) {
        RTC_LOG_ERR("Transport: failed to create socket");
        return RTC_ERR_SOCKET;
    }

    int v6only = 0;
    if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only)) != 0) {
        RTC_LOG_WARN("Transport: IPV6_V6ONLY=0 failed; IPv4 traffic may not be accepted");
    }

    /* Bind to any available port on both families. */
    struct sockaddr_in6 bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_addr = in6addr_any;
    bind_addr.sin6_port = 0;
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        RTC_LOG_ERR("Transport: bind failed");
        rtc_close_socket(s);
        return RTC_ERR_SOCKET;
    }

    /* Store local address (full storage size for v6). */
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

#if RTC_TRANSPORT_HAS_PKTINFO
    /* Receive IPV6_PKTINFO cmsg so we know which local interface each
     * packet arrived on. Required for correct multi-homed reply behavior. */
    int on = 1;
    if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) != 0) {
        RTC_LOG_WARN("Transport: IPV6_RECVPKTINFO failed; reply source-pinning disabled");
    }
#endif

    /* Request larger kernel socket buffers to absorb video bursts and
     * avoid drops between drain iterations. The kernel may clamp; we log
     * the actual value so misconfiguration is visible. */
    int wanted = RTC_TRANSPORT_SOCKBUF_BYTES;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&wanted, sizeof(wanted));
    (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&wanted, sizeof(wanted));

    /* Production socket options. All are best-effort: most are quietly
     * ignored or unsupported on some platforms, which is fine — they're
     * optimizations, not correctness. */

    /* SO_REUSEADDR: lets us re-bind quickly after a restart. Common
     * WebRTC practice; harmless for ephemeral-port binds. */
    {
        int on = 1;
        (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    }

    /* DSCP EF (0xB8 / decimal 184) for expedited forwarding on
     * cooperating networks. IPV6_TCLASS is the v6 counterpart. On
     * Windows DSCP marking requires the qWAVE API which we don't link;
     * setsockopt here is silently ignored. TODO-test on Windows. */
#ifndef _WIN32
    {
        int tos = 0xB8;
        (void)setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        (void)setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
    }
#endif

    /* Disable path-MTU fragmentation. On Linux IP_MTU_DISCOVER = DO sets
     * the DF bit; on BSD/Windows IP_DONTFRAG. Forces oversized DTLS to
     * surface as EMSGSIZE / ICMP "fragmentation needed" instead of being
     * silently fragmented and probably dropped by middleboxes. */
#ifdef __linux__
    {
        int do_pmtu = IP_PMTUDISC_DO;
        (void)setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &do_pmtu, sizeof(do_pmtu));
        int do_pmtu6 = IPV6_PMTUDISC_DO;
        (void)setsockopt(s, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &do_pmtu6, sizeof(do_pmtu6));
    }
#elif defined(_WIN32)
    {
        DWORD df = 1;
        (void)setsockopt(s, IPPROTO_IP, IP_DONTFRAGMENT, (const char *)&df, sizeof(df));
    }
#elif defined(IP_DONTFRAG) /* BSD / macOS */
    {
        int on = 1;
        (void)setsockopt(s, IPPROTO_IP, IP_DONTFRAG, &on, sizeof(on));
    }
#endif

    /* Linux: IP_RECVERR / IPV6_RECVERR queues ICMP errors (e.g. remote
     * "port unreachable") on the socket's MSG_ERRQUEUE. Without this,
     * the kernel returns ECONNREFUSED on the NEXT sendto and keeps
     * doing so until the queue is drained — wedging the send path when
     * a peer dies. We enable the option and drain the error queue
     * opportunistically below. */
#ifdef __linux__
    {
        int on = 1;
        (void)setsockopt(s, IPPROTO_IP, IP_RECVERR, &on, sizeof(on));
        (void)setsockopt(s, IPPROTO_IPV6, IPV6_RECVERR, &on, sizeof(on));
        /* Also enable kernel-drop notifications so we can attribute
         * receive losses to overflow vs. drop-on-the-wire. */
        (void)setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
    }
#endif
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
    rtc_mutex_init(&t->timer_mutex);

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
        rtc_mutex_destroy(&t->timer_mutex);
        return rc;
    }

    char ipbuf[64];
    uint16_t port = 0;
    if (rtc_addr_to_string(&t->local_addr, ipbuf, sizeof(ipbuf), &port) == RTC_OK)
        RTC_LOG_INFO("Transport: bound %s:%u (rcvbuf=%d sndbuf=%d)", ipbuf, port, got_rcv, got_snd);
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
    if (s == RTC_INVALID_SOCKET) {
        atomic_fetch_add_explicit(&t->send_errors, 1, memory_order_relaxed);
        return RTC_ERR_SOCKET;
    }

    /* Socket is AF_INET6 dual-stack. A v4 destination must be sent as
     * v4-mapped v6 (::ffff:a.b.c.d) — kernels reject AF_INET sendto on
     * a v6 socket. Local copy so we don't mutate the caller's struct. */
    rtc_addr_t d = *dest;
    addr_map_v4(&d);

    ssize_t sent =
        sendto(s, (const char *)data, (int)len, 0, (const struct sockaddr *)&d.addr, d.len);
    if (sent > 0) {
        atomic_fetch_add_explicit(&t->pkts_sent, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->bytes_sent, (uint64_t)sent, memory_order_relaxed);
        return RTC_OK;
    }

    /* Distinguish "kernel SNDBUF momentarily full" from real failures.
     * EAGAIN/EWOULDBLOCK is transient and recoverable; callers that
     * implement pacing should retry rather than treat as a hard error.
     * Counted separately so it doesn't false-alarm in dashboards. */
#ifdef _WIN32
    int err = WSAGetLastError();
    bool would_block = (err == WSAEWOULDBLOCK);
#else
    bool would_block = (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
    if (would_block) {
        atomic_fetch_add_explicit(&t->send_would_block, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&t->send_errors, 1, memory_order_relaxed);
    }
#if RTC_TRANSPORT_HAS_PKTINFO
    /* A real error often correlates with a queued ICMP unreachable that
     * would otherwise wedge subsequent sends. Drain to unblock. */
    if (!would_block)
        drain_err_queue(s);
#endif
    return RTC_ERR_SOCKET;
}

int rtc_transport_send_to_remote(rtc_transport_t *t, const uint8_t *data, size_t len) {
    /* Acquire-load pairs with the release-store in rtc_transport_set_remote;
     * seeing true guarantees a fully-written remote_addr. */
    if (!atomic_load_explicit(&t->remote_addr_set, memory_order_acquire))
        return RTC_ERR_INVALID;

#if RTC_TRANSPORT_HAS_PKTINFO
    /* Source-pin to the local IP we last received traffic on (set by the
     * recv loop from IPV6_PKTINFO cmsg). Falls back to the unpinned path
     * if no recv has happened yet, in which case the kernel chooses the
     * source IP via its routing table. */
    if (atomic_load_explicit(&t->last_local_valid, memory_order_acquire)) {
        rtc_socket_t s = atomic_load_explicit(&t->sock, memory_order_acquire);
        if (s == RTC_INVALID_SOCKET) {
            atomic_fetch_add_explicit(&t->send_errors, 1, memory_order_relaxed);
            return RTC_ERR_SOCKET;
        }

        rtc_addr_t d = t->remote_addr;
        addr_map_v4(&d);

        struct iovec iov;
        iov.iov_base = (void *)data;
        iov.iov_len = len;

        uint8_t cbuf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
        memset(cbuf, 0, sizeof(cbuf));

        struct msghdr mh;
        memset(&mh, 0, sizeof(mh));
        mh.msg_name = (void *)&d.addr;
        mh.msg_namelen = d.len;
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);

        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = IPPROTO_IPV6;
        c->cmsg_type = IPV6_PKTINFO;
        c->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
        struct in6_pktinfo *pi = (struct in6_pktinfo *)CMSG_DATA(c);
        memcpy(&pi->ipi6_addr, &t->last_local_v6, sizeof(pi->ipi6_addr));
        pi->ipi6_ifindex = 0; /* let kernel pick the egress iface */

        ssize_t sent = sendmsg(s, &mh, 0);
        if (sent > 0) {
            atomic_fetch_add_explicit(&t->pkts_sent, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&t->bytes_sent, (uint64_t)sent, memory_order_relaxed);
            return RTC_OK;
        }
        bool would_block = (errno == EAGAIN || errno == EWOULDBLOCK);
        if (would_block) {
            atomic_fetch_add_explicit(&t->send_would_block, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&t->send_errors, 1, memory_order_relaxed);
            drain_err_queue(s);
        }
        return RTC_ERR_SOCKET;
    }
#endif
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
    rtc_timer_id_t id = timer_add(t->timers, RTC_TRANSPORT_MAX_TIMERS, &t->timer_mutex,
                                  &t->timer_slot_hwm, deadline_ms, fn, user);
    if (id < 0) {
        RTC_LOG_WARN("Transport: no free timer slots (max=%d) — caller will"
                     " silently lose this scheduling; raise RTC_TRANSPORT_MAX_TIMERS",
                     RTC_TRANSPORT_MAX_TIMERS);
        return id;
    }
    /* Break the poll loop so the new (possibly sooner) deadline is
     * observed promptly. Without this, the thread can sleep up to the
     * previous timeout (default 100ms) before noticing. */
    rtc_poller_wake(&t->poller);
    return id;
}

void rtc_transport_cancel_timer(rtc_transport_t *t, rtc_timer_id_t id) {
    timer_cancel(t->timers, RTC_TRANSPORT_MAX_TIMERS, &t->timer_mutex, id);
}

rtc_socket_t rtc_transport_get_socket(const rtc_transport_t *t) {
    return atomic_load_explicit(&t->sock, memory_order_acquire);
}

int rtc_transport_get_local_addr(rtc_transport_t *t, rtc_addr_t *out) {
    *out = t->local_addr;
    return RTC_OK;
}

void rtc_transport_get_stats(const rtc_transport_t *t, rtc_transport_stats_t *out) {
    out->pkts_recv = atomic_load_explicit(&t->pkts_recv, memory_order_relaxed);
    out->bytes_recv = atomic_load_explicit(&t->bytes_recv, memory_order_relaxed);
    out->pkts_sent = atomic_load_explicit(&t->pkts_sent, memory_order_relaxed);
    out->bytes_sent = atomic_load_explicit(&t->bytes_sent, memory_order_relaxed);
    out->send_errors = atomic_load_explicit(&t->send_errors, memory_order_relaxed);
    out->send_would_block = atomic_load_explicit(&t->send_would_block, memory_order_relaxed);
    out->recv_drain_full = atomic_load_explicit(&t->recv_drain_full, memory_order_relaxed);
    out->recv_kernel_drops = atomic_load_explicit(&t->recv_kernel_drops, memory_order_relaxed);
    out->timer_slot_hwm = t->timer_slot_hwm;
}

void rtc_transport_close(rtc_transport_t *t) {
    /* Idempotent: only the thread that flips running true -> false runs
     * the shutdown sequence. Concurrent / repeated close calls no-op. */
    bool was_running = atomic_exchange_explicit(&t->running, false, memory_order_acq_rel);
    if (!was_running)
        return;

    /* Wake the poller so the thread observes running==false now, not
     * after the next timeout. */
    rtc_poller_wake(&t->poller);

    /* Join thread (it observes running==false and exits its loop). */
    rtc_thread_join(&t->thread);

    /* Cleanup */
    rtc_poller_close(&t->poller);

    rtc_socket_t s = atomic_exchange_explicit(&t->sock, RTC_INVALID_SOCKET, memory_order_acq_rel);
    if (s != RTC_INVALID_SOCKET)
        rtc_close_socket(s);

    rtc_mutex_destroy(&t->timer_mutex);

    RTC_LOG_INFO("Transport: closed (timer slot hwm=%d/%d)", t->timer_slot_hwm,
                 RTC_TRANSPORT_MAX_TIMERS);
}
