/*
 * Transport-Wide Congestion Control — Sender side history
 * (draft-holmer-rmcat-transport-wide-cc-extensions).
 *
 * Records the transport-wide sequence number, send time and packet size for
 * each outgoing media packet that carries the transport-cc header extension.
 * Indexed by `twcc_seq & (RING - 1)`. Slots are overwritten when the ring
 * wraps; receiver feedback that arrives later than RING packets is ignored.
 */
#ifndef RTC_TWCC_SENDER_H
#define RTC_TWCC_SENDER_H

#include "rtc_common.h"

#include <stdatomic.h>

#define RTC_TWCC_SENDER_RING 1024 /* must be power of two */

typedef struct {
    uint16_t seq;
    uint16_t size;
    uint64_t send_time_us;
    bool used;
} rtc_twcc_sent_pkt_t;

typedef struct {
    _Atomic uint16_t next_seq;
    rtc_twcc_sent_pkt_t ring[RTC_TWCC_SENDER_RING];
} rtc_twcc_sender_t;

void rtc_twcc_sender_init(rtc_twcc_sender_t *s);

/* Assign next transport-wide sequence number and record send time + size.
 * Returns the assigned 16-bit seq. */
uint16_t rtc_twcc_sender_assign(rtc_twcc_sender_t *s, uint64_t send_time_us, uint16_t size);

/* Lookup a previously-sent packet by transport seq. Returns NULL if the
 * ring slot no longer matches (overwritten / never set). */
const rtc_twcc_sent_pkt_t *rtc_twcc_sender_lookup(const rtc_twcc_sender_t *s, uint16_t seq);

#endif /* RTC_TWCC_SENDER_H */
