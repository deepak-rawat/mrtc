/*
 * SFU outbound media consumer.
 */
#include "rtc/rtc_consumer.h"

#include "rtc_consumer_internal.h"
#include "rtc_producer_internal.h"
#include "rtc/rtc_rtcp.h"
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
    rtc_worker_t *worker;
    rtc_worker_timer_t report_timer;
};

static _Atomic uint32_t g_next_consumer_id = 1;

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
    consumer->report_timer = rtc_worker_add_timer(consumer->worker, next > now ? next : now + 1000,
                                                  consumer_report_timer, consumer);
    if (consumer->report_timer == RTC_WORKER_TIMER_INVALID)
        RTC_LOG_ERR("Consumer report timer allocation failed");
    rtc_mutex_unlock(&consumer->media_mutex);
}

static void consumer_retire_on_worker(void *user) {
    rtc_consumer_t *consumer = user;
    if (consumer->report_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(consumer->worker, consumer->report_timer);
        consumer->report_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (!atomic_load(&consumer->source_ended) && atomic_load(&consumer->packets_sent) &&
        rtc_transport_send_bye(consumer->transport, consumer->rtp.ssrc) != RTC_OK)
        RTC_LOG_DBG("RTCP BYE unavailable on retiring consumer");
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
    atomic_store_explicit(&consumer->paused, opts->paused, memory_order_release);
    if (rtc_producer_add_consumer(opts->producer, consumer) != RTC_OK) {
        rtc_rate_control_destroy(consumer->rate_controller);
        rtc_mutex_destroy(&consumer->media_mutex);
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

void rtc_consumer_source_ended(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    rtc_mutex_lock(&consumer->media_mutex);
    if (!atomic_load(&consumer->closed) && !atomic_exchange(&consumer->source_ended, true)) {
        atomic_store(&consumer->paused, true);
        atomic_store(&consumer->bitrate_feedback_ms, 0);
        memset(consumer->history, 0, sizeof(consumer->history));
        if (atomic_load(&consumer->packets_sent)) {
            int rc = rtc_transport_send_bye(consumer->transport, consumer->rtp.ssrc);
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
}

void rtc_consumer_destroy(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    rtc_consumer_close(consumer);
    rtc_rate_control_destroy(consumer->rate_controller);
    rtc_mutex_destroy(&consumer->media_mutex);
    free(consumer);
}

int rtc_consumer_get_stats(rtc_consumer_t *consumer, rtc_consumer_stats_t *out) {
    if (!consumer || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
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
    return RTC_OK;
}

uint32_t rtc_consumer_bitrate_limit(rtc_consumer_t *consumer, uint64_t now_ms) {
    if (!consumer || atomic_load(&consumer->closed) || atomic_load(&consumer->paused) ||
        consumer->kind != RTC_MEDIA_KIND_VIDEO || !atomic_load(&consumer->bitrate_feedback_ms) ||
        now_ms - atomic_load(&consumer->bitrate_feedback_ms) > 10000)
        return UINT32_MAX;
    return atomic_load(&consumer->estimated_bitrate_bps);
}

void rtc_consumer_on_producer_rtp(rtc_consumer_t *consumer, const rtc_rtp_packet_t *pkt) {
    if (!consumer || !pkt)
        return;
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
    int rc = rtc_rtp_build(&out_pkt, pkt->header.payload_type, seq, pkt->header.timestamp,
                           consumer->rtp.ssrc, pkt->header.marker, pkt->payload, pkt->payload_len);
    if (rc != RTC_OK) {
        atomic_fetch_add_explicit(&consumer->packets_dropped, 1, memory_order_relaxed);
        rtc_mutex_unlock(&consumer->media_mutex);
        return;
    }

    size_t wire_len = out_pkt.buf_len;
    rc = rtc_transport_send_rtp(consumer->transport, out_pkt.buf, &wire_len, sizeof(out_pkt.buf));
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
                rtc_rate_control_on_rtcp_rr(consumer->rate_controller,
                                            report.reports[i].fraction_lost,
                                            rtc_rtcp_rtt_from_rr(&report.reports[i]),
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
