/*
 * SFU outbound media consumer.
 */
#include "rtc/rtc_consumer.h"

#include "rtc_consumer_internal.h"
#include "rtc_producer_internal.h"
#include "rtc/rtc_rtcp.h"

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
    rtc_rtcp_stats_init(&consumer->rtcp, consumer->rtp.ssrc);
    atomic_store_explicit(&consumer->paused, opts->paused, memory_order_release);
    if (rtc_producer_add_consumer(opts->producer, consumer) != RTC_OK) {
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
}

void rtc_consumer_resume(rtc_consumer_t *consumer) {
    if (!consumer)
        return;
    atomic_store_explicit(&consumer->paused, false, memory_order_release);
    rtc_producer_request_keyframe(consumer->producer);
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
    rtc_mutex_destroy(&consumer->media_mutex);
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
    out->packets_dropped = atomic_load_explicit(&consumer->packets_dropped, memory_order_relaxed);
    out->packets_retransmitted =
        atomic_load_explicit(&consumer->packets_retransmitted, memory_order_relaxed);
    return RTC_OK;
}

void rtc_consumer_on_producer_rtp(rtc_consumer_t *consumer, const rtc_rtp_packet_t *pkt) {
    if (!consumer || !pkt)
        return;
    if (atomic_load_explicit(&consumer->closed, memory_order_acquire) ||
        atomic_load_explicit(&consumer->paused, memory_order_acquire)) {
        return;
    }

    rtc_mutex_lock(&consumer->media_mutex);
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
    uint64_t now = rtc_time_ms();
    if (now - consumer->rtcp.last_report_time >= 1000) {
        rtc_rtcp_packet_t report;
        if (rtc_rtcp_build_report(&report, &consumer->rtcp, true) == RTC_OK) {
            uint8_t wire[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
            size_t len = report.buf_len;
            memcpy(wire, report.buf, len);
            if (rtc_transport_send_rtcp(consumer->transport, wire, &len, sizeof(wire)) != RTC_OK)
                atomic_fetch_add(&consumer->packets_dropped, 1);
            else
                rtc_rtcp_stats_report_sent(&consumer->rtcp);
        }
    }
    rtc_mutex_unlock(&consumer->media_mutex);
}

int rtc_consumer_handle_rtcp(rtc_consumer_t *consumer, const uint8_t *data, size_t len) {
    if (!consumer || !data || atomic_load_explicit(&consumer->closed, memory_order_acquire))
        return RTC_ERR_INVALID;
    size_t offset = 0;
    while (offset + 4 <= len) {
        const uint8_t *packet = data + offset;
        size_t size = (((size_t)packet[2] << 8) | packet[3]) * 4 + 4;
        uint8_t pt, fmt;
        if (size > len - offset || !rtc_rtcp_get_pt_fmt(packet, size, &pt, &fmt))
            return RTC_ERR_INVALID;
        if (pt == RTCP_PT_RTPFB && fmt == RTCP_FMT_NACK) {
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
