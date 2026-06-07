/*
 * rtc_consumer.c - SFU outbound media consumer skeleton.
 */
#include "rtc/rtc_consumer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rtc_consumer {
    char id[32];
    rtc_transport_t *transport;
    rtc_producer_t *producer;
    rtc_rtp_parameters_t rtp;
    rtc_media_kind_t kind;
    void *app_data;
    _Atomic bool closed;
    _Atomic bool paused;
    _Atomic uint64_t packets_sent;
    _Atomic uint64_t bytes_sent;
};

static _Atomic uint32_t g_next_consumer_id = 1;

rtc_consumer_t *rtc_transport_consume(rtc_transport_t *transport,
                                      const rtc_consumer_options_t *opts) {
    if (!transport || !opts || !opts->producer)
        return NULL;

    rtc_transport_stats_t transport_stats;
    rtc_producer_stats_t producer_stats;
    if (rtc_transport_get_stats(transport, &transport_stats) != RTC_OK || transport_stats.closed)
        return NULL;
    if (rtc_producer_get_stats(opts->producer, &producer_stats) != RTC_OK || producer_stats.closed)
        return NULL;

    rtc_consumer_t *consumer = (rtc_consumer_t *)calloc(1, sizeof(*consumer));
    if (!consumer)
        return NULL;

    uint32_t id = atomic_fetch_add_explicit(&g_next_consumer_id, 1, memory_order_relaxed);
    snprintf(consumer->id, sizeof(consumer->id), "cons-%u", (unsigned)id);
    consumer->transport = transport;
    consumer->producer = opts->producer;
    consumer->kind = producer_stats.kind;
    consumer->app_data = opts->app_data;
    atomic_store_explicit(&consumer->paused, opts->paused, memory_order_release);
    return consumer;
}

const char *rtc_consumer_id(const rtc_consumer_t *consumer) {
    return consumer ? consumer->id : "";
}

int rtc_consumer_get_rtp_parameters(rtc_consumer_t *consumer, rtc_rtp_parameters_t *out) {
    if (!consumer || !out)
        return RTC_ERR_INVALID;
    *out = consumer->rtp;
    return RTC_OK;
}

void rtc_consumer_pause(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    atomic_store_explicit(&consumer->paused, true, memory_order_release);
}

void rtc_consumer_resume(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    atomic_store_explicit(&consumer->paused, false, memory_order_release);
}

void rtc_consumer_close(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    atomic_store_explicit(&consumer->closed, true, memory_order_release);
}

void rtc_consumer_destroy(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    rtc_consumer_close(consumer);
    free(consumer);
}

int rtc_consumer_get_stats(rtc_consumer_t *consumer, rtc_consumer_stats_t *out) {
    if (!consumer || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->closed = atomic_load_explicit(&consumer->closed, memory_order_acquire);
    out->paused = atomic_load_explicit(&consumer->paused, memory_order_acquire);
    out->kind = consumer->kind;
    out->packets_sent = atomic_load_explicit(&consumer->packets_sent, memory_order_relaxed);
    out->bytes_sent = atomic_load_explicit(&consumer->bytes_sent, memory_order_relaxed);
    return RTC_OK;
}