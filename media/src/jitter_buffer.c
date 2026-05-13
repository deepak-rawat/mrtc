/*
 * jitter_buffer.c - Packet reordering and adaptive delay.
 *
 * Slots are heap-allocated and indexed by RTP sequence number via
 * rtc_u32_map_t. JB_MAX_PACKETS caps the depth; the buffer evicts the
 * oldest slot when full. Pop returns a pointer into the popped slot;
 * the slot is kept alive in jb->last_popped until the next pop or destroy.
 */
#include "jitter_buffer.h"
#include <stdlib.h>
#include <string.h>

struct jb_slot {
    uint8_t data[2048];
    int len;
    uint16_t seq;
    uint32_t timestamp;
    bool marker; /* Last fragment of frame */
    uint64_t arrival_ms;
};

static int seq_compare(uint16_t a, uint16_t b) {
    /* Handles 16-bit wraparound: returns <0 if a is before b */
    int16_t diff = (int16_t)(a - b);
    return (int)diff;
}

jitter_buffer_t *jitter_buffer_create(const jitter_buffer_config_t *cfg) {
    jitter_buffer_t *jb = (jitter_buffer_t *)calloc(1, sizeof(*jb));
    if (!jb)
        return NULL;
    if (rtc_u32_map_init(&jb->by_seq) != RTC_OK) {
        free(jb);
        return NULL;
    }
    jb->target_delay_ms = cfg ? cfg->target_delay_ms : 80;
    jb->min_delay_ms = jb->target_delay_ms;
    jb->max_delay_ms = cfg ? cfg->max_delay_ms : 500;
    if (jb->target_delay_ms < 0)
        jb->target_delay_ms = 80;
    if (jb->max_delay_ms <= 0)
        jb->max_delay_ms = 500;
    return jb;
}

/* Find the slot with the smallest seq (by wrap-aware compare). */
static jb_slot_t *find_oldest(jitter_buffer_t *jb, uint16_t *out_seq) {
    rtc_u32_map_iter_t it = {0};
    uint32_t k;
    void *v;
    jb_slot_t *oldest = NULL;
    while (rtc_u32_map_next(&jb->by_seq, &it, &k, &v)) {
        jb_slot_t *s = (jb_slot_t *)v;
        if (!oldest || seq_compare(s->seq, oldest->seq) < 0)
            oldest = s;
    }
    if (oldest && out_seq)
        *out_seq = oldest->seq;
    return oldest;
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
        /* Packet is older than next_seq. If the gap is small (likely
         * reordering), pull next_seq back so we'll deliver this one.
         * Otherwise drop it as too old. */
        int gap = seq_compare(jb->next_seq, seq);
        if (gap > 0 && gap < JB_MAX_PACKETS / 2) {
            jb->next_seq = seq;
        } else {
            return;
        }
    }

    /* Drop exact duplicates */
    if (rtc_u32_map_get(&jb->by_seq, (uint32_t)seq))
        return;

    /* Update jitter estimate */
    if (jb->last_arrival_ms > 0) {
        int arrival_diff = (int)(now - jb->last_arrival_ms);
        int ts_diff = (int)((timestamp - jb->last_timestamp) / 90); /* 90kHz -> ms */
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

    /* Evict oldest if at capacity */
    if (rtc_u32_map_len(&jb->by_seq) >= JB_MAX_PACKETS) {
        uint16_t oldest_seq = 0;
        jb_slot_t *old = find_oldest(jb, &oldest_seq);
        if (old) {
            rtc_u32_map_remove(&jb->by_seq, (uint32_t)oldest_seq);
            free(old);
        }
    }

    /* Allocate new slot */
    jb_slot_t *slot = (jb_slot_t *)malloc(sizeof(*slot));
    if (!slot)
        return;
    int copy_len = len;
    if (copy_len < 0)
        copy_len = 0;
    if (copy_len > (int)sizeof(slot->data))
        copy_len = (int)sizeof(slot->data);
    memcpy(slot->data, data, (size_t)copy_len);
    slot->len = copy_len;
    slot->seq = seq;
    slot->timestamp = timestamp;
    slot->marker = marker;
    slot->arrival_ms = now;

    if (rtc_u32_map_set(&jb->by_seq, (uint32_t)seq, slot) != RTC_OK) {
        free(slot);
    }
}

int jitter_buffer_pop(jitter_buffer_t *jb, jitter_buffer_packet_t *out) {
    if (rtc_u32_map_len(&jb->by_seq) == 0)
        return -1;

    uint64_t now = rtc_time_ms();

    /* Try next_seq first */
    jb_slot_t *slot = (jb_slot_t *)rtc_u32_map_get(&jb->by_seq, (uint32_t)jb->next_seq);
    if (slot) {
        int age = (int)(now - slot->arrival_ms);
        if (age < jb->target_delay_ms)
            return -1; /* Not ready yet */

        rtc_u32_map_remove(&jb->by_seq, (uint32_t)jb->next_seq);
        free(jb->last_popped);
        jb->last_popped = slot;

        out->data = slot->data;
        out->len = slot->len;
        out->timestamp = slot->timestamp;
        out->seq = slot->seq;
        out->marker = slot->marker;

        jb->next_seq++;
        return 0;
    }

    /* next_seq missing - check if the earliest available has aged out */
    uint16_t earliest_seq = 0;
    jb_slot_t *earliest = find_oldest(jb, &earliest_seq);
    if (earliest) {
        int age = (int)(now - earliest->arrival_ms);
        if (age > jb->max_delay_ms) {
            /* Skip lost packet(s), advance to earliest available */
            jb->next_seq = earliest_seq;
            return jitter_buffer_pop(jb, out);
        }
    }

    return -1;
}

int jitter_buffer_get_delay(jitter_buffer_t *jb) {
    return jb->target_delay_ms;
}

void jitter_buffer_destroy(jitter_buffer_t *jb) {
    if (!jb)
        return;
    /* Free all remaining slots */
    rtc_u32_map_iter_t it = {0};
    uint32_t k;
    void *v;
    while (rtc_u32_map_next(&jb->by_seq, &it, &k, &v))
        free(v);
    rtc_u32_map_free(&jb->by_seq);
    free(jb->last_popped);
    free(jb);
}
