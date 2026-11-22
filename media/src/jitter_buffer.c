/*
 * Packet reordering and adaptive delay.
 *
 * Slots are heap-allocated and indexed by RTP sequence number via
 * rtc_u32_map_t. JB_MAX_PACKETS caps the depth; the buffer evicts the
 * oldest slot when full. Pop returns a pointer into the popped slot;
 * the slot is kept alive in jb->last_popped until the next pop or destroy.
 */
#include "jitter_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct jb_slot {
    uint8_t data[4000];
    int len;
    uint16_t seq;
    uint32_t timestamp;
    bool marker; /* Last fragment of frame */
    uint64_t arrival_ms;
    int sample_count;
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
    jb->clock_rate = cfg && cfg->clock_rate ? cfg->clock_rate : 90000;
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

static int push_packet(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                       uint32_t timestamp, bool marker, int samples, uint64_t now) {
    if (!jb || !data || len <= 0 || len > (int)sizeof(((jb_slot_t *)0)->data)) {
        RTC_LOG_ERR("Jitter buffer rejected invalid packet size/data");
        return RTC_ERR_INVALID;
    }

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
        if (!jb->played && gap > 0 && gap < JB_MAX_PACKETS / 2) {
            jb->next_seq = seq;
        } else {
            return RTC_OK;
        }
    }

    /* Drop exact duplicates */
    if (rtc_u32_map_get(&jb->by_seq, (uint32_t)seq))
        return RTC_OK;

    /* Update jitter estimate */
    if (jb->last_arrival_ms > 0) {
        int64_t arrival_diff = (int64_t)(now - jb->last_arrival_ms);
        int64_t ts_diff =
            (int64_t)(int32_t)(timestamp - jb->last_timestamp) * 1000 / jb->clock_rate;
        int64_t jitter_sample = llabs(arrival_diff - ts_diff);
        if (jitter_sample > INT_MAX)
            jitter_sample = INT_MAX;
        /* Exponential moving average: est = est * 15/16 + sample * 1/16 */
        jb->est_jitter_ms = (int)(((int64_t)jb->est_jitter_ms * 15 + jitter_sample) / 16);
    }
    jb->last_arrival_ms = now;
    jb->last_timestamp = timestamp;

    /* Adapt target delay: target = 2 * estimated jitter, clamped */
    if (jb->min_delay_ms > 0) {
        int64_t new_target = (int64_t)jb->est_jitter_ms * 2;
        if (new_target < jb->min_delay_ms)
            new_target = jb->min_delay_ms;
        if (new_target > jb->max_delay_ms)
            new_target = jb->max_delay_ms;
        jb->target_delay_ms = (int)new_target;
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
    if (!slot) {
        RTC_LOG_ERR("Jitter buffer packet allocation failed");
        return RTC_ERR_NOMEM;
    }
    memcpy(slot->data, data, (size_t)len);
    slot->len = len;
    slot->seq = seq;
    slot->timestamp = timestamp;
    slot->marker = marker;
    slot->arrival_ms = now;
    slot->sample_count = samples;

    if (rtc_u32_map_set(&jb->by_seq, (uint32_t)seq, slot) != RTC_OK) {
        free(slot);
        RTC_LOG_ERR("Jitter buffer packet index allocation failed");
        return RTC_ERR_NOMEM;
    }
    return RTC_OK;
}

int jitter_buffer_push(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                       uint32_t timestamp, bool marker) {
    return push_packet(jb, data, len, seq, timestamp, marker, 0, rtc_time_ms());
}

int jitter_buffer_push_audio(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                             uint32_t timestamp, int samples, uint64_t now_ms) {
    if (samples <= 0) {
        RTC_LOG_ERR("Jitter buffer rejected invalid audio duration");
        return RTC_ERR_INVALID;
    }
    return push_packet(jb, data, len, seq, timestamp, false, samples, now_ms);
}

int jitter_buffer_pop(jitter_buffer_t *jb, jitter_buffer_packet_t *out) {
    if (rtc_u32_map_len(&jb->by_seq) == 0)
        return -1;

    uint64_t now = rtc_time_ms();

    /* Loop instead of recursing: each "skip lost packet" round advances
     * next_seq and retries from the top. */
    for (;;) {
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
            out->sample_count = slot->sample_count;

            jb->next_seq++;
            jb->played = true;
            return 0;
        }

        uint16_t earliest_seq = 0;
        jb_slot_t *earliest = find_oldest(jb, &earliest_seq);
        if (!earliest)
            return -1;
        int age = (int)(now - earliest->arrival_ms);
        if (age <= jb->max_delay_ms)
            return -1;
        jb->next_seq = earliest_seq;
    }
}

static void advance_audio(jitter_buffer_t *jb, uint32_t samples) {
    uint64_t duration = (uint64_t)samples * 1000 + jb->audio_time_remainder;
    jb->audio_deadline_ms += duration / jb->clock_rate;
    jb->audio_time_remainder = (uint32_t)(duration % jb->clock_rate);
    jb->audio_timestamp += samples;
}

int jitter_buffer_pop_audio(jitter_buffer_t *jb, uint64_t now_ms, jitter_buffer_packet_t *out) {
    jb_slot_t *slot;
    for (;;) {
        slot = find_oldest(jb, NULL);
        if (!jb->audio_started) {
            if (!slot)
                return -1;
            if (now_ms - slot->arrival_ms > (uint64_t)jb->max_delay_ms) {
                rtc_u32_map_remove(&jb->by_seq, slot->seq);
                free(slot);
                continue;
            }
            jb->audio_started = true;
            jb->audio_deadline_ms = slot->arrival_ms + (uint64_t)jb->target_delay_ms;
            jb->audio_timestamp = slot->timestamp;
            jb->audio_samples = slot->sample_count;
            jb->audio_time_remainder = 0;
            jb->concealed_samples = 0;
            jb->next_seq = slot->seq;
        }
        if (now_ms < jb->audio_deadline_ms)
            return -1;
        if (now_ms - jb->audio_deadline_ms > (uint64_t)jb->max_delay_ms) {
            jb->audio_started = false;
            continue;
        }
        int32_t gap = slot ? (int32_t)(slot->timestamp - jb->audio_timestamp) : 0;
        if (slot && gap < 0) {
            rtc_u32_map_remove(&jb->by_seq, slot->seq);
            jb->next_seq = (uint16_t)(slot->seq + 1);
            free(slot);
            continue;
        }
        if (slot && gap > 0 && slot->seq == jb->next_seq) {
            /* No transmitted sequence was lost: preserve the sender's silence
             * interval instead of filling DTX with concealment. */
            advance_audio(jb, (uint32_t)gap);
            jb->concealed_samples = 0;
            continue;
        }
        if (slot && gap == 0) {
            rtc_u32_map_remove(&jb->by_seq, slot->seq);
            free(jb->last_popped);
            jb->last_popped = slot;
            *out = (jitter_buffer_packet_t){.data = slot->data,
                                            .len = slot->len,
                                            .timestamp = slot->timestamp,
                                            .seq = slot->seq,
                                            .sample_count = slot->sample_count};
            jb->next_seq = (uint16_t)(slot->seq + 1);
            jb->audio_samples = slot->sample_count;
            jb->concealed_samples = 0;
            jb->played = true;
            advance_audio(jb, (uint32_t)slot->sample_count);
            return 0;
        }
        uint32_t limit = jb->clock_rate * JB_AUDIO_MAX_CONCEAL_MS / 1000;
        if (jb->concealed_samples >= limit) {
            if (!slot) {
                jb->audio_started = false;
                return -1;
            }
            advance_audio(jb, (uint32_t)gap);
            jb->next_seq = slot->seq;
            continue;
        }
        uint32_t samples = (uint32_t)jb->audio_samples;
        if (slot && (uint32_t)gap < samples)
            samples = (uint32_t)gap;
        if (samples > limit - jb->concealed_samples)
            samples = limit - jb->concealed_samples;
        *out = (jitter_buffer_packet_t){
            .timestamp = jb->audio_timestamp, .seq = jb->next_seq, .sample_count = (int)samples};
        jb->concealed_samples += samples;
        advance_audio(jb, samples);
        /* Do not consume hypothetical sequence numbers during an arrival gap:
         * a later contiguous packet may establish that the gap was DTX. */
        return 0;
    }
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
