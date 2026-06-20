/*
 * Transport-Wide CC — Receiver side
 * (draft-holmer-rmcat-transport-wide-cc-extensions §3).
 *
 * Records arrival of media packets tagged with transport-cc seq numbers and
 * periodically emits an RTCP Transport Feedback (PT=205, FMT=15) packet
 * describing arrival deltas for the recorded window.
 */
#ifndef RTC_TWCC_RECEIVER_H
#define RTC_TWCC_RECEIVER_H

#include "rtc_common.h"

#define RTC_TWCC_RECV_WINDOW 256

typedef struct {
    uint16_t seq;
    uint64_t recv_time_us;
    bool received;
} rtc_twcc_recv_entry_t;

typedef struct {
    /* Arrival window. Index 0 corresponds to `base_seq`. */
    rtc_twcc_recv_entry_t window[RTC_TWCC_RECV_WINDOW];
    uint16_t base_seq;
    int count; /* number of entries in window (received + missing) */
    bool have_base;

    uint8_t fb_pkt_count;
} rtc_twcc_receiver_t;

void rtc_twcc_receiver_init(rtc_twcc_receiver_t *r);

/* Record arrival of one packet. */
void rtc_twcc_receiver_on_packet(rtc_twcc_receiver_t *r, uint16_t twcc_seq, uint64_t recv_time_us);

/* Build a TWCC feedback RTCP packet covering the buffered window and reset
 * the window. Writes into `out` (caller-supplied, at least 1200 bytes).
 * Returns RTC_OK and sets *out_len on success. Returns RTC_ERR_INVALID if
 * there is nothing to report. */
int rtc_twcc_receiver_build_feedback(rtc_twcc_receiver_t *r, uint32_t sender_ssrc,
                                     uint32_t media_ssrc, uint8_t *out, size_t cap,
                                     size_t *out_len);

#endif /* RTC_TWCC_RECEIVER_H */
