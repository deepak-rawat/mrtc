/*
 * rtc_nack_buf.h - Circular buffer for NACK retransmission.
 *
 * Stores the last N sent RTP packets (post-SRTP) indexed by sequence number.
 * On NACK receipt, packets can be looked up and retransmitted without
 * re-encryption since SRTP ciphertext is deterministic for a given seq/ROC.
 *
 * Thread safety: store() is called on main thread, get()/retransmit() on
 * transport thread. Lock-free by design — with 512 slots at ~30fps, the
 * buffer holds ~17 seconds of history, far exceeding any practical RTT.
 */
#ifndef RTC_NACK_BUF_H
#define RTC_NACK_BUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NACK_BUF_DEFAULT_SIZE 512
#define NACK_BUF_MAX_PKT_SIZE 1500

/* Cap on retransmits per RTP sequence number. WebRTC implementations typically
 * cap around 10 — beyond that the original is almost certainly lost forever
 * and further retransmits only waste bandwidth and tempt amplification abuse. */
#define RTC_NACK_MAX_RETRANSMITS 10

typedef struct {
    uint8_t data[NACK_BUF_MAX_PKT_SIZE];
    size_t len;
    uint16_t seq;
    uint16_t twcc_seq;        /* transport-cc seq carried by this packet (0 = none) */
    uint8_t retransmit_count; /* how many times retransmit() has succeeded for this seq */
    bool has_twcc;
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

/* Store a packet (post-SRTP). Overwrites oldest if buffer is full.
 * If the packet carried a transport-cc header extension, pass has_twcc=true
 * and twcc_seq=the assigned transport-wide seq; otherwise pass false / 0. */
void rtc_nack_buf_store(rtc_nack_buf_t *buf, const uint8_t *pkt, size_t len, uint16_t seq,
                        bool has_twcc, uint16_t twcc_seq);

/* Look up a packet by sequence number (read-only). Returns true if found.
 * *out_pkt points into internal buffer (valid until overwritten). */
bool rtc_nack_buf_get(const rtc_nack_buf_t *buf, uint16_t seq, const uint8_t **out_pkt,
                      size_t *out_len);

/* Retransmit path: look up by seq, increment the retransmit counter, and
 * return whether the caller should send the packet. Returns false if the
 * packet is missing OR if the per-seq cap (RTC_NACK_MAX_RETRANSMITS) is hit.
 * twcc_seq_out is set to the original transport-cc seq when the packet
 * carried one (so the caller can invalidate the TWCC sender ring to keep
 * BWE from feeding on stale send times). 0 means no TWCC. */
bool rtc_nack_buf_retransmit(rtc_nack_buf_t *buf, uint16_t seq, const uint8_t **out_pkt,
                             size_t *out_len, uint16_t *twcc_seq_out);

#endif /* RTC_NACK_BUF_H */
