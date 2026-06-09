/*
 * rtc_listener.c - Shared RTC listener foundation.
 */
#include "rtc/rtc_listener.h"

#include "rtc/rtc_str_map.h"
#include "rtc_listener_internal.h"
#include "rtc_packet_io.h"
#include "rtc_stun.h"

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
    rtc_transport_candidate_t candidate;
    rtc_addr_t local_addr;
    rtc_str_map_t ufrag_routes;
    rtc_str_map_t txn_routes;
    rtc_str_map_t tuple_routes;
    rtc_mutex_t route_mutex;
    bool route_mutex_ready;
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

static int listener_find_host_ip(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return RTC_ERR_INVALID;

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

    int rc = RTC_ERR_GENERIC;
    for (PIP_ADAPTER_ADDRESSES a = addrs; a && rc != RTC_OK; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ua->Address.lpSockaddr;
            if (!sin || sin->sin_family != AF_INET)
                continue;
            if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
                continue;
            if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)out_len)) {
                rc = RTC_OK;
                break;
            }
        }
    }
    free(addrs);
    return rc;
#else
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0)
        return RTC_ERR_SOCKET;

    int rc = RTC_ERR_GENERIC;
    for (struct ifaddrs *ifa = ifap; ifa && rc != RTC_OK; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, out_len))
            rc = RTC_OK;
    }
    freeifaddrs(ifap);
    return rc;
#endif
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

static int listener_fill_candidate(rtc_listener_t *listener, const rtc_listener_config_t *cfg) {
    char ip[64];
    uint16_t port = 0;
    int rc = rtc_addr_to_string(&listener->local_addr, ip, sizeof(ip), &port);
    if (rc != RTC_OK)
        return rc;

    memset(&listener->candidate, 0, sizeof(listener->candidate));
    copy_string(listener->candidate.foundation, sizeof(listener->candidate.foundation), "H0");
    copy_string(listener->candidate.protocol, sizeof(listener->candidate.protocol), "udp");
    listener->candidate.port = port;
    listener->candidate.type = RTC_TRANSPORT_CANDIDATE_HOST;

    if (cfg && cfg->announced_ip && cfg->announced_ip[0] != '\0') {
        copy_string(listener->candidate.address, sizeof(listener->candidate.address),
                    cfg->announced_ip);
    } else if (cfg && !is_wildcard_ip(cfg->listen_ip)) {
        copy_string(listener->candidate.address, sizeof(listener->candidate.address),
                    cfg->listen_ip);
    } else {
        char host_ip[64];
        if (is_wildcard_ip(ip) && listener_find_host_ip(host_ip, sizeof(host_ip)) == RTC_OK) {
            copy_string(listener->candidate.address, sizeof(listener->candidate.address), host_ip);
        } else {
            copy_string(listener->candidate.address, sizeof(listener->candidate.address), ip);
        }
    }
    return RTC_OK;
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
    if (rtc_str_map_init_owned(&listener->ufrag_routes) != RTC_OK ||
        rtc_str_map_init_owned(&listener->txn_routes) != RTC_OK ||
        rtc_str_map_init_owned(&listener->tuple_routes) != RTC_OK ||
        rtc_mutex_init(&listener->route_mutex) != RTC_OK) {
        rtc_str_map_free(&listener->ufrag_routes);
        rtc_str_map_free(&listener->txn_routes);
        rtc_str_map_free(&listener->tuple_routes);
        free(listener);
        return NULL;
    }
    listener->route_mutex_ready = true;

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
        free(listener);
        return NULL;
    }

    rc = listener_fill_candidate(listener, cfg);
    if (rc != RTC_OK) {
        rtc_packet_io_close(&listener->io);
        free_routes(&listener->ufrag_routes);
        free_routes(&listener->txn_routes);
        free_routes(&listener->tuple_routes);
        rtc_mutex_destroy(&listener->route_mutex);
        free(listener);
        return NULL;
    }
    return listener;
}

void rtc_listener_close(rtc_listener_t *listener) {
    if (!listener)
        return;
    bool was_closed = atomic_exchange_explicit(&listener->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
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
    *count = 1;
    if (!out)
        return RTC_OK;
    if (capacity < 1)
        return RTC_ERR_NOMEM;
    out[0] = listener->candidate;
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
