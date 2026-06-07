/*
 * rtc_listener.c - Shared RTC listener foundation.
 */
#include "rtc/rtc_listener.h"

#include "rtc_packet_io.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct rtc_listener {
    rtc_worker_t *worker;
    rtc_packet_io_t io;
    rtc_transport_candidate_t candidate;
    rtc_addr_t local_addr;
    _Atomic bool closed;
    _Atomic uint64_t packets_unhandled;
};

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

static void listener_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                               const rtc_addr_t *from, void *user) {
    (void)type;
    (void)data;
    (void)len;
    (void)from;
    rtc_listener_t *listener = (rtc_listener_t *)user;
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
        copy_string(listener->candidate.address, sizeof(listener->candidate.address), cfg->listen_ip);
    } else {
        copy_string(listener->candidate.address, sizeof(listener->candidate.address), ip);
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
    rtc_packet_io_config_t io_cfg = {
        .listen_ip = cfg ? cfg->listen_ip : NULL,
        .port = cfg ? cfg->udp_port : 0,
    };
    int rc = rtc_packet_io_init_ex(&listener->io, &io_cfg, listener_on_packet, listener);
    if (rc != RTC_OK) {
        free(listener);
        return NULL;
    }

    rc = rtc_packet_io_get_local_addr(&listener->io, &listener->local_addr);
    if (rc != RTC_OK) {
        rtc_packet_io_close(&listener->io);
        free(listener);
        return NULL;
    }

    rc = listener_fill_candidate(listener, cfg);
    if (rc != RTC_OK) {
        rtc_packet_io_close(&listener->io);
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