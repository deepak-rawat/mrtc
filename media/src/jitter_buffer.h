/*
 * Packet reordering and adaptive delay for RTP streams.
 */
#ifndef MEDIA_JITTER_BUFFER_H
#define MEDIA_JITTER_BUFFER_H

#include <rtc/rtc_common.h>
#include <rtc/rtc_u32_map.h>
#include <stdint.h>
#include <stdbool.h>

#define JB_MAX_PACKETS          256
#define JB_AUDIO_MAX_CONCEAL_MS 120

typedef struct {
    int target_delay_ms; /* Default: 80ms */
    int max_delay_ms;    /* Max before drop: 500ms */
    uint32_t clock_rate; /* RTP ticks per second; zero preserves the 90kHz default */
} jitter_buffer_config_t;

/* Opaque slot type (defined in jitter_buffer.c) */
typedef struct jb_slot jb_slot_t;

typedef struct jitter_buffer {
    /* Map of (uint32_t)seq -> jb_slot_t* (heap-allocated, owned) */
    rtc_u32_map_t by_seq;
    /* Slot owned by the last successful pop; freed on next pop or destroy.
     * Keeps the popped payload valid for the caller until the next call. */
    jb_slot_t *last_popped;

    uint16_t next_seq; /* Next expected sequence number */
    bool started;

    /* Adaptive delay */
    int target_delay_ms;
    int min_delay_ms; /* Configured minimum (0 = no delay enforcement) */
    int max_delay_ms;
    int est_jitter_ms; /* Running estimate */
    uint64_t last_arrival_ms;
    uint32_t last_timestamp;
    uint32_t clock_rate;
    bool played;
    bool audio_started;
    uint64_t audio_deadline_ms;
    uint32_t audio_timestamp;
    uint32_t audio_time_remainder;
    int audio_samples;
    uint32_t concealed_samples;
} jitter_buffer_t;

typedef struct {
    const uint8_t *data;
    int len;
    uint32_t timestamp;
    uint16_t seq;
    bool marker;
    int sample_count; /* audio duration, including a concealment interval */
} jitter_buffer_packet_t;

jitter_buffer_t *jitter_buffer_create(const jitter_buffer_config_t *cfg);
int jitter_buffer_push(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                       uint32_t timestamp, bool marker);
int jitter_buffer_push_audio(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                             uint32_t timestamp, int samples, uint64_t now_ms);
int jitter_buffer_pop(jitter_buffer_t *jb, jitter_buffer_packet_t *out);
/* Clock-driven audio output. NULL data denotes bounded PLC; contiguous
 * sequence numbers with timestamp gaps denote DTX, not lost packets. */
int jitter_buffer_pop_audio(jitter_buffer_t *jb, uint64_t now_ms, jitter_buffer_packet_t *out);
int jitter_buffer_get_delay(jitter_buffer_t *jb);
void jitter_buffer_destroy(jitter_buffer_t *jb);

#endif /* MEDIA_JITTER_BUFFER_H */
