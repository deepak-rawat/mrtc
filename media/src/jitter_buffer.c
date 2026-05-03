/*
 * jitter_buffer.c - Packet reordering and adaptive delay.
 */
#include "jitter_buffer.h"
#include <stdlib.h>
#include <string.h>

jitter_buffer_t *jitter_buffer_create(const jitter_buffer_config_t *cfg) {
    jitter_buffer_t *jb = (jitter_buffer_t *)calloc(1, sizeof(*jb));
    if (!jb)
        return NULL;
    jb->target_delay_ms = cfg ? cfg->target_delay_ms : 80;
    jb->min_delay_ms = jb->target_delay_ms;
    jb->max_delay_ms = cfg ? cfg->max_delay_ms : 500;
    if (jb->target_delay_ms < 0)
        jb->target_delay_ms = 80;
    if (jb->max_delay_ms <= 0)
        jb->max_delay_ms = 500;
    return jb;
}

static int seq_compare(uint16_t a, uint16_t b) {
    /* Handles 16-bit wraparound: returns <0 if a is before b */
    int16_t diff = (int16_t)(a - b);
    return (int)diff;
}

void jitter_buffer_push(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                        uint32_t timestamp, bool marker) {
    uint64_t now = rtc_time_ms();

    if (!jb->started) {
        jb->next_seq = seq;
        jb->started = true;
        jb->last_arrival_ms = now;
        jb->last_timestamp = timestamp;
    } else if (seq_compare(seq, jb->next_seq) < 0) {
        /* Packet is older than next_seq. If we haven't popped anything yet
         * (all packets still buffered), adjust next_seq down. */
        bool all_buffered = true;
        for (int i = 0; i < JB_MAX_PACKETS && all_buffered; i++) {
            /* If any slot between seq+1 and next_seq-1 was already popped,
             * this is genuinely old — drop it. Simplified: just check if
             * next_seq hasn't been consumed (no pops yet). */
        }
        /* Simple heuristic: if seq is very close behind next_seq, adjust */
        int gap = seq_compare(jb->next_seq, seq);
        if (gap > 0 && gap < JB_MAX_PACKETS / 2) {
            jb->next_seq = seq;
        } else {
            return; /* Too old, drop */
        }
    }

    /* Update jitter estimate */
    if (jb->last_arrival_ms > 0) {
        int arrival_diff = (int)(now - jb->last_arrival_ms);
        int ts_diff = (int)((timestamp - jb->last_timestamp) / 90); /* 90kHz → ms */
        int jitter_sample = abs(arrival_diff - ts_diff);
        /* Exponential moving average: est = est * 15/16 + sample * 1/16 */
        jb->est_jitter_ms = (jb->est_jitter_ms * 15 + jitter_sample) / 16;
    }
    jb->last_arrival_ms = now;
    jb->last_timestamp = timestamp;

    /* Adapt target delay: target = 2 * estimated jitter, clamped */
    if (jb->min_delay_ms > 0) {
        int new_target = jb->est_jitter_ms * 2;
        if (new_target < jb->min_delay_ms)
            new_target = jb->min_delay_ms;
        if (new_target > jb->max_delay_ms)
            new_target = jb->max_delay_ms;
        jb->target_delay_ms = new_target;
    }

    /* Find free slot */
    if (jb->count >= JB_MAX_PACKETS) {
        /* Buffer full — drop oldest */
        uint16_t oldest_seq = 0xFFFF;
        int oldest_idx = 0;
        for (int i = 0; i < JB_MAX_PACKETS; i++) {
            if (jb->slots[i].used && seq_compare(jb->slots[i].seq, oldest_seq) < 0) {
                oldest_seq = jb->slots[i].seq;
                oldest_idx = i;
            }
        }
        jb->slots[oldest_idx].used = false;
        jb->count--;
    }

    for (int i = 0; i < JB_MAX_PACKETS; i++) {
        if (!jb->slots[i].used) {
            jb->slots[i].used = true;
            jb->slots[i].seq = seq;
            jb->slots[i].timestamp = timestamp;
            jb->slots[i].marker = marker;
            jb->slots[i].arrival_ms = now;
            int copy_len = len;
            if (copy_len > (int)sizeof(jb->slots[i].data))
                copy_len = (int)sizeof(jb->slots[i].data);
            memcpy(jb->slots[i].data, data, (size_t)copy_len);
            jb->slots[i].len = copy_len;
            jb->count++;
            return;
        }
    }
}

int jitter_buffer_pop(jitter_buffer_t *jb, jitter_buffer_packet_t *out) {
    if (jb->count == 0)
        return -1;

    /* Find the slot with next_seq */
    for (int i = 0; i < JB_MAX_PACKETS; i++) {
        if (jb->slots[i].used && jb->slots[i].seq == jb->next_seq) {
            /* Check if enough delay has elapsed */
            uint64_t now = rtc_time_ms();
            int age = (int)(now - jb->slots[i].arrival_ms);
            if (age < jb->target_delay_ms)
                return -1; /* Not ready yet */

            out->data = jb->slots[i].data;
            out->len = jb->slots[i].len;
            out->timestamp = jb->slots[i].timestamp;
            out->seq = jb->slots[i].seq;
            out->marker = jb->slots[i].marker;

            jb->slots[i].used = false;
            jb->count--;
            jb->next_seq++;
            return 0;
        }
    }

    /* next_seq not found — check if we've waited too long (packet lost) */
    uint64_t now = rtc_time_ms();
    /* Find earliest available packet */
    uint16_t earliest_seq = 0;
    int earliest_idx = -1;
    for (int i = 0; i < JB_MAX_PACKETS; i++) {
        if (jb->slots[i].used) {
            if (earliest_idx < 0 || seq_compare(jb->slots[i].seq, earliest_seq) < 0) {
                earliest_seq = jb->slots[i].seq;
                earliest_idx = i;
            }
        }
    }

    if (earliest_idx >= 0) {
        int age = (int)(now - jb->slots[earliest_idx].arrival_ms);
        if (age > jb->max_delay_ms) {
            /* Skip lost packet(s), advance to earliest available */
            jb->next_seq = earliest_seq;
            return jitter_buffer_pop(jb, out); /* Retry with updated next_seq */
        }
    }

    return -1; /* Not ready */
}

int jitter_buffer_get_delay(jitter_buffer_t *jb) {
    return jb->target_delay_ms;
}

void jitter_buffer_destroy(jitter_buffer_t *jb) {
    free(jb);
}
