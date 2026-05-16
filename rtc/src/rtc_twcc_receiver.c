/*
 * Transport-Wide CC — Receiver side.
 *
 * Uses run-length status chunks where possible to keep feedback packets small.
 * Receive deltas are emitted in 64µs ticks; values that fit in [-128..127]
 * use small (1-byte) deltas, others use 2-byte signed deltas. Values outside
 * the 16-bit signed range cause the packet to be marked not-received.
 */
#include "rtc_twcc_receiver.h"
#include "rtc_rtcp.h"

#include <string.h>

#define RTC_TWCC_PT      RTCP_PT_RTPFB
#define RTC_TWCC_FMT     15
#define RTC_TWCC_TICK_US 250 /* reference time / send delta resolution */

/* Per-packet status codes (draft §3.1.1) */
#define STATUS_NOT_RECV    0
#define STATUS_SMALL_DELTA 1
#define STATUS_LARGE_DELTA 2

void rtc_twcc_receiver_init(rtc_twcc_receiver_t *r) {
    memset(r, 0, sizeof(*r));
}

/* Distance in 16-bit seq space (b - a) treated as signed-ish: returns the
 * smallest non-negative representative if b is "after" a, otherwise -1. */
static int seq_after(uint16_t a, uint16_t b) {
    return (int)(int16_t)(b - a);
}

void rtc_twcc_receiver_on_packet(rtc_twcc_receiver_t *r, uint16_t twcc_seq, uint64_t recv_time_us) {
    if (!r->have_base) {
        r->base_seq = twcc_seq;
        r->count = 1;
        r->window[0].seq = twcc_seq;
        r->window[0].recv_time_us = recv_time_us;
        r->window[0].received = true;
        r->have_base = true;
        return;
    }
    int delta = seq_after(r->base_seq, twcc_seq);
    if (delta < 0)
        return; /* out-of-order before base — drop */
    if (delta >= RTC_TWCC_RECV_WINDOW)
        return; /* window overflow — drop until next feedback */
    if (delta + 1 > r->count)
        r->count = delta + 1;
    rtc_twcc_recv_entry_t *e = &r->window[delta];
    if (e->received)
        return; /* duplicate */
    e->seq = twcc_seq;
    e->recv_time_us = recv_time_us;
    e->received = true;
}

static void w16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}
static void w24(uint8_t *p, uint32_t v) {
    p[0] = (v >> 16) & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = v & 0xFF;
}
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

int rtc_twcc_receiver_build_feedback(rtc_twcc_receiver_t *r, uint32_t sender_ssrc,
                                     uint32_t media_ssrc, uint8_t *out, size_t cap,
                                     size_t *out_len) {
    if (!r->have_base || r->count == 0)
        return RTC_ERR_INVALID;
    if (cap < 32)
        return RTC_ERR_INVALID;

    /* Find first received entry to use as reference time origin. */
    int first_recv = -1;
    for (int i = 0; i < r->count; i++) {
        if (r->window[i].received) {
            first_recv = i;
            break;
        }
    }
    if (first_recv < 0) {
        /* Nothing received at all — just reset, no feedback */
        rtc_twcc_receiver_init(r);
        return RTC_ERR_INVALID;
    }

    uint64_t ref_us = r->window[first_recv].recv_time_us;
    /* Reference time is in 64ms units (24-bit). Quantize down. */
    uint32_t ref_time_64ms = (uint32_t)((ref_us / 64000ULL) & 0x00FFFFFFu);
    uint64_t ref_base_us = (uint64_t)ref_time_64ms * 64000ULL;

    /* Compute per-packet 250µs deltas relative to running "previous" time
     * (initialized at ref_base_us). */
    int16_t deltas[RTC_TWCC_RECV_WINDOW];
    uint8_t status[RTC_TWCC_RECV_WINDOW];
    uint64_t prev_us = ref_base_us;
    for (int i = 0; i < r->count; i++) {
        rtc_twcc_recv_entry_t *e = &r->window[i];
        if (!e->received) {
            status[i] = STATUS_NOT_RECV;
            deltas[i] = 0;
            continue;
        }
        int64_t d_ticks = ((int64_t)e->recv_time_us - (int64_t)prev_us) / (int64_t)RTC_TWCC_TICK_US;
        if (d_ticks >= -128 && d_ticks <= 127) {
            status[i] = STATUS_SMALL_DELTA;
            deltas[i] = (int16_t)d_ticks;
        } else if (d_ticks >= INT16_MIN && d_ticks <= INT16_MAX) {
            status[i] = STATUS_LARGE_DELTA;
            deltas[i] = (int16_t)d_ticks;
        } else {
            /* Outside representable range — mark as not received. */
            status[i] = STATUS_NOT_RECV;
            deltas[i] = 0;
        }
        if (status[i] != STATUS_NOT_RECV)
            prev_us = (uint64_t)((int64_t)prev_us + (int64_t)deltas[i] * RTC_TWCC_TICK_US);
    }

    /* ---- Build the packet ---- */
    /* Header: V=2, P=0, FMT=15, PT=205, length, sender_ssrc, media_ssrc,
     * base_seq(16), pkt_count(16), ref_time(24), fb_pkt_count(8) */
    if (cap < 20)
        return RTC_ERR_INVALID;
    size_t p = 0;
    out[p++] = 0x80 | RTC_TWCC_FMT; /* V=2, P=0, FMT=15 */
    out[p++] = RTC_TWCC_PT;         /* 205 */
    out[p++] = 0;
    out[p++] = 0; /* length placeholder */
    w32(&out[p], sender_ssrc);
    p += 4;
    w32(&out[p], media_ssrc);
    p += 4;
    w16(&out[p], r->base_seq);
    p += 2;
    w16(&out[p], (uint16_t)r->count);
    p += 2;
    w24(&out[p], ref_time_64ms);
    p += 3;
    out[p++] = r->fb_pkt_count++;

    /* Status chunks: emit run-length chunks when 3+ consecutive same-status,
     * otherwise pack into 14-status-vector chunks (1 bit per status — only
     * SMALL_DELTA vs NOT_RECV; LARGE_DELTA forces a run-length chunk). */
    int i = 0;
    while (i < r->count) {
        if (cap < p + 2)
            return RTC_ERR_INVALID;
        /* Try a run of identical status. */
        int run = 1;
        while (i + run < r->count && status[i + run] == status[i] && run < 0x1FFF)
            run++;
        if (run >= 3 || status[i] == STATUS_LARGE_DELTA) {
            /* Run-length chunk: T=0 (MSB=0), S (2 bits), run-length (13 bits) */
            uint16_t chunk = ((uint16_t)(status[i] & 0x3) << 13) | (uint16_t)(run & 0x1FFF);
            w16(&out[p], chunk);
            p += 2;
            i += run;
        } else {
            /* Status-vector chunk T=1, S=0 (1-bit symbols, 14 statuses).
             * 1-bit symbols only encode {NOT_RECV=0, SMALL_DELTA=1}; if any
             * LARGE_DELTA appears in the 14-window we'd have entered the
             * run-length branch above. */
            int n = r->count - i;
            if (n > 14)
                n = 14;
            uint16_t chunk = 0x8000; /* T=1, S=0 */
            for (int k = 0; k < n; k++) {
                if (status[i + k] == STATUS_SMALL_DELTA)
                    chunk |= (uint16_t)(1 << (13 - k));
            }
            w16(&out[p], chunk);
            p += 2;
            i += n;
        }
    }

    /* Receive deltas */
    for (int j = 0; j < r->count; j++) {
        if (status[j] == STATUS_SMALL_DELTA) {
            if (p + 1 > cap)
                return RTC_ERR_INVALID;
            out[p++] = (uint8_t)(int8_t)deltas[j];
        } else if (status[j] == STATUS_LARGE_DELTA) {
            if (p + 2 > cap)
                return RTC_ERR_INVALID;
            int16_t d = deltas[j];
            out[p++] = (uint8_t)((d >> 8) & 0xFF);
            out[p++] = (uint8_t)(d & 0xFF);
        }
    }

    /* Pad to 4-byte boundary */
    while (p & 0x3) {
        if (p >= cap)
            return RTC_ERR_INVALID;
        out[p++] = 0;
    }

    /* Length in 32-bit words minus 1 */
    uint16_t length_words = (uint16_t)((p / 4) - 1);
    out[2] = (length_words >> 8) & 0xFF;
    out[3] = length_words & 0xFF;

    *out_len = p;

    /* Reset window — feedback is sent at most once per window. */
    uint8_t saved_count = r->fb_pkt_count;
    rtc_twcc_receiver_init(r);
    r->fb_pkt_count = saved_count;

    return RTC_OK;
}
