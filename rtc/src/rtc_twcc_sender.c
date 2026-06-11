#include "rtc/rtc_twcc_sender.h"

#include <string.h>

void rtc_twcc_sender_init(rtc_twcc_sender_t *s) {
    memset(s, 0, sizeof(*s));
    atomic_store(&s->next_seq, 0);
}

uint16_t rtc_twcc_sender_assign(rtc_twcc_sender_t *s, uint64_t send_time_us, uint16_t size) {
    uint16_t seq = atomic_fetch_add(&s->next_seq, 1);
    rtc_twcc_sent_pkt_t *e = &s->ring[seq & (RTC_TWCC_SENDER_RING - 1)];
    e->seq = seq;
    e->size = size;
    e->send_time_us = send_time_us;
    e->used = true;
    return seq;
}

const rtc_twcc_sent_pkt_t *rtc_twcc_sender_lookup(const rtc_twcc_sender_t *s, uint16_t seq) {
    const rtc_twcc_sent_pkt_t *e = &s->ring[seq & (RTC_TWCC_SENDER_RING - 1)];
    if (!e->used || e->seq != seq)
        return NULL;
    return e;
}

void rtc_twcc_sender_invalidate(rtc_twcc_sender_t *s, uint16_t seq) {
    rtc_twcc_sent_pkt_t *e = &s->ring[seq & (RTC_TWCC_SENDER_RING - 1)];
    if (e->seq == seq)
        e->used = false;
}
