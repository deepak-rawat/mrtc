/*
 * Shared RTC listener foundation.
 */
#include "rtc/rtc_listener.h"

#include "rtc/rtc_str_map.h"
#include "rtc_listener_internal.h"
#include "rtc_packet_io.h"
#include "rtc_stun.h"
#include "rtc_worker_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <iphlpapi.h>
#else
#  include <ifaddrs.h>
#  include <net/if.h>
#endif

typedef struct {
    rtc_listener_packet_fn fn;
    void *user;
} listener_route_t;

struct rtc_listener {
    rtc_worker_t *worker;
    rtc_packet_io_t io;
    rtc_transport_candidate_t candidates[ICE_MAX_CANDIDATES];
    int candidate_count;
    rtc_addr_t local_addr;
    rtc_str_map_t ufrag_routes;
    rtc_str_map_t txn_routes;
    rtc_str_map_t tuple_routes;
    rtc_mutex_t route_mutex;
    rtc_mutex_t candidate_mutex;
    bool route_mutex_ready;
    bool candidate_mutex_ready;
    rtc_listener_on_candidate_fn on_candidate;
    void *on_candidate_user;
    rtc_listener_on_gathering_done_fn on_gathering_done;
    void *on_gathering_done_user;
    bool gathering_complete;
    char stun_server[256];
    uint16_t stun_port;
    rtc_addr_t stun_addr;
    uint8_t gather_txn_id[STUN_TXN_ID_SIZE];
    bool gather_txn_registered;
    rtc_worker_timer_t gather_timer;
    rtc_addr_t gather_related_addr;
    _Atomic bool closed;
    _Atomic uint64_t packets_unhandled;
};

static const char hex_digits[] = "0123456789abcdef";

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0)
        return;
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool is_wildcard_ip(const char *ip) {
    return !ip || ip[0] == '\0' || strcmp(ip, "0.0.0.0") == 0 || strcmp(ip, "::") == 0;
}

static int listener_add_candidate(rtc_listener_t *listener, const rtc_transport_candidate_t *cand) {
    if (!listener || !cand)
        return RTC_ERR_INVALID;

    rtc_listener_on_candidate_fn fn = NULL;
    void *user = NULL;
    rtc_mutex_lock(&listener->candidate_mutex);
    if (listener->candidate_count >= ICE_MAX_CANDIDATES) {
        rtc_mutex_unlock(&listener->candidate_mutex);
        return RTC_ERR_NOMEM;
    }
    listener->candidates[listener->candidate_count++] = *cand;
    fn = listener->on_candidate;
    user = listener->on_candidate_user;
    rtc_mutex_unlock(&listener->candidate_mutex);

    if (fn)
        fn(cand, user);
    return RTC_OK;
}

static void listener_mark_gathering_done(rtc_listener_t *listener) {
    rtc_listener_on_gathering_done_fn fn = NULL;
    void *user = NULL;
    rtc_mutex_lock(&listener->candidate_mutex);
    if (listener->gathering_complete) {
        rtc_mutex_unlock(&listener->candidate_mutex);
        return;
    }
    listener->gathering_complete = true;
    fn = listener->on_gathering_done;
    user = listener->on_gathering_done_user;
    rtc_mutex_unlock(&listener->candidate_mutex);

    if (fn)
        fn(user);
}

static int listener_candidate_from_address(rtc_transport_candidate_t *cand, const char *foundation,
                                           const char *address, uint16_t port,
                                           rtc_transport_candidate_type_t type) {
    if (!cand || !foundation || !address || port == 0)
        return RTC_ERR_INVALID;
    memset(cand, 0, sizeof(*cand));
    copy_string(cand->foundation, sizeof(cand->foundation), foundation);
    copy_string(cand->address, sizeof(cand->address), address);
    copy_string(cand->protocol, sizeof(cand->protocol), "udp");
    cand->port = port;
    cand->type = type;
    return RTC_OK;
}

static int listener_add_host_address(rtc_listener_t *listener, const char *address, uint16_t port) {
    char foundation[32];
    rtc_mutex_lock(&listener->candidate_mutex);
    int index = listener->candidate_count;
    rtc_mutex_unlock(&listener->candidate_mutex);
    snprintf(foundation, sizeof(foundation), "H%d", index);

    rtc_transport_candidate_t cand;
    int rc = listener_candidate_from_address(&cand, foundation, address, port,
                                             RTC_TRANSPORT_CANDIDATE_HOST);
    if (rc != RTC_OK)
        return rc;
    return listener_add_candidate(listener, &cand);
}

static bool listener_has_srflx_duplicate_locked(rtc_listener_t *listener, const char *address,
                                                uint16_t port) {
    for (int i = 0; i < listener->candidate_count; i++) {
        const rtc_transport_candidate_t *cand = &listener->candidates[i];
        if (cand->type == RTC_TRANSPORT_CANDIDATE_HOST && cand->port == port &&
            strcmp(cand->address, address) == 0) {
            return true;
        }
    }
    return false;
}

static bool listener_first_host_candidate(rtc_listener_t *listener,
                                          rtc_transport_candidate_t *out) {
    bool found = false;
    rtc_mutex_lock(&listener->candidate_mutex);
    for (int i = 0; i < listener->candidate_count; i++) {
        if (listener->candidates[i].type == RTC_TRANSPORT_CANDIDATE_HOST) {
            if (out)
                *out = listener->candidates[i];
            found = true;
            break;
        }
    }
    rtc_mutex_unlock(&listener->candidate_mutex);
    return found;
}

static int listener_add_srflx_candidate(rtc_listener_t *listener, const rtc_addr_t *mapped,
                                        const rtc_addr_t *related) {
    char ip[64];
    uint16_t port = 0;
    int rc = rtc_addr_to_string(mapped, ip, sizeof(ip), &port);
    if (rc != RTC_OK)
        return rc;

    char related_ip[64];
    uint16_t related_port = 0;
    rc = rtc_addr_to_string(related, related_ip, sizeof(related_ip), &related_port);
    if (rc != RTC_OK)
        return rc;

    rtc_listener_on_candidate_fn fn = NULL;
    void *user = NULL;
    rtc_transport_candidate_t cand;
    rtc_mutex_lock(&listener->candidate_mutex);
    if (listener_has_srflx_duplicate_locked(listener, ip, port)) {
        rtc_mutex_unlock(&listener->candidate_mutex);
        return RTC_OK;
    }
    if (listener->candidate_count >= ICE_MAX_CANDIDATES) {
        rtc_mutex_unlock(&listener->candidate_mutex);
        return RTC_ERR_NOMEM;
    }
    char foundation[32];
    int srflx_index = 0;
    for (int i = 0; i < listener->candidate_count; i++) {
        if (listener->candidates[i].type == RTC_TRANSPORT_CANDIDATE_SRFLX)
            srflx_index++;
    }
    snprintf(foundation, sizeof(foundation), "S%d", srflx_index);
    listener_candidate_from_address(&cand, foundation, ip, port, RTC_TRANSPORT_CANDIDATE_SRFLX);
    copy_string(cand.related_address, sizeof(cand.related_address), related_ip);
    cand.related_port = related_port;
    cand.has_related_address = true;
    listener->candidates[listener->candidate_count++] = cand;
    fn = listener->on_candidate;
    user = listener->on_candidate_user;
    rtc_mutex_unlock(&listener->candidate_mutex);

    if (fn)
        fn(&cand, user);
    return RTC_OK;
}

static int listener_resolve_addr(const char *server, uint16_t port, rtc_addr_t *out) {
    if (!server || !out)
        return RTC_ERR_INVALID;
    if (rtc_addr_from_string(out, server, port) == RTC_OK)
        return RTC_OK;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *result = NULL;
    int gai = getaddrinfo(server, port_str, &hints, &result);
    if (gai != 0 || !result)
        return RTC_ERR_INVALID;

    memset(out, 0, sizeof(*out));
    memcpy(&out->addr, result->ai_addr, result->ai_addrlen);
    out->len = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return RTC_OK;
}

static void listener_cancel_gather(rtc_listener_t *listener) {
    if (listener->gather_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(listener->worker, listener->gather_timer);
        listener->gather_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (listener->gather_txn_registered) {
        rtc_listener_unregister_stun_txn(listener, listener->gather_txn_id);
        listener->gather_txn_registered = false;
    }
}

static void listener_stun_timeout(void *user) {
    rtc_listener_t *listener = (rtc_listener_t *)user;
    listener->gather_timer = RTC_WORKER_TIMER_INVALID;
    listener_cancel_gather(listener);
    listener_mark_gathering_done(listener);
}

static void listener_on_stun_gather_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                           const rtc_addr_t *from, void *user) {
    (void)type;
    (void)from;
    rtc_listener_t *listener = (rtc_listener_t *)user;
    rtc_stun_msg_t msg;
    if (rtc_stun_parse(&msg, data, len) != RTC_OK)
        return;
    if (msg.type != STUN_BINDING_RESPONSE)
        return;

    listener_cancel_gather(listener);

    rtc_addr_t mapped;
    if (rtc_stun_get_mapped_address(&msg, &mapped) == RTC_OK)
        (void)listener_add_srflx_candidate(listener, &mapped, &listener->gather_related_addr);
    listener_mark_gathering_done(listener);
}

static void listener_start_stun_gather(void *user) {
    rtc_listener_t *listener = (rtc_listener_t *)user;
    listener->gather_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load_explicit(&listener->closed, memory_order_acquire))
        return;
    if (listener->stun_server[0] == '\0') {
        listener_mark_gathering_done(listener);
        return;
    }

    rtc_transport_candidate_t host;
    if (!listener_first_host_candidate(listener, &host)) {
        listener_mark_gathering_done(listener);
        return;
    }
    if (rtc_addr_from_string(&listener->gather_related_addr, host.address, host.port) != RTC_OK) {
        listener_mark_gathering_done(listener);
        return;
    }
    if (listener_resolve_addr(listener->stun_server, listener->stun_port, &listener->stun_addr) !=
        RTC_OK) {
        RTC_LOG_WARN("Listener: failed to resolve STUN server %s", listener->stun_server);
        listener_mark_gathering_done(listener);
        return;
    }

    rtc_stun_msg_t req;
    if (rtc_stun_build_binding_request(&req, NULL, NULL, 0, false, 0, false) != RTC_OK) {
        listener_mark_gathering_done(listener);
        return;
    }

    memcpy(listener->gather_txn_id, req.txn_id, STUN_TXN_ID_SIZE);
    int rc = rtc_listener_register_stun_txn(listener, listener->gather_txn_id,
                                            listener_on_stun_gather_packet, listener);
    if (rc != RTC_OK) {
        listener_mark_gathering_done(listener);
        return;
    }
    listener->gather_txn_registered = true;

    rc = rtc_listener_send_to(listener, req.buf, req.buf_len, &listener->stun_addr);
    if (rc != RTC_OK) {
        listener_cancel_gather(listener);
        listener_mark_gathering_done(listener);
        return;
    }
    listener->gather_timer = rtc_worker_add_timer(listener->worker, rtc_time_ms() + 3000,
                                                  listener_stun_timeout, listener);
}

static int listener_gather_host_candidates(rtc_listener_t *listener,
                                           const rtc_listener_config_t *cfg) {
    char ip[64];
    uint16_t port = 0;
    int rc = rtc_addr_to_string(&listener->local_addr, ip, sizeof(ip), &port);
    if (rc != RTC_OK)
        return rc;

    if (cfg && cfg->announced_ip && cfg->announced_ip[0] != '\0')
        return listener_add_host_address(listener, cfg->announced_ip, port);
    if (cfg && !is_wildcard_ip(cfg->listen_ip))
        return listener_add_host_address(listener, cfg->listen_ip, port);

#ifdef _WIN32
    ULONG bufsize = 15000;
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufsize);
    if (!addrs)
        return RTC_ERR_NOMEM;

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &bufsize);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufsize);
        if (!addrs)
            return RTC_ERR_NOMEM;
        ret = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &bufsize);
    }
    if (ret != NO_ERROR) {
        free(addrs);
        return RTC_ERR_GENERIC;
    }

    for (PIP_ADAPTER_ADDRESSES a = addrs; a && listener->candidate_count < ICE_MAX_CANDIDATES;
         a = a->Next) {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ua->Address.lpSockaddr;
            if (!sin || sin->sin_family != AF_INET)
                continue;
            if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
                continue;
            char host_ip[64];
            if (inet_ntop(AF_INET, &sin->sin_addr, host_ip, (socklen_t)sizeof(host_ip)))
                (void)listener_add_host_address(listener, host_ip, port);
        }
    }
    free(addrs);
#else
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0)
        return RTC_ERR_SOCKET;

    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        char host_ip[64];
        if (inet_ntop(AF_INET, &sin->sin_addr, host_ip, sizeof(host_ip)))
            (void)listener_add_host_address(listener, host_ip, port);
    }
    freeifaddrs(ifap);
#endif

    if (listener->candidate_count == 0) {
        if (is_wildcard_ip(ip))
            copy_string(ip, sizeof(ip), "127.0.0.1");
        return listener_add_host_address(listener, ip, port);
    }
    return RTC_OK;
}

static listener_route_t *route_create(rtc_listener_packet_fn fn, void *user) {
    if (!fn)
        return NULL;
    listener_route_t *route = (listener_route_t *)calloc(1, sizeof(*route));
    if (!route)
        return NULL;
    route->fn = fn;
    route->user = user;
    return route;
}

static void free_routes(rtc_str_map_t *map) {
    rtc_str_map_iter_t it;
    memset(&it, 0, sizeof(it));
    const char *key;
    void *value;
    while (rtc_str_map_next(map, &it, &key, &value)) {
        (void)key;
        free(value);
    }
    rtc_str_map_free(map);
}

static void txn_key(const uint8_t txn_id[STUN_TXN_ID_SIZE], char out[STUN_TXN_ID_SIZE * 2 + 1]) {
    for (int i = 0; i < STUN_TXN_ID_SIZE; i++) {
        out[i * 2] = hex_digits[txn_id[i] >> 4];
        out[i * 2 + 1] = hex_digits[txn_id[i] & 0x0F];
    }
    out[STUN_TXN_ID_SIZE * 2] = '\0';
}

static int tuple_key(const rtc_addr_t *addr, char *out, size_t out_len) {
    if (!addr || !out || out_len == 0)
        return RTC_ERR_INVALID;
    char ip[64];
    uint16_t port = 0;
    int rc = rtc_addr_to_string(addr, ip, sizeof(ip), &port);
    if (rc != RTC_OK)
        return rc;
    const struct sockaddr *sa = (const struct sockaddr *)&addr->addr;
    int n = snprintf(out, out_len, "%d|%s|%u", (int)sa->sa_family, ip, (unsigned)port);
    if (n < 0 || (size_t)n >= out_len)
        return RTC_ERR_INVALID;
    return RTC_OK;
}

static int username_local_ufrag(const rtc_stun_msg_t *msg, char *out, size_t out_len) {
    uint16_t attr_len = 0;
    const uint8_t *attr = rtc_stun_find_attr(msg, STUN_ATTR_USERNAME, &attr_len);
    if (!attr || attr_len == 0 || out_len == 0)
        return RTC_ERR_INVALID;

    size_t len = 0;
    while (len < attr_len && attr[len] != ':')
        len++;
    if (len == 0)
        return RTC_ERR_INVALID;
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, attr, len);
    out[len] = '\0';
    return RTC_OK;
}

static bool listener_lookup_route(rtc_listener_t *listener, rtc_str_map_t *map, const char *key,
                                  rtc_listener_packet_fn *fn, void **route_user) {
    bool found = false;
    rtc_mutex_lock(&listener->route_mutex);
    listener_route_t *route = (listener_route_t *)rtc_str_map_get(map, key);
    if (route) {
        *fn = route->fn;
        *route_user = route->user;
        found = true;
    }
    rtc_mutex_unlock(&listener->route_mutex);
    return found;
}

static bool listener_route_stun(rtc_listener_t *listener, rtc_pkt_type_t type, const uint8_t *data,
                                size_t len, const rtc_addr_t *from) {
    rtc_stun_msg_t msg;
    if (rtc_stun_parse(&msg, data, len) != RTC_OK)
        return false;

    rtc_listener_packet_fn fn = NULL;
    void *route_user = NULL;

    if (msg.type == STUN_BINDING_REQUEST) {
        char ufrag[128];
        if (username_local_ufrag(&msg, ufrag, sizeof(ufrag)) == RTC_OK &&
            listener_lookup_route(listener, &listener->ufrag_routes, ufrag, &fn, &route_user)) {
            fn(type, data, len, from, route_user);
            return true;
        }
    }

    char key[STUN_TXN_ID_SIZE * 2 + 1];
    txn_key(msg.txn_id, key);
    if (listener_lookup_route(listener, &listener->txn_routes, key, &fn, &route_user)) {
        fn(type, data, len, from, route_user);
        return true;
    }

    return false;
}

static bool listener_route_tuple(rtc_listener_t *listener, rtc_pkt_type_t type, const uint8_t *data,
                                 size_t len, const rtc_addr_t *from) {
    char key[128];
    if (tuple_key(from, key, sizeof(key)) != RTC_OK)
        return false;
    rtc_listener_packet_fn fn = NULL;
    void *route_user = NULL;
    if (!listener_lookup_route(listener, &listener->tuple_routes, key, &fn, &route_user))
        return false;
    fn(type, data, len, from, route_user);
    return true;
}

static void listener_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                               const rtc_addr_t *from, void *user) {
    rtc_listener_t *listener = (rtc_listener_t *)user;
    bool routed = false;
    if (type == RTC_PKT_STUN) {
        routed = listener_route_stun(listener, type, data, len, from);
    } else if (type == RTC_PKT_DTLS || type == RTC_PKT_RTP || type == RTC_PKT_RTCP) {
        routed = listener_route_tuple(listener, type, data, len, from);
    }
    if (routed)
        return;
    atomic_fetch_add_explicit(&listener->packets_unhandled, 1, memory_order_relaxed);
}

/* Worker poller readiness callback: runs on the worker loop thread and
 * drains the UDP socket, dispatching each packet to listener_on_packet. */
static void listener_io_ready(rtc_socket_t fd, void *user) {
    (void)fd;
    rtc_listener_t *listener = (rtc_listener_t *)user;
    rtc_packet_io_drain(&listener->io);
}

rtc_listener_t *rtc_listener_create(rtc_worker_t *worker, const rtc_listener_config_t *cfg) {
    if (!worker)
        return NULL;

    bool enable_udp = !cfg || cfg->enable_udp || !cfg->enable_tcp;
    if (!enable_udp || (cfg && cfg->enable_tcp)) {
        RTC_LOG_ERR("Listener: TCP is not implemented yet");
        return NULL;
    }

    rtc_listener_t *listener = (rtc_listener_t *)calloc(1, sizeof(*listener));
    if (!listener)
        return NULL;

    listener->worker = worker;
    listener->stun_port = cfg && cfg->stun_port ? cfg->stun_port : 3478;
    listener->gather_timer = RTC_WORKER_TIMER_INVALID;
    if (cfg && cfg->stun_server)
        copy_string(listener->stun_server, sizeof(listener->stun_server), cfg->stun_server);
    if (rtc_str_map_init_owned(&listener->ufrag_routes) != RTC_OK ||
        rtc_str_map_init_owned(&listener->txn_routes) != RTC_OK ||
        rtc_str_map_init_owned(&listener->tuple_routes) != RTC_OK) {
        rtc_str_map_free(&listener->ufrag_routes);
        rtc_str_map_free(&listener->txn_routes);
        rtc_str_map_free(&listener->tuple_routes);
        free(listener);
        return NULL;
    }
    if (rtc_mutex_init(&listener->route_mutex) != RTC_OK) {
        rtc_str_map_free(&listener->ufrag_routes);
        rtc_str_map_free(&listener->txn_routes);
        rtc_str_map_free(&listener->tuple_routes);
        free(listener);
        return NULL;
    }
    listener->route_mutex_ready = true;
    if (rtc_mutex_init(&listener->candidate_mutex) != RTC_OK) {
        rtc_str_map_free(&listener->ufrag_routes);
        rtc_str_map_free(&listener->txn_routes);
        rtc_str_map_free(&listener->tuple_routes);
        rtc_mutex_destroy(&listener->route_mutex);
        free(listener);
        return NULL;
    }
    listener->candidate_mutex_ready = true;

    rtc_packet_io_config_t io_cfg = {
        .listen_ip = cfg ? cfg->listen_ip : NULL,
        .port = cfg ? cfg->udp_port : 0,
    };
    int rc = rtc_packet_io_init_ex(&listener->io, &io_cfg, listener_on_packet, listener);
    if (rc != RTC_OK) {
        free_routes(&listener->ufrag_routes);
        free_routes(&listener->txn_routes);
        free_routes(&listener->tuple_routes);
        rtc_mutex_destroy(&listener->route_mutex);
        rtc_mutex_destroy(&listener->candidate_mutex);
        free(listener);
        return NULL;
    }

    rc = rtc_packet_io_get_local_addr(&listener->io, &listener->local_addr);
    if (rc != RTC_OK) {
        rtc_packet_io_close(&listener->io);
        free_routes(&listener->ufrag_routes);
        free_routes(&listener->txn_routes);
        free_routes(&listener->tuple_routes);
        rtc_mutex_destroy(&listener->route_mutex);
        rtc_mutex_destroy(&listener->candidate_mutex);
        free(listener);
        return NULL;
    }

    rc = listener_gather_host_candidates(listener, cfg);
    if (rc != RTC_OK) {
        rtc_packet_io_close(&listener->io);
        free_routes(&listener->ufrag_routes);
        free_routes(&listener->txn_routes);
        free_routes(&listener->tuple_routes);
        rtc_mutex_destroy(&listener->route_mutex);
        rtc_mutex_destroy(&listener->candidate_mutex);
        free(listener);
        return NULL;
    }

    /* Hand the socket to the worker: from here its loop thread drains
     * the socket and dispatches packets to listener_on_packet. */
    rc = rtc_worker_add_io(worker, rtc_packet_io_get_socket(&listener->io), listener_io_ready,
                           listener);
    if (rc != RTC_OK) {
        rtc_packet_io_close(&listener->io);
        free_routes(&listener->ufrag_routes);
        free_routes(&listener->txn_routes);
        free_routes(&listener->tuple_routes);
        rtc_mutex_destroy(&listener->route_mutex);
        rtc_mutex_destroy(&listener->candidate_mutex);
        free(listener);
        return NULL;
    }
    if (listener->stun_server[0] != '\0') {
        listener->gather_timer =
            rtc_worker_add_timer(worker, rtc_time_ms(), listener_start_stun_gather, listener);
    } else {
        listener_mark_gathering_done(listener);
    }
    return listener;
}

void rtc_listener_close(rtc_listener_t *listener) {
    if (!listener)
        return;
    bool was_closed = atomic_exchange_explicit(&listener->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    listener_cancel_gather(listener);
    /* Stop the worker polling this socket (runs on the loop thread, so no
     * drain can be in flight afterwards), then close the socket. */
    rtc_worker_remove_io(listener->worker, rtc_packet_io_get_socket(&listener->io));
    rtc_packet_io_close(&listener->io);
}

void rtc_listener_destroy(rtc_listener_t *listener) {
    if (!listener)
        return;
    rtc_listener_close(listener);
    if (listener->route_mutex_ready) {
        rtc_mutex_lock(&listener->route_mutex);
        free_routes(&listener->ufrag_routes);
        free_routes(&listener->txn_routes);
        free_routes(&listener->tuple_routes);
        rtc_mutex_unlock(&listener->route_mutex);
        rtc_mutex_destroy(&listener->route_mutex);
    }
    if (listener->candidate_mutex_ready)
        rtc_mutex_destroy(&listener->candidate_mutex);
    free(listener);
}

int rtc_listener_get_local_addr(rtc_listener_t *listener, rtc_addr_t *out) {
    if (!listener || !out)
        return RTC_ERR_INVALID;
    *out = listener->local_addr;
    return RTC_OK;
}

int rtc_listener_get_candidates(rtc_listener_t *listener, rtc_transport_candidate_t *out,
                                int *count) {
    if (!listener || !count)
        return RTC_ERR_INVALID;
    int capacity = *count;
    rtc_mutex_lock(&listener->candidate_mutex);
    int actual = listener->candidate_count;
    *count = actual;
    if (!out) {
        rtc_mutex_unlock(&listener->candidate_mutex);
        return RTC_OK;
    }
    int copy_count = capacity < actual ? capacity : actual;
    for (int i = 0; i < copy_count; i++)
        out[i] = listener->candidates[i];
    rtc_mutex_unlock(&listener->candidate_mutex);
    if (capacity < actual)
        return RTC_ERR_NOMEM;
    return RTC_OK;
}

bool rtc_listener_gathering_complete(rtc_listener_t *listener) {
    if (!listener)
        return false;
    rtc_mutex_lock(&listener->candidate_mutex);
    bool complete = listener->gathering_complete;
    rtc_mutex_unlock(&listener->candidate_mutex);
    return complete;
}

void rtc_listener_set_on_candidate(rtc_listener_t *listener, rtc_listener_on_candidate_fn fn,
                                   void *user) {
    if (!listener)
        return;
    rtc_transport_candidate_t snapshot[ICE_MAX_CANDIDATES];
    int count = 0;
    rtc_mutex_lock(&listener->candidate_mutex);
    listener->on_candidate = fn;
    listener->on_candidate_user = user;
    if (fn) {
        count = listener->candidate_count;
        for (int i = 0; i < count; i++)
            snapshot[i] = listener->candidates[i];
    }
    rtc_mutex_unlock(&listener->candidate_mutex);

    if (fn) {
        for (int i = 0; i < count; i++)
            fn(&snapshot[i], user);
    }
}

void rtc_listener_set_on_gathering_done(rtc_listener_t *listener,
                                        rtc_listener_on_gathering_done_fn fn, void *user) {
    if (!listener)
        return;
    bool complete = false;
    rtc_mutex_lock(&listener->candidate_mutex);
    listener->on_gathering_done = fn;
    listener->on_gathering_done_user = user;
    complete = listener->gathering_complete;
    rtc_mutex_unlock(&listener->candidate_mutex);
    if (fn && complete)
        fn(user);
}

static rtc_ice_candidate_type_t listener_ice_type(rtc_transport_candidate_type_t type) {
    switch (type) {
        case RTC_TRANSPORT_CANDIDATE_SRFLX:
            return ICE_CANDIDATE_SRFLX;
        case RTC_TRANSPORT_CANDIDATE_RELAY:
            return ICE_CANDIDATE_RELAY;
        case RTC_TRANSPORT_CANDIDATE_HOST:
        default:
            return ICE_CANDIDATE_HOST;
    }
}

int rtc_listener_candidate_to_ice(const rtc_transport_candidate_t *candidate, int local_pref,
                                  rtc_ice_candidate_t *out) {
    if (!candidate || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->type = listener_ice_type(candidate->type);
    out->component = 1;
    out->priority = rtc_ice_candidate_priority(out->type, local_pref, out->component);
    copy_string(out->foundation, sizeof(out->foundation), candidate->foundation);
    int rc = rtc_addr_from_string(&out->addr, candidate->address, candidate->port);
    if (rc != RTC_OK)
        return rc;
    if (candidate->has_related_address) {
        rc = rtc_addr_from_string(&out->related_addr, candidate->related_address,
                                  candidate->related_port);
        if (rc != RTC_OK)
            return rc;
        out->has_related_addr = true;
    }
    return RTC_OK;
}

int rtc_listener_fill_sdp_candidates(rtc_listener_t *listener, rtc_sdp_t *sdp) {
    if (!listener || !sdp)
        return RTC_ERR_INVALID;
    rtc_transport_candidate_t candidates[ICE_MAX_CANDIDATES];
    int count = ICE_MAX_CANDIDATES;
    int rc = rtc_listener_get_candidates(listener, candidates, &count);
    if (rc != RTC_OK && rc != RTC_ERR_NOMEM)
        return rc;

    int host_index = 0;
    int srflx_index = 0;
    for (int i = 0; i < count; i++) {
        rtc_ice_candidate_t c;
        memset(&c, 0, sizeof(c));
        c.type = listener_ice_type(candidates[i].type);
        c.component = 1;
        int local_pref = 65535;
        if (c.type == ICE_CANDIDATE_HOST)
            local_pref -= host_index++;
        else if (c.type == ICE_CANDIDATE_SRFLX)
            local_pref -= srflx_index++;
        rc = rtc_listener_candidate_to_ice(&candidates[i], local_pref, &c);
        if (rc != RTC_OK)
            return rc;
        rc = rtc_sdp_add_candidate(sdp, &c);
        if (rc != RTC_OK)
            return rc;
    }
    return RTC_OK;
}

int rtc_listener_get_stats(rtc_listener_t *listener, rtc_listener_stats_t *out) {
    if (!listener || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    rtc_packet_io_stats_t io_stats;
    rtc_packet_io_get_stats(&listener->io, &io_stats);

    out->closed = atomic_load_explicit(&listener->closed, memory_order_acquire);
    out->local_addr = listener->local_addr;
    out->packets_received = io_stats.pkts_recv;
    out->bytes_received = io_stats.bytes_recv;
    out->packets_sent = io_stats.pkts_sent;
    out->bytes_sent = io_stats.bytes_sent;
    out->packets_unhandled =
        atomic_load_explicit(&listener->packets_unhandled, memory_order_relaxed);
    return RTC_OK;
}

static int listener_register_route(rtc_listener_t *listener, rtc_str_map_t *map, const char *key,
                                   rtc_listener_packet_fn fn, void *user) {
    if (!listener || !key || !fn)
        return RTC_ERR_INVALID;
    listener_route_t *route = route_create(fn, user);
    if (!route)
        return RTC_ERR_NOMEM;

    rtc_mutex_lock(&listener->route_mutex);
    listener_route_t *old = (listener_route_t *)rtc_str_map_get(map, key);
    int rc = rtc_str_map_set(map, key, route);
    if (rc == RTC_OK) {
        free(old);
    } else {
        free(route);
    }
    rtc_mutex_unlock(&listener->route_mutex);
    return rc;
}

static void listener_unregister_route(rtc_listener_t *listener, rtc_str_map_t *map,
                                      const char *key) {
    if (!listener || !key)
        return;
    rtc_mutex_lock(&listener->route_mutex);
    listener_route_t *route = (listener_route_t *)rtc_str_map_get(map, key);
    if (rtc_str_map_remove(map, key))
        free(route);
    rtc_mutex_unlock(&listener->route_mutex);
}

int rtc_listener_register_ufrag(rtc_listener_t *listener, const char *ufrag,
                                rtc_listener_packet_fn fn, void *user) {
    return listener_register_route(listener, &listener->ufrag_routes, ufrag, fn, user);
}

void rtc_listener_unregister_ufrag(rtc_listener_t *listener, const char *ufrag) {
    if (!listener)
        return;
    listener_unregister_route(listener, &listener->ufrag_routes, ufrag);
}

int rtc_listener_register_stun_txn(rtc_listener_t *listener, const uint8_t txn_id[STUN_TXN_ID_SIZE],
                                   rtc_listener_packet_fn fn, void *user) {
    if (!txn_id)
        return RTC_ERR_INVALID;
    char key[STUN_TXN_ID_SIZE * 2 + 1];
    txn_key(txn_id, key);
    return listener_register_route(listener, &listener->txn_routes, key, fn, user);
}

void rtc_listener_unregister_stun_txn(rtc_listener_t *listener,
                                      const uint8_t txn_id[STUN_TXN_ID_SIZE]) {
    if (!listener || !txn_id)
        return;
    char key[STUN_TXN_ID_SIZE * 2 + 1];
    txn_key(txn_id, key);
    listener_unregister_route(listener, &listener->txn_routes, key);
}

int rtc_listener_register_tuple(rtc_listener_t *listener, const rtc_addr_t *remote,
                                rtc_listener_packet_fn fn, void *user) {
    char key[128];
    int rc = tuple_key(remote, key, sizeof(key));
    if (rc != RTC_OK)
        return rc;
    return listener_register_route(listener, &listener->tuple_routes, key, fn, user);
}

void rtc_listener_unregister_tuple(rtc_listener_t *listener, const rtc_addr_t *remote) {
    if (!listener || !remote)
        return;
    char key[128];
    if (tuple_key(remote, key, sizeof(key)) != RTC_OK)
        return;
    listener_unregister_route(listener, &listener->tuple_routes, key);
}

int rtc_listener_send_to(rtc_listener_t *listener, const uint8_t *data, size_t len,
                         const rtc_addr_t *dest) {
    if (!listener || !data || !dest)
        return RTC_ERR_INVALID;
    return rtc_packet_io_send(&listener->io, data, len, dest);
}
