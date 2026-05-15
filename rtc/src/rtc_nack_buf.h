/*
 * rtc_nack_buf.h - Circular buffer for NACK retransmission.
 *
 * Stores the last N sent RTP packets (post-SRTP) indexed by sequence number.
 * On NACK receipt, packets can be looked up and retransmitted without
 * re-encryption since SRTP ciphertext is deterministic for a given seq/ROC.
 *
 * Thread safety: store() is called on main thread, get() on transport thread.
 * Lock-free by design — with 512 slots at ~30fps, the buffer holds ~17 seconds
 * of history, far exceeding any practical RTT.
 */
#ifndef RTC_NACK_BUF_H
#define RTC_NACK_BUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NACK_BUF_DEFAULT_SIZE 512
#define NACK_BUF_MAX_PKT_SIZE 1500

typedef struct {
    uint8_t data[NACK_BUF_MAX_PKT_SIZE];
    size_t len;
    uint16_t seq;
    bool used;
} rtc_nack_slot_t;

typedef struct {
    rtc_nack_slot_t *slots;
    int capacity; /* power of 2 for fast modulo via bitmask */
    int mask;     /* capacity - 1 */
} rtc_nack_buf_t;

/* Create a NACK buffer. max_packets is rounded up to next power of 2. */
rtc_nack_buf_t *rtc_nack_buf_create(int max_packets);

/* Destroy and free all memory. */
void rtc_nack_buf_destroy(rtc_nack_buf_t *buf);

/* Store a packet (post-SRTP). Overwrites oldest if buffer is full. */
void rtc_nack_buf_store(rtc_nack_buf_t *buf, const uint8_t *pkt, size_t len, uint16_t seq);

/* Look up a packet by sequence number. Returns true if found.
 * *out_pkt points into internal buffer (valid until overwritten). */
bool rtc_nack_buf_get(const rtc_nack_buf_t *buf, uint16_t seq,
                      const uint8_t **out_pkt, size_t *out_len);

#endif /* RTC_NACK_BUF_H */
