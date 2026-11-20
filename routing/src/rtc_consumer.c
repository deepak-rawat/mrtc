/*
 * SFU outbound media consumer.
 */
#include "rtc/rtc_consumer.h"

#include "rtc_consumer_internal.h"
#include "rtc_producer_internal.h"
#include "rtc/rtc_rtcp.h"
#include "rtc/rtc_rtp_ext.h"
#include "rtc_rate_control.h"
#include "rtc_transport_internal.h"
#include "rtc_worker_internal.h"

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
    uint16_t initial_seq;
    uint16_t seq_offset;
    uint16_t highest_source_seq;
    bool source_initialized;
    uint8_t mid_ext_id;
    uint64_t source_generation;
    rtc_mutex_t media_mutex;
    void *app_data;
    _Atomic bool closed;
    _Atomic bool paused;
    _Atomic bool source_ended;
    _Atomic uint64_t packets_sent;
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t packets_dropped;
    _Atomic uint64_t packets_retransmitted;
    struct {
        uint16_t seq;
        bool used;
        uint8_t attempts;
        size_t len;
        uint8_t data[sizeof(((rtc_rtp_packet_t *)0)->buf)];
    } history[RTC_CONSUMER_HISTORY_SIZE];
    rtc_rtcp_stats_t rtcp;
    rtc_rate_controller_t *rate_controller;
    _Atomic uint32_t estimated_bitrate_bps;
    _Atomic uint64_t bitrate_feedback_ms;
    _Atomic uint32_t allocated_bitrate_bps;
    _Atomic bool allocation_excluded;
    _Atomic uint64_t last_packet_ms;
    rtc_worker_t *worker;
    rtc_worker_timer_t report_timer;
};

static void consumer_sync_sent(rtc_consumer_t *consumer) {
    rtc_pacer_sent_t sent;
    if (rtc_transport_rtp_sent(consumer->transport, consumer->rtp.ssrc, &sent)) {
        (void)rtc_transport_sync_rtp_stats(consumer->transport, &consumer->rtcp, NULL);
        atomic_store(&consumer->packets_sent, sent.packets_sent);
        atomic_store(&consumer->bytes_sent, sent.octets_sent);
    }
}

static _Atomic uint32_t g_next_consumer_id = 1;

static int consumer_rotate_source(uint32_t previous, uint32_t current, void *user) {
    (void)previous;
    rtc_consumer_t *consumer = user;
    uint16_t seq;
    if (rtc_random_bytes((uint8_t *)&seq, sizeof(seq)) != RTC_OK)
        return RTC_ERR_GENERIC;
    if (seq < RTC_CONSUMER_HISTORY_SIZE)
        seq += RTC_CONSUMER_HISTORY_SIZE;
    rtc_mutex_lock(&consumer->media_mutex);
    if (atomic_load(&consumer->closed) || atomic_load(&consumer->source_ended)) {
        rtc_mutex_unlock(&consumer->media_mutex);
        return RTC_OK;
    }
    rtc_rtcp_stats_restart_sender(&consumer->rtcp, current);
    consumer->rtp.ssrc = current;
    consumer->source_generation++;
    consumer->initial_seq = seq;
    consumer->source_initialized = false;
    memset(consumer->history, 0, sizeof(consumer->history));
    atomic_store(&consumer->packets_sent, 0);
    atomic_store(&consumer->bytes_sent, 0);
    atomic_store(&consumer->packets_dropped, 0);
    atomic_store(&consumer->packets_retransmitted, 0);
    rtc_mutex_unlock(&consumer->media_mutex);
    (void)rtc_producer_request_keyframe(consumer->producer);
    return RTC_OK;
}

static void consumer_report_timer(void *user) {
    rtc_consumer_t *consumer = user;
    rtc_mutex_lock(&consumer->media_mutex);
    consumer->report_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load(&consumer->closed) || atomic_load(&consumer->source_ended)) {
        rtc_mutex_unlock(&consumer->media_mutex);
        return;
    }
    int rc = rtc_transport_send_report(consumer->transport, &consumer->rtcp, true);
    uint64_t next = consumer->rtcp.next_report_time;
    if (rc != RTC_OK && rc != RTC_ERR_TIMEOUT)
        RTC_LOG_WARN("Consumer sender report failed: %d", rc);
    uint64_t now = rtc_time_ms();
    if ((rc == RTC_OK || rc == RTC_ERR_TIMEOUT) &&
        (next <= now || next > now + RTC_RTCP_RECONSIDER_POLL_MS))
        next = now + RTC_RTCP_RECONSIDER_POLL_MS;
    consumer->report_timer = rtc_worker_add_timer(consumer->worker, next > now ? next : now + 1000,
                                                  consumer_report_timer, consumer);
    if (consumer->report_timer == RTC_WORKER_TIMER_INVALID)
        RTC_LOG_ERR("Consumer report timer allocation failed");
    rtc_mutex_unlock(&consumer->media_mutex);
}

static void consumer_retire_on_worker(void *user) {
    rtc_consumer_t *consumer = user;
    rtc_mutex_lock(&consumer->media_mutex);
    consumer_sync_sent(consumer);
    if (consumer->report_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(consumer->worker, consumer->report_timer);
        consumer->report_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (!atomic_load(&consumer->source_ended) && atomic_load(&consumer->packets_sent) &&
        rtc_transport_send_bye_named(consumer->transport, consumer->rtp.ssrc,
                                     consumer->rtp.rtcp_cname) != RTC_OK)
        RTC_LOG_DBG("RTCP BYE unavailable on retiring consumer");
    rtc_transport_discard_rtp(consumer->transport, consumer->rtp.ssrc);
    rtc_mutex_unlock(&consumer->media_mutex);
}

rtc_consumer_t *rtc_transport_consume(rtc_transport_t *transport,
                                      const rtc_consumer_options_t *opts) {
    if (!transport || !opts || !opts->producer)
        return NULL;

    rtc_transport_stats_t transport_stats;
    rtc_producer_stats_t producer_stats;
    if (rtc_transport_get_stats(transport, &transport_stats) != RTC_OK || transport_stats.closed)
        return NULL;
    if (rtc_producer_get_stats(opts->producer, &producer_stats) != RTC_OK ||
        producer_stats.closed || producer_stats.source_ended)
        return NULL;

    rtc_consumer_t *consumer = (rtc_consumer_t *)calloc(1, sizeof(*consumer));
    if (!consumer)
        return NULL;

    uint32_t id = atomic_fetch_add_explicit(&g_next_consumer_id, 1, memory_order_relaxed);
    snprintf(consumer->id, sizeof(consumer->id), "cons-%u", (unsigned)id);
    consumer->transport = transport;
    consumer->worker = rtc_transport_worker(transport);
    consumer->report_timer = RTC_WORKER_TIMER_INVALID;
    consumer->producer = opts->producer;
    consumer->kind = producer_stats.kind;
    atomic_init(&consumer->allocated_bitrate_bps, UINT32_MAX);
    if (rtc_producer_get_rtp_parameters(opts->producer, &consumer->rtp) != RTC_OK) {
        free(consumer);
        return NULL;
    }
    if (rtc_transport_new_ssrc(transport, &consumer->rtp.ssrc) != RTC_OK) {
        free(consumer);
        return NULL;
    }
    consumer->source_generation = 1;
    if (consumer->rtp.mid[0] == '\0') {
        size_t len = strlen(consumer->id);
        if (len >= sizeof(consumer->rtp.mid))
            len = sizeof(consumer->rtp.mid) - 1;
        memcpy(consumer->rtp.mid, consumer->id, len);
        consumer->rtp.mid[len] = '\0';
    }
    if (rtc_random_bytes((uint8_t *)&consumer->initial_seq, sizeof(consumer->initial_seq)) !=
            RTC_OK ||
        rtc_mutex_init(&consumer->media_mutex) != RTC_OK) {
        free(consumer);
        return NULL;
    }
    /* Leave room for first-frame reordering without mapping an older packet
     * into a nonexistent negative SRTP rollover epoch. */
    if (consumer->initial_seq < RTC_CONSUMER_HISTORY_SIZE)
        consumer->initial_seq += RTC_CONSUMER_HISTORY_SIZE;
    consumer->app_data = opts->app_data;
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (consumer->kind == RTC_MEDIA_KIND_VIDEO) {
        rtc_rate_control_config_t rate = {
            .target_bitrate_kbps = 500, .min_bitrate_kbps = 100, .max_bitrate_kbps = 2500};
        consumer->rate_controller = rtc_rate_control_create(&rate);
        if (!consumer->rate_controller) {
            rtc_mutex_destroy(&consumer->media_mutex);
            free(consumer);
            return NULL;
        }
    }
#endif
    rtc_rtcp_stats_init(&consumer->rtcp, consumer->rtp.ssrc);
    consumer->rtcp.send_clock_rate = consumer->rtp.codecs[0].clock_rate;
    snprintf(consumer->rtcp.cname, sizeof(consumer->rtcp.cname), "%s", consumer->rtp.rtcp_cname);
    atomic_store_explicit(&consumer->paused, opts->paused, memory_order_release);
    if (rtc_transport_register_source(transport, consumer->rtp.ssrc, consumer_rotate_source,
                                      consumer, consumer->rtp.rtcp_cname) != RTC_OK) {
        rtc_rate_control_destroy(consumer->rate_controller);
        rtc_mutex_destroy(&consumer->media_mutex);
        free(consumer);
        return NULL;
    }
    if (rtc_producer_add_consumer(opts->producer, consumer) != RTC_OK) {
        rtc_transport_unregister_source(transport, consumer);
        rtc_rate_control_destroy(consumer->rate_controller);
        rtc_mutex_destroy(&consumer->media_mutex);
        free(consumer);
        return NULL;
    }
    rtc_transport_retain(transport);
    return consumer;
}

const char *rtc_consumer_id(const rtc_consumer_t *consumer) {
    return consumer ? consumer->id : "";
}

int rtc_consumer_get_rtp_parameters(rtc_consumer_t *consumer, rtc_rtp_parameters_t *out) {
    if (!consumer || !out)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&consumer->media_mutex);
    *out = consumer->rtp;
    rtc_mutex_unlock(&consumer->media_mutex);
    return RTC_OK;
}

int rtc_consumer_set_mid(rtc_consumer_t *consumer, uint8_t ext_id, const char *mid) {
    if (!consumer || ext_id > 14 ||
        (ext_id &&
         (!mid || !mid[0] || strlen(mid) > 16 || strlen(mid) >= sizeof(consumer->rtp.mid))))
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&consumer->media_mutex);
    if (consumer->mid_ext_id == ext_id && (!ext_id || strcmp(consumer->rtp.mid, mid) == 0)) {
        rtc_mutex_unlock(&consumer->media_mutex);
        return RTC_OK;
    }
    if (!atomic_load(&consumer->paused) && consumer->source_initialized) {
        rtc_mutex_unlock(&consumer->media_mutex);
        return RTC_ERR_INVALID;
    }
    consumer->mid_ext_id = ext_id;
    if (ext_id)
        memcpy(consumer->rtp.mid, mid, strlen(mid) + 1);
    rtc_mutex_unlock(&consumer->media_mutex);
    return RTC_OK;
}

void rtc_consumer_pause(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    atomic_store_explicit(&consumer->paused, true, memory_order_release);
    rtc_mutex_lock(&consumer->media_mutex);
    consumer_sync_sent(consumer);
    rtc_transport_discard_rtp(consumer->transport, consumer->rtp.ssrc);
    memset(consumer->history, 0, sizeof(consumer->history));
    rtc_mutex_unlock(&consumer->media_mutex);
    (void)rtc_producer_update_bitrate(consumer->producer);
}

void rtc_consumer_resume(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    rtc_mutex_lock(&consumer->media_mutex);
    if (atomic_load(&consumer->closed) || atomic_load(&consumer->source_ended)) {
        rtc_mutex_unlock(&consumer->media_mutex);
        RTC_LOG_WARN("Cannot resume a closed or retired consumer source");
        return;
    }
    atomic_store_explicit(&consumer->paused, false, memory_order_release);
    atomic_store(&consumer->bitrate_feedback_ms, 0);
    rtc_mutex_unlock(&consumer->media_mutex);
    rtc_producer_request_keyframe(consumer->producer);
}

void rtc_consumer_set_source_clock(rtc_consumer_t *consumer, const rtc_rtcp_sr_t *sr,
                                   uint64_t now_ms) {
    if (!consumer || !sr || atomic_load(&consumer->closed) || atomic_load(&consumer->source_ended))
        return;
    rtc_mutex_lock(&consumer->media_mutex);
    if (consumer->rtcp.send_clock_rate) {
        consumer->rtcp.source_clock = *sr;
        consumer->rtcp.source_clock_ms = now_ms;
        consumer->rtcp.source_clock_valid = true;
    } else {
        RTC_LOG_WARN("Cannot preserve publisher clock without a codec clock rate");
    }
    rtc_mutex_unlock(&consumer->media_mutex);
}

void rtc_consumer_source_ended(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    rtc_mutex_lock(&consumer->media_mutex);
    if (!atomic_load(&consumer->closed) && !atomic_exchange(&consumer->source_ended, true)) {
        consumer_sync_sent(consumer);
        atomic_store(&consumer->paused, true);
        atomic_store(&consumer->bitrate_feedback_ms, 0);
        memset(consumer->history, 0, sizeof(consumer->history));
        rtc_transport_discard_rtp(consumer->transport, consumer->rtp.ssrc);
        if (atomic_load(&consumer->packets_sent)) {
            int rc = rtc_transport_send_bye_named(consumer->transport, consumer->rtp.ssrc,
                                                  consumer->rtp.rtcp_cname);
            if (rc != RTC_OK)
                RTC_LOG_DBG("RTCP BYE unavailable on ended consumer: %d", rc);
        }
    }
    rtc_mutex_unlock(&consumer->media_mutex);
}

void rtc_consumer_close(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    bool was_closed = atomic_exchange_explicit(&consumer->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    if (consumer->producer)
        rtc_producer_remove_consumer(consumer->producer, consumer);
    rtc_worker_invoke(consumer->worker, consumer_retire_on_worker, consumer);
    rtc_transport_unregister_source(consumer->transport, consumer);
}

void rtc_consumer_destroy(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    rtc_consumer_close(consumer);
    rtc_transport_forget_rtp(consumer->transport, consumer->rtp.ssrc);
    rtc_transport_release(consumer->transport);
    rtc_rate_control_destroy(consumer->rate_controller);
    rtc_mutex_destroy(&consumer->media_mutex);
    free(consumer);
}

int rtc_consumer_get_stats(rtc_consumer_t *consumer, rtc_consumer_stats_t *out) {
    if (!consumer || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    rtc_mutex_lock(&consumer->media_mutex);
    out->ssrc = consumer->rtp.ssrc;
    out->source_generation = consumer->source_generation;
    out->closed = atomic_load_explicit(&consumer->closed, memory_order_acquire);
    out->source_ended = atomic_load(&consumer->source_ended);
    out->paused =
        out->source_ended || atomic_load_explicit(&consumer->paused, memory_order_acquire);
    out->kind = consumer->kind;
    out->packets_sent = atomic_load_explicit(&consumer->packets_sent, memory_order_relaxed);
    out->bytes_sent = atomic_load_explicit(&consumer->bytes_sent, memory_order_relaxed);
    out->packets_dropped = atomic_load_explicit(&consumer->packets_dropped, memory_order_relaxed);
    out->packets_retransmitted =
        atomic_load_explicit(&consumer->packets_retransmitted, memory_order_relaxed);
    out->estimated_bitrate_bps = atomic_load(&consumer->estimated_bitrate_bps);
    out->bitrate_feedback_ms = atomic_load(&consumer->bitrate_feedback_ms);
    out->last_packet_ms = atomic_load(&consumer->last_packet_ms);
    out->allocated_bitrate_bps = atomic_load(&consumer->allocated_bitrate_bps);
    out->allocation_excluded = atomic_load(&consumer->allocation_excluded);
    rtc_pacer_sent_t snapshot;
    if (rtc_transport_rtp_sent(consumer->transport, consumer->rtp.ssrc, &snapshot)) {
        out->packets_sent = snapshot.packets_sent;
        out->bytes_sent = snapshot.octets_sent;
        out->packets_dropped += snapshot.packets_dropped - snapshot.packets_rejected;
        out->packets_queued = snapshot.packets_queued;
    }
    rtc_mutex_unlock(&consumer->media_mutex);
    return RTC_OK;
}

int rtc_consumer_set_bitrate_allocation(rtc_consumer_t *consumer, uint32_t bitrate_bps) {
    if (!consumer || consumer->kind != RTC_MEDIA_KIND_VIDEO || atomic_load(&consumer->closed))
        return RTC_ERR_INVALID;
    bool excluded = atomic_exchange(&consumer->allocation_excluded, false);
    uint32_t previous = atomic_exchange(&consumer->allocated_bitrate_bps, bitrate_bps);
    if (previous == bitrate_bps && !excluded)
        return RTC_OK;
    if (bitrate_bps == 0) {
        rtc_mutex_lock(&consumer->media_mutex);
        consumer_sync_sent(consumer);
        rtc_transport_discard_rtp(consumer->transport, consumer->rtp.ssrc);
        memset(consumer->history, 0, sizeof(consumer->history));
        rtc_mutex_unlock(&consumer->media_mutex);
    } else if (previous == 0) {
        rtc_producer_request_keyframe(consumer->producer);
    }
    return rtc_producer_update_bitrate(consumer->producer);
}

int rtc_consumer_allocate_video(rtc_consumer_t *const *consumers, size_t count, uint32_t budget_bps,
                                uint64_t now_ms) {
    if ((!consumers && count) || count > RTC_CONSUMER_MAX_ALLOCATION_LEGS)
        return RTC_ERR_INVALID;
    bool active[RTC_CONSUMER_MAX_ALLOCATION_LEGS] = {false};
    bool changed[RTC_CONSUMER_MAX_ALLOCATION_LEGS] = {false};
    uint64_t pool = 0;
    unsigned legs = 0;
    for (size_t i = 0; i < count; i++) {
        rtc_consumer_t *c = consumers[i];
        if (!c || c->kind != RTC_MEDIA_KIND_VIDEO || atomic_load(&c->closed) ||
            atomic_load(&c->source_ended) || atomic_load(&c->paused))
            continue;
        uint64_t last = atomic_load(&c->last_packet_ms);
        if (last && now_ms >= last && now_ms - last >= 10000)
            continue;
        active[i] = true;
        legs++;
        uint64_t feedback = atomic_load(&c->bitrate_feedback_ms);
        uint32_t estimate = feedback && now_ms >= feedback && now_ms - feedback < 10000
                                ? atomic_load(&c->estimated_bitrate_bps)
                                : 500000;
        pool += estimate < 2500000 ? estimate : 2500000;
    }
    if (budget_bps != UINT32_MAX)
        pool = budget_bps;
    uint32_t share = legs ? (uint32_t)(pool / legs) : 0;
    if (share > 2500000)
        share = 2500000;
    for (size_t i = 0; i < count; i++) {
        rtc_consumer_t *c = consumers[i];
        if (!c || c->kind != RTC_MEDIA_KIND_VIDEO || atomic_load(&c->closed))
            continue;
        uint32_t allocation = active[i] ? share : UINT32_MAX;
        uint32_t previous = atomic_exchange(&c->allocated_bitrate_bps, allocation);
        bool excluded = atomic_exchange(&c->allocation_excluded, !active[i]);
        changed[i] = previous != allocation || excluded != !active[i];
        if (!changed[i])
            continue;
        if (allocation == 0) {
            rtc_mutex_lock(&c->media_mutex);
            consumer_sync_sent(c);
            rtc_transport_discard_rtp(c->transport, c->rtp.ssrc);
            memset(c->history, 0, sizeof(c->history));
            rtc_mutex_unlock(&c->media_mutex);
        } else if (previous == 0 && active[i]) {
            rtc_producer_request_keyframe(c->producer);
        }
    }
    int result = RTC_OK;
    for (size_t i = 0; i < count; i++)
        if (changed[i]) {
            int rc = rtc_producer_update_bitrate(consumers[i]->producer);
            if (rc != RTC_OK)
                result = rc;
        }
    return result;
}

uint32_t rtc_consumer_bitrate_limit(rtc_consumer_t *consumer, uint64_t now_ms) {
    if (!consumer || atomic_load(&consumer->closed) || atomic_load(&consumer->source_ended) ||
        atomic_load(&consumer->paused) || atomic_load(&consumer->allocation_excluded) ||
        consumer->kind != RTC_MEDIA_KIND_VIDEO)
        return UINT32_MAX;
    uint32_t allocation = atomic_load(&consumer->allocated_bitrate_bps);
    if (allocation == 0)
        return UINT32_MAX;
    if (allocation != UINT32_MAX)
        return allocation;
    if (!atomic_load(&consumer->bitrate_feedback_ms) ||
        now_ms - atomic_load(&consumer->bitrate_feedback_ms) > 10000)
        return UINT32_MAX;
    return atomic_load(&consumer->estimated_bitrate_bps);
}

void rtc_consumer_on_producer_rtp(rtc_consumer_t *consumer, const rtc_rtp_packet_t *pkt) {
    if (!consumer || !pkt)
        return;
    atomic_store(&consumer->last_packet_ms, rtc_time_ms());
    if (consumer->kind == RTC_MEDIA_KIND_VIDEO &&
        atomic_load(&consumer->allocated_bitrate_bps) == 0) {
        atomic_fetch_add(&consumer->packets_dropped, 1);
        return;
    }
    if (atomic_load_explicit(&consumer->closed, memory_order_acquire) ||
        atomic_load_explicit(&consumer->paused, memory_order_acquire) ||
        atomic_load(&consumer->source_ended)) {
        return;
    }

    rtc_mutex_lock(&consumer->media_mutex);
    if (atomic_load(&consumer->closed) || atomic_load(&consumer->source_ended) ||
        atomic_load(&consumer->paused)) {
        rtc_mutex_unlock(&consumer->media_mutex);
        return;
    }
    uint16_t source_seq = pkt->header.seq;
    if (!consumer->source_initialized) {
        consumer->seq_offset = (uint16_t)(consumer->initial_seq - source_seq);
        consumer->highest_source_seq = source_seq;
        consumer->source_initialized = true;
    } else {
        int16_t delta = (int16_t)(source_seq - consumer->highest_source_seq);
        if (delta <= -RTC_CONSUMER_HISTORY_SIZE) {
            atomic_fetch_add(&consumer->packets_dropped, 1);
            rtc_mutex_unlock(&consumer->media_mutex);
            return;
        }
        if (delta > 0) {
            consumer->highest_source_seq = source_seq;
            if (delta >= RTC_CONSUMER_HISTORY_SIZE)
                memset(consumer->history, 0, sizeof(consumer->history));
        }
    }
    rtc_rtp_packet_t out_pkt;
    uint16_t seq = (uint16_t)(source_seq + consumer->seq_offset);
    size_t slot = seq % RTC_CONSUMER_HISTORY_SIZE;
    if (consumer->history[slot].used && consumer->history[slot].seq == seq) {
        atomic_fetch_add(&consumer->packets_dropped, 1);
        rtc_mutex_unlock(&consumer->media_mutex);
        return;
    }
    /* A failed send may already have consumed its SRTP nonce. Never protect
     * the same source position twice, even if it has no successful cache entry. */
    consumer->history[slot].used = true;
    consumer->history[slot].seq = seq;
    consumer->history[slot].len = 0;
    consumer->history[slot].attempts = 0;
    rtc_rtp_ext_t ext;
    if (consumer->mid_ext_id)
        rtc_rtp_ext_make_string(&ext, consumer->mid_ext_id, consumer->rtp.mid);
    int rc = rtc_rtp_build_with_ext(&out_pkt, pkt->header.payload_type, seq, pkt->header.timestamp,
                                    consumer->rtp.ssrc, pkt->header.marker, &ext,
                                    consumer->mid_ext_id ? 1 : 0, pkt->payload, pkt->payload_len);
    if (rc != RTC_OK) {
        atomic_fetch_add_explicit(&consumer->packets_dropped, 1, memory_order_relaxed);
        rtc_mutex_unlock(&consumer->media_mutex);
        return;
    }

    size_t wire_len = out_pkt.buf_len;
    uint32_t allocation = atomic_load(&consumer->allocated_bitrate_bps);
    rc = rtc_transport_send_rtp_media(consumer->transport, out_pkt.buf, &wire_len,
                                      sizeof(out_pkt.buf), consumer->kind == RTC_MEDIA_KIND_VIDEO,
                                      allocation != UINT32_MAX ? allocation : 500000);
    if (rc != RTC_OK) {
        atomic_fetch_add_explicit(&consumer->packets_dropped, 1, memory_order_relaxed);
        rtc_mutex_unlock(&consumer->media_mutex);
        return;
    }
    consumer->history[slot].len = wire_len;
    memcpy(consumer->history[slot].data, out_pkt.buf, wire_len);

    atomic_fetch_add_explicit(&consumer->packets_sent, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&consumer->bytes_sent, (uint64_t)pkt->payload_len,
                              memory_order_relaxed);
    rtc_rtcp_stats_on_rtp_send(&consumer->rtcp, pkt->header.timestamp, pkt->payload_len);
    consumer_sync_sent(consumer);
    if (consumer->report_timer == RTC_WORKER_TIMER_INVALID) {
        consumer->report_timer =
            rtc_worker_add_timer(consumer->worker, rtc_time_ms(), consumer_report_timer, consumer);
        if (consumer->report_timer == RTC_WORKER_TIMER_INVALID)
            RTC_LOG_ERR("Consumer report timer allocation failed");
    }
    rtc_mutex_unlock(&consumer->media_mutex);
}

int rtc_consumer_handle_rtcp(rtc_consumer_t *consumer, const uint8_t *data, size_t len) {
    if (!consumer || !data || atomic_load_explicit(&consumer->closed, memory_order_acquire))
        return RTC_ERR_INVALID;
    if (rtc_rtcp_validate_compound(data, len) != RTC_OK)
        return RTC_ERR_INVALID;
    if (atomic_load(&consumer->source_ended))
        return RTC_OK;
    size_t offset = 0;
    while (offset + 4 <= len) {
        const uint8_t *packet = data + offset;
        size_t size = (((size_t)packet[2] << 8) | packet[3]) * 4 + 4;
        uint8_t pt, fmt;
        if (size > len - offset || !rtc_rtcp_get_pt_fmt(packet, size, &pt, &fmt))
            return RTC_ERR_INVALID;
        if ((pt == RTCP_PT_RR || pt == RTCP_PT_SR) && consumer->rate_controller) {
            rtc_rtcp_packet_t report;
            if (rtc_rtcp_parse(&report, packet, size) != RTC_OK)
                return RTC_ERR_INVALID;
            bool updated = false;
            for (int i = 0; i < report.report_count; i++) {
                if (report.reports[i].ssrc != consumer->rtp.ssrc)
                    continue;
                rtc_rate_control_on_rtcp_rr(
                    consumer->rate_controller, report.reports[i].fraction_lost,
                    rtc_rtcp_stats_rtt_from_rr(&consumer->rtcp, &report.reports[i], rtc_time_ms()),
                    (int)report.reports[i].jitter);
                atomic_store(&consumer->estimated_bitrate_bps,
                             (uint32_t)rtc_rate_control_get_bitrate(consumer->rate_controller) *
                                 1000);
                atomic_store(&consumer->bitrate_feedback_ms, rtc_time_ms());
                updated = true;
            }
            if (updated) {
                int rc = rtc_producer_update_bitrate(consumer->producer);
                if (rc != RTC_OK)
                    return rc;
            }
        } else if (pt == RTCP_PT_PSFB && fmt == 15 && consumer->kind == RTC_MEDIA_KIND_VIDEO) {
            rtc_rtcp_remb_t feedback;
            if (rtc_rtcp_parse_remb(&feedback, packet, size) == RTC_OK &&
                feedback.media_ssrc == consumer->rtp.ssrc) {
                atomic_store(&consumer->estimated_bitrate_bps, feedback.bitrate_bps);
                atomic_store(&consumer->bitrate_feedback_ms, rtc_time_ms());
                int rc = rtc_producer_update_bitrate(consumer->producer);
                if (rc != RTC_OK)
                    return rc;
            }
        } else if (pt == RTCP_PT_RTPFB && fmt == RTCP_FMT_NACK) {
            rtc_rtcp_nack_t nack;
            if (rtc_rtcp_parse_nack(&nack, packet, size) != RTC_OK)
                return RTC_ERR_INVALID;
            if (nack.media_ssrc == consumer->rtp.ssrc) {
                for (int i = 0; i < nack.lost_count; i++) {
                    rtc_mutex_lock(&consumer->media_mutex);
                    size_t slot = nack.lost_seqs[i] % RTC_CONSUMER_HISTORY_SIZE;
                    uint16_t highest =
                        (uint16_t)(consumer->highest_source_seq + consumer->seq_offset);
                    if (!consumer->history[slot].len ||
                        consumer->history[slot].seq != nack.lost_seqs[i] ||
                        (uint16_t)(highest - nack.lost_seqs[i]) >= RTC_CONSUMER_HISTORY_SIZE) {
                        rtc_mutex_unlock(&consumer->media_mutex);
                        int rc = rtc_producer_request_keyframe(consumer->producer);
                        if (rc != RTC_OK)
                            return rc;
                        continue;
                    }
                    if (consumer->history[slot].attempts >= RTC_CONSUMER_MAX_RETRANSMITS) {
                        rtc_mutex_unlock(&consumer->media_mutex);
                        int rc = rtc_producer_request_keyframe(consumer->producer);
                        if (rc != RTC_OK)
                            return rc;
                        continue;
                    }
                    consumer->history[slot].attempts++;
                    int rc = rtc_transport_retransmit_rtp(consumer->transport,
                                                          consumer->history[slot].data,
                                                          consumer->history[slot].len);
                    if (rc != RTC_OK) {
                        atomic_fetch_add(&consumer->packets_dropped, 1);
                        rtc_mutex_unlock(&consumer->media_mutex);
                        return rc;
                    }
                    atomic_fetch_add(&consumer->packets_retransmitted, 1);
                    rtc_mutex_unlock(&consumer->media_mutex);
                }
            }
        } else if (pt == RTCP_PT_PSFB && fmt == RTCP_FMT_PLI) {
            rtc_rtcp_pli_t pli;
            if (rtc_rtcp_parse_pli(&pli, packet, size) != RTC_OK)
                return RTC_ERR_INVALID;
            if (pli.media_ssrc == consumer->rtp.ssrc)
                return rtc_producer_request_keyframe(consumer->producer);
        } else if (pt == RTCP_PT_PSFB && fmt == RTCP_FMT_FIR) {
            rtc_rtcp_fir_t fir;
            if (rtc_rtcp_parse_fir(&fir, packet, size) != RTC_OK)
                return RTC_ERR_INVALID;
            if (fir.media_ssrc == consumer->rtp.ssrc)
                return rtc_producer_request_keyframe(consumer->producer);
        }
        offset += size;
    }
    return offset == len ? RTC_OK : RTC_ERR_INVALID;
}
