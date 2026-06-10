/*
 * SFU outbound media consumer skeleton.
 */
#include "rtc/rtc_consumer.h"

#include "rtc_consumer_internal.h"
#include "rtc_producer_internal.h"
#include "rtc_transport_internal.h"

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
    uint16_t seq;
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
    if (rtc_producer_get_rtp_parameters(opts->producer, &consumer->rtp) != RTC_OK) {
        free(consumer);
        return NULL;
    }
    rtc_random_bytes((uint8_t *)&consumer->rtp.ssrc, sizeof(consumer->rtp.ssrc));
    if (consumer->rtp.ssrc == 0)
        consumer->rtp.ssrc = 1;
    if (consumer->rtp.mid[0] == '\0') {
        size_t len = strlen(consumer->id);
        if (len >= sizeof(consumer->rtp.mid))
            len = sizeof(consumer->rtp.mid) - 1;
        memcpy(consumer->rtp.mid, consumer->id, len);
        consumer->rtp.mid[len] = '\0';
    }
    rtc_random_bytes((uint8_t *)&consumer->seq, sizeof(consumer->seq));
    consumer->app_data = opts->app_data;
    atomic_store_explicit(&consumer->paused, opts->paused, memory_order_release);
    if (rtc_producer_add_consumer(opts->producer, consumer) != RTC_OK) {
        free(consumer);
        return NULL;
    }
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
    bool was_closed = atomic_exchange_explicit(&consumer->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    if (consumer->producer)
        rtc_producer_remove_consumer(consumer->producer, consumer);
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

void rtc_consumer_on_producer_rtp(rtc_consumer_t *consumer, const rtc_rtp_packet_t *pkt) {
    if (!consumer || !pkt)
        return;
    if (atomic_load_explicit(&consumer->closed, memory_order_acquire) ||
        atomic_load_explicit(&consumer->paused, memory_order_acquire)) {
        return;
    }

    rtc_rtp_packet_t out_pkt;
    uint16_t seq = consumer->seq++;
    int rc = rtc_rtp_build(&out_pkt, pkt->header.payload_type, seq, pkt->header.timestamp,
                           consumer->rtp.ssrc, pkt->header.marker, pkt->payload, pkt->payload_len);
    if (rc != RTC_OK)
        return;

    size_t wire_len = out_pkt.buf_len;
    rc = rtc_transport_send_rtp(consumer->transport, out_pkt.buf, &wire_len, sizeof(out_pkt.buf));
    if (rc != RTC_OK)
        return;

    atomic_fetch_add_explicit(&consumer->packets_sent, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&consumer->bytes_sent, (uint64_t)pkt->payload_len,
                              memory_order_relaxed);
}
