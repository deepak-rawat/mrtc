/*
 * SFU inbound media producer skeleton.
 */
#include "rtc/rtc_producer.h"
#include "rtc/rtc_vec.h"

#include "rtc_consumer_internal.h"
#include "rtc_producer_internal.h"
#include "rtc_router_internal.h"

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
    rtc_vec_t consumers;
    rtc_mutex_t consumers_mutex;
    bool consumers_ready;
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
    if (!transport || !opts || opts->rtp.ssrc == 0)
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
    if (rtc_vec_init(&producer->consumers, sizeof(rtc_consumer_t *)) != RTC_OK ||
        rtc_mutex_init(&producer->consumers_mutex) != RTC_OK) {
        rtc_vec_free(&producer->consumers);
        free(producer);
        return NULL;
    }
    producer->consumers_ready = true;
    if (rtc_router_register_producer(transport, producer, producer->rtp.ssrc) != RTC_OK) {
        rtc_mutex_destroy(&producer->consumers_mutex);
        rtc_vec_free(&producer->consumers);
        free(producer);
        return NULL;
    }
    return producer;
}

const char *rtc_producer_id(const rtc_producer_t *producer) {
    return producer ? producer->id : "";
}

int rtc_producer_get_rtp_parameters(rtc_producer_t *producer, rtc_rtp_parameters_t *out) {
    if (!producer || !out)
        return RTC_ERR_INVALID;
    *out = producer->rtp;
    return RTC_OK;
}

void rtc_producer_close(rtc_producer_t *producer) {
    if (!producer)
        return;
    bool was_closed = atomic_exchange_explicit(&producer->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    if (producer->transport && producer->rtp.ssrc != 0)
        rtc_router_unregister_producer(producer->transport, producer->rtp.ssrc);
}

void rtc_producer_destroy(rtc_producer_t *producer) {
    if (!producer)
        return;
    rtc_producer_close(producer);
    if (producer->consumers_ready) {
        rtc_mutex_destroy(&producer->consumers_mutex);
        producer->consumers_ready = false;
    }
    rtc_vec_free(&producer->consumers);
    free(producer);
}

int rtc_producer_get_stats(rtc_producer_t *producer, rtc_producer_stats_t *out) {
    if (!producer || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->closed = atomic_load_explicit(&producer->closed, memory_order_acquire);
    out->kind = producer->kind;
    out->packets_received = atomic_load_explicit(&producer->packets_received, memory_order_relaxed);
    out->bytes_received = atomic_load_explicit(&producer->bytes_received, memory_order_relaxed);
    return RTC_OK;
}

uint32_t rtc_producer_ssrc(const rtc_producer_t *producer) {
    return producer ? producer->rtp.ssrc : 0;
}

int rtc_producer_add_consumer(rtc_producer_t *producer, rtc_consumer_t *consumer) {
    if (!producer || !consumer)
        return RTC_ERR_INVALID;
    if (atomic_load_explicit(&producer->closed, memory_order_acquire))
        return RTC_ERR_INVALID;

    rtc_mutex_lock(&producer->consumers_mutex);
    int rc = rtc_vec_push(&producer->consumers, &consumer);
    rtc_mutex_unlock(&producer->consumers_mutex);
    return rc;
}

void rtc_producer_remove_consumer(rtc_producer_t *producer, rtc_consumer_t *consumer) {
    if (!producer || !consumer || !producer->consumers_ready)
        return;

    rtc_mutex_lock(&producer->consumers_mutex);
    size_t count = rtc_vec_len(&producer->consumers);
    for (size_t i = 0; i < count; i++) {
        rtc_consumer_t **slot = (rtc_consumer_t **)rtc_vec_at(&producer->consumers, i);
        if (slot && *slot == consumer) {
            rtc_vec_swap_remove(&producer->consumers, i);
            break;
        }
    }
    rtc_mutex_unlock(&producer->consumers_mutex);
}

void rtc_producer_on_rtp(rtc_producer_t *producer, const rtc_rtp_packet_t *pkt) {
    if (!producer || !pkt || atomic_load_explicit(&producer->closed, memory_order_acquire))
        return;
    atomic_fetch_add_explicit(&producer->packets_received, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&producer->bytes_received, (uint64_t)pkt->payload_len,
                              memory_order_relaxed);

    rtc_mutex_lock(&producer->consumers_mutex);
    size_t count = rtc_vec_len(&producer->consumers);
    for (size_t i = 0; i < count; i++) {
        rtc_consumer_t **slot = (rtc_consumer_t **)rtc_vec_at(&producer->consumers, i);
        if (slot && *slot)
            rtc_consumer_on_producer_rtp(*slot, pkt);
    }
    rtc_mutex_unlock(&producer->consumers_mutex);
}
