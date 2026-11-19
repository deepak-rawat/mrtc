/*
 * SFU inbound media producer.
 */
#include "rtc/rtc_producer.h"
#include "rtc/rtc_vec.h"
#include "rtc/rtc_rtcp.h"

#include "rtc_consumer_internal.h"
#include "rtc_producer_internal.h"
#include "rtc_router_internal.h"
#include "rtc_transport_internal.h"
#include "rtc_worker_internal.h"

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
    _Atomic bool source_active;
    _Atomic bool source_ended;
    uint64_t last_activity_ms;
    _Atomic uint64_t packets_received;
    _Atomic uint64_t bytes_received;
    _Atomic uint64_t last_keyframe_request_ms;
    rtc_rtcp_stats_t rtcp;
    _Atomic uint8_t feedback;
    uint32_t last_bitrate_limit;
    uint64_t last_bitrate_feedback_ms;
    _Atomic unsigned fir_sequence;
    rtc_worker_t *worker;
    rtc_worker_timer_t report_timer;
};

static _Atomic uint32_t g_next_producer_id = 1;

static void producer_report_timer(void *user) {
    rtc_producer_t *producer = user;
    producer->report_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load(&producer->closed) || atomic_load(&producer->source_ended) ||
        !producer->last_activity_ms)
        return;
    uint64_t now = rtc_time_ms();
    uint64_t timeout = rtc_rtcp_source_timeout_ms(&producer->rtcp);
    if (rtc_producer_expire_source(producer, now))
        return;
    int rc = producer->rtcp.packets_received
                 ? rtc_transport_send_report(producer->transport, &producer->rtcp, false)
                 : RTC_ERR_TIMEOUT;
    if (rc != RTC_OK && rc != RTC_ERR_TIMEOUT)
        RTC_LOG_WARN("Producer receiver report failed: %d", rc);
    uint64_t expiry = timeout > UINT64_MAX - producer->last_activity_ms
                          ? UINT64_MAX
                          : producer->last_activity_ms + timeout;
    uint64_t next =
        producer->rtcp.next_report_time > now ? producer->rtcp.next_report_time : now + 1000;
    if (!producer->rtcp.packets_received || expiry < next)
        next = expiry;
    if ((rc == RTC_OK || rc == RTC_ERR_TIMEOUT) && next > now + RTC_RTCP_RECONSIDER_POLL_MS)
        next = now + RTC_RTCP_RECONSIDER_POLL_MS;
    producer->report_timer =
        rtc_worker_add_timer(producer->worker, next, producer_report_timer, producer);
    if (producer->report_timer == RTC_WORKER_TIMER_INVALID)
        RTC_LOG_ERR("Producer report timer allocation failed");
}

static void producer_retire_on_worker(void *user) {
    rtc_producer_t *producer = user;
    if (producer->report_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(producer->worker, producer->report_timer);
        producer->report_timer = RTC_WORKER_TIMER_INVALID;
    }
}

bool rtc_producer_expire_source(rtc_producer_t *producer, uint64_t now_ms) {
    if (!producer || atomic_load(&producer->closed) || atomic_load(&producer->source_ended) ||
        !producer->last_activity_ms || now_ms < producer->last_activity_ms ||
        now_ms - producer->last_activity_ms < rtc_rtcp_source_timeout_ms(&producer->rtcp))
        return false;
    producer_retire_on_worker(producer);
    atomic_store(&producer->source_active, false);
    producer->last_activity_ms = 0;
    rtc_rtcp_stats_reset_source(&producer->rtcp);
    RTC_LOG_DBG("Producer RTP source 0x%08x expired", producer->rtp.ssrc);
    return true;
}

static void copy_label(char *dst, size_t dst_len, const char *src) {
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void producer_note_activity(rtc_producer_t *producer) {
    producer->last_activity_ms = rtc_time_ms();
    atomic_store(&producer->source_active, true);
    if (producer->report_timer == RTC_WORKER_TIMER_INVALID) {
        producer->report_timer =
            rtc_worker_add_timer(producer->worker, rtc_time_ms(), producer_report_timer, producer);
        if (producer->report_timer == RTC_WORKER_TIMER_INVALID)
            RTC_LOG_ERR("Producer report timer allocation failed");
    }
}

static void producer_end_source(rtc_producer_t *producer) {
    if (atomic_exchange(&producer->source_ended, true))
        return;
    atomic_store(&producer->source_active, false);
    producer->last_activity_ms = 0;
    rtc_rtcp_stats_reset_source(&producer->rtcp);
    producer_retire_on_worker(producer);
    rtc_mutex_lock(&producer->consumers_mutex);
    for (size_t i = 0; i < rtc_vec_len(&producer->consumers); i++) {
        rtc_consumer_t **slot = rtc_vec_at(&producer->consumers, i);
        rtc_consumer_source_ended(*slot);
    }
    rtc_mutex_unlock(&producer->consumers_mutex);
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
    producer->worker = rtc_transport_worker(transport);
    producer->report_timer = RTC_WORKER_TIMER_INVALID;
    producer->kind = opts->kind;
    producer->rtp = opts->rtp;
    copy_label(producer->label, sizeof(producer->label), opts->label);
    producer->app_data = opts->app_data;
    atomic_init(&producer->feedback, RTC_SDP_FB_NACK | RTC_SDP_FB_PLI | RTC_SDP_FB_FIR);
    producer->last_bitrate_limit = UINT32_MAX;
    rtc_rtcp_stats_init(&producer->rtcp, id);
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

void rtc_producer_set_rtcp_feedback(rtc_producer_t *producer, uint8_t feedback) {
    if (producer)
        atomic_store(&producer->feedback, feedback);
}

int rtc_producer_handle_rtcp(rtc_producer_t *producer, const uint8_t *data, size_t len) {
    if (!producer || !data || atomic_load(&producer->closed))
        return RTC_ERR_INVALID;
    if (rtc_rtcp_validate_compound(data, len) != RTC_OK)
        return RTC_ERR_INVALID;
    if (atomic_load(&producer->source_ended))
        return RTC_OK;
    size_t offset = 0;
    while (len - offset >= 4) {
        size_t size = (((size_t)data[offset + 2] << 8) | data[offset + 3]) * 4 + 4;
        if (size > len - offset || (data[offset] >> 6) != 2)
            return RTC_ERR_INVALID;
        if (data[offset + 1] == RTCP_PT_BYE) {
            rtc_rtcp_bye_t bye;
            if (rtc_rtcp_parse_bye(&bye, data + offset, size) != RTC_OK)
                return RTC_ERR_INVALID;
            for (unsigned i = 0; i < bye.count; i++)
                if (bye.ssrcs[i] == producer->rtp.ssrc) {
                    producer_end_source(producer);
                    return RTC_OK;
                }
        } else if (data[offset + 1] == RTCP_PT_SR || data[offset + 1] == RTCP_PT_RR) {
            rtc_rtcp_packet_t report;
            if (rtc_rtcp_parse(&report, data + offset, size) != RTC_OK)
                return RTC_ERR_INVALID;
            if (report.sender_ssrc == producer->rtp.ssrc) {
                producer_note_activity(producer);
                if (data[offset + 1] == RTCP_PT_SR)
                    rtc_rtcp_stats_on_sr_recv(&producer->rtcp, &report.sr);
            }
        }
        offset += size;
    }
    return offset == len ? RTC_OK : RTC_ERR_INVALID;
}

typedef struct {
    rtc_producer_t *producer;
    int result;
} producer_bitrate_args_t;

static uint32_t producer_bitrate_limit(rtc_producer_t *producer, uint64_t now) {
    uint32_t limit = UINT32_MAX;
    rtc_mutex_lock(&producer->consumers_mutex);
    for (size_t i = 0; i < rtc_vec_len(&producer->consumers); i++) {
        rtc_consumer_t **slot = rtc_vec_at(&producer->consumers, i);
        uint32_t candidate = rtc_consumer_bitrate_limit(*slot, now);
        if (candidate < limit)
            limit = candidate;
    }
    rtc_mutex_unlock(&producer->consumers_mutex);
    return limit;
}

static void producer_update_bitrate_on_worker(void *user) {
    producer_bitrate_args_t *args = user;
    rtc_producer_t *producer = args->producer;
    args->result = RTC_OK;
    if (atomic_load(&producer->closed) || atomic_load(&producer->source_ended) ||
        producer->kind != RTC_MEDIA_KIND_VIDEO ||
        !(atomic_load(&producer->feedback) & RTC_SDP_FB_REMB))
        return;
    uint64_t now = rtc_time_ms();
    uint32_t limit = producer_bitrate_limit(producer, now);
    if ((limit == UINT32_MAX && producer->last_bitrate_limit == UINT32_MAX) ||
        now - producer->last_bitrate_feedback_ms < 200 ||
        (limit == producer->last_bitrate_limit && now - producer->last_bitrate_feedback_ms < 1000))
        return;
    producer->last_bitrate_feedback_ms = now;
    uint8_t packet[64 + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    size_t len;
    int rc = rtc_rtcp_build_remb(packet, sizeof(packet), &len, producer->rtcp.ssrc,
                                 producer->rtp.ssrc, limit);
    if (rc == RTC_OK)
        rc = rtc_transport_send_rtcp(producer->transport, packet, &len, sizeof(packet));
    args->result = rc;
    if (rc == RTC_OK)
        producer->last_bitrate_limit = limit;
    else
        RTC_LOG_WARN("Producer bitrate feedback failed: %d", rc);
}

int rtc_producer_update_bitrate(rtc_producer_t *producer) {
    if (!producer)
        return RTC_ERR_INVALID;
    producer_bitrate_args_t args = {.producer = producer, .result = RTC_ERR_INVALID};
    int rc = rtc_transport_invoke(producer->transport, producer_update_bitrate_on_worker, &args);
    return rc == RTC_OK ? args.result : rc;
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
    atomic_store(&producer->source_active, false);
    if (producer->transport && producer->rtp.ssrc != 0)
        rtc_router_unregister_producer(producer->transport, producer->rtp.ssrc);
    rtc_worker_invoke(producer->worker, producer_retire_on_worker, producer);
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
    out->source_active = atomic_load(&producer->source_active);
    out->source_ended = atomic_load(&producer->source_ended);
    out->kind = producer->kind;
    out->packets_received = atomic_load_explicit(&producer->packets_received, memory_order_relaxed);
    out->bytes_received = atomic_load_explicit(&producer->bytes_received, memory_order_relaxed);
    out->target_bitrate_bps = producer_bitrate_limit(producer, rtc_time_ms());
    return RTC_OK;
}

int rtc_producer_request_keyframe(rtc_producer_t *producer) {
    if (!producer || atomic_load_explicit(&producer->closed, memory_order_acquire) ||
        atomic_load(&producer->source_ended))
        return RTC_ERR_INVALID;
    if (producer->kind != RTC_MEDIA_KIND_VIDEO)
        return RTC_OK;
    uint64_t now = rtc_time_ms();
    uint64_t previous =
        atomic_load_explicit(&producer->last_keyframe_request_ms, memory_order_relaxed);
    if (now - previous < 250)
        return RTC_OK;
    if (!atomic_compare_exchange_strong(&producer->last_keyframe_request_ms, &previous, now))
        return RTC_OK;
    uint8_t feedback = atomic_load(&producer->feedback);
    if (!(feedback & (RTC_SDP_FB_PLI | RTC_SDP_FB_FIR))) {
        RTC_LOG_WARN("Producer keyframe feedback was not negotiated");
        return RTC_ERR_INVALID;
    }
    uint8_t packet[64 + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    size_t len = 0;
    int rc = feedback & RTC_SDP_FB_PLI
                 ? rtc_rtcp_build_pli(packet, sizeof(packet), &len, producer->rtcp.ssrc,
                                      producer->rtp.ssrc)
                 : rtc_rtcp_build_fir(packet, sizeof(packet), &len, producer->rtcp.ssrc,
                                      producer->rtp.ssrc,
                                      (uint8_t)atomic_fetch_add(&producer->fir_sequence, 1));
    if (rc != RTC_OK)
        return rc;
    return rtc_transport_send_rtcp(producer->transport, packet, &len, sizeof(packet));
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
    if (atomic_load(&producer->feedback) & RTC_SDP_FB_REMB)
        (void)rtc_producer_update_bitrate(producer);
}

void rtc_producer_on_rtp(rtc_producer_t *producer, const rtc_rtp_packet_t *pkt) {
    if (!producer || !pkt || pkt->header.ssrc != producer->rtp.ssrc ||
        atomic_load_explicit(&producer->closed, memory_order_acquire) ||
        atomic_load(&producer->source_ended))
        return;
    atomic_fetch_add_explicit(&producer->packets_received, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&producer->bytes_received, (uint64_t)pkt->payload_len,
                              memory_order_relaxed);
    uint32_t clock_rate = producer->rtp.codec_count ? producer->rtp.codecs[0].clock_rate : 90000;
    rtc_rtcp_stats_on_rtp_recv(&producer->rtcp, pkt->header.seq, pkt->header.timestamp,
                               pkt->header.ssrc, clock_rate);
    producer_note_activity(producer);

    rtc_mutex_lock(&producer->consumers_mutex);
    size_t count = rtc_vec_len(&producer->consumers);
    for (size_t i = 0; i < count; i++) {
        rtc_consumer_t **slot = (rtc_consumer_t **)rtc_vec_at(&producer->consumers, i);
        if (slot && *slot)
            rtc_consumer_on_producer_rtp(*slot, pkt);
    }
    rtc_mutex_unlock(&producer->consumers_mutex);
    if (atomic_load(&producer->feedback) & RTC_SDP_FB_REMB)
        (void)rtc_producer_update_bitrate(producer);
}
