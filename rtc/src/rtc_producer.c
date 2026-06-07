/*
 * rtc_producer.c - SFU inbound media producer skeleton.
 */
#include "rtc/rtc_producer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rtc_producer {
    char id[32];
    rtc_transport_t *transport;
    rtc_media_kind_t kind;
    rtc_rtp_parameters_t rtp;
    char label[64];
    void *app_data;
    _Atomic bool closed;
    _Atomic uint64_t packets_received;
    _Atomic uint64_t bytes_received;
};

static _Atomic uint32_t g_next_producer_id = 1;

static void copy_label(char *dst, size_t dst_len, const char *src) {
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

rtc_producer_t *rtc_transport_produce(rtc_transport_t *transport,
                                      const rtc_producer_options_t *opts) {
    if (!transport || !opts)
        return NULL;

    rtc_transport_stats_t transport_stats;
    if (rtc_transport_get_stats(transport, &transport_stats) != RTC_OK || transport_stats.closed)
        return NULL;

    rtc_producer_t *producer = (rtc_producer_t *)calloc(1, sizeof(*producer));
    if (!producer)
        return NULL;

    uint32_t id = atomic_fetch_add_explicit(&g_next_producer_id, 1, memory_order_relaxed);
    snprintf(producer->id, sizeof(producer->id), "prod-%u", (unsigned)id);
    producer->transport = transport;
    producer->kind = opts->kind;
    producer->rtp = opts->rtp;
    copy_label(producer->label, sizeof(producer->label), opts->label);
    producer->app_data = opts->app_data;
    return producer;
}

const char *rtc_producer_id(const rtc_producer_t *producer) {
    return producer ? producer->id : "";
}

void rtc_producer_close(rtc_producer_t *producer) {
    if (!producer)
        return;
    atomic_store_explicit(&producer->closed, true, memory_order_release);
}

void rtc_producer_destroy(rtc_producer_t *producer) {
    if (!producer)
        return;
    rtc_producer_close(producer);
    free(producer);
}

int rtc_producer_get_stats(rtc_producer_t *producer, rtc_producer_stats_t *out) {
    if (!producer || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->closed = atomic_load_explicit(&producer->closed, memory_order_acquire);
    out->kind = producer->kind;
    out->packets_received =
        atomic_load_explicit(&producer->packets_received, memory_order_relaxed);
    out->bytes_received = atomic_load_explicit(&producer->bytes_received, memory_order_relaxed);
    return RTC_OK;
}