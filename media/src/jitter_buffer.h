/*
 * jitter_buffer.h - Packet reordering and adaptive delay for RTP streams.
 */
#ifndef MEDIA_JITTER_BUFFER_H
#define MEDIA_JITTER_BUFFER_H

#include <rtc/rtc_common.h>
#include <stdint.h>
#include <stdbool.h>

#define JB_MAX_PACKETS 256

typedef struct {
    int target_delay_ms; /* Default: 80ms */
    int max_delay_ms;    /* Max before drop: 500ms */
} jitter_buffer_config_t;

typedef struct {
    uint8_t data[2048];
    int len;
    uint16_t seq;
    uint32_t timestamp;
    bool marker; /* Last fragment of frame */
    bool used;
    uint64_t arrival_ms; /* rtc_time_ms() when pushed */
} jb_slot_t;

typedef struct jitter_buffer {
    jb_slot_t slots[JB_MAX_PACKETS];
    int count;
    uint16_t next_seq; /* Next expected sequence number */
    bool started;

    /* Adaptive delay */
    int target_delay_ms;
    int min_delay_ms; /* Configured minimum (0 = no delay enforcement) */
    int max_delay_ms;
    int est_jitter_ms; /* Running estimate */
    uint64_t last_arrival_ms;
    uint32_t last_timestamp;
} jitter_buffer_t;

typedef struct {
    const uint8_t *data;
    int len;
    uint32_t timestamp;
    uint16_t seq;
    bool marker;
} jitter_buffer_packet_t;

jitter_buffer_t *jitter_buffer_create(const jitter_buffer_config_t *cfg);
void jitter_buffer_push(jitter_buffer_t *jb, const uint8_t *data, int len, uint16_t seq,
                        uint32_t timestamp, bool marker);
int jitter_buffer_pop(jitter_buffer_t *jb, jitter_buffer_packet_t *out);
int jitter_buffer_get_delay(jitter_buffer_t *jb);
void jitter_buffer_destroy(jitter_buffer_t *jb);

#endif /* MEDIA_JITTER_BUFFER_H */
