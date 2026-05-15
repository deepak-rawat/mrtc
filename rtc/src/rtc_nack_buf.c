/*
 * rtc_nack_buf.c - Circular buffer for NACK retransmission.
 */
#include "rtc_nack_buf.h"
#include <stdlib.h>
#include <string.h>

/* Round up to next power of 2. */
static int next_pow2(int v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v < 2 ? 2 : v;
}

rtc_nack_buf_t *rtc_nack_buf_create(int max_packets) {
    if (max_packets <= 0)
        max_packets = NACK_BUF_DEFAULT_SIZE;

    rtc_nack_buf_t *buf = (rtc_nack_buf_t *)calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->capacity = next_pow2(max_packets);
    buf->mask = buf->capacity - 1;
    buf->slots = (rtc_nack_slot_t *)calloc((size_t)buf->capacity, sizeof(rtc_nack_slot_t));
    if (!buf->slots) {
        free(buf);
        return NULL;
    }

    return buf;
}

void rtc_nack_buf_destroy(rtc_nack_buf_t *buf) {
    if (!buf)
        return;
    free(buf->slots);
    free(buf);
}

void rtc_nack_buf_store(rtc_nack_buf_t *buf, const uint8_t *pkt, size_t len, uint16_t seq) {
    if (!buf || !pkt || len == 0 || len > NACK_BUF_MAX_PKT_SIZE)
        return;

    int idx = (int)seq & buf->mask;
    rtc_nack_slot_t *slot = &buf->slots[idx];
    memcpy(slot->data, pkt, len);
    slot->len = len;
    slot->seq = seq;
    slot->used = true;
}

bool rtc_nack_buf_get(const rtc_nack_buf_t *buf, uint16_t seq,
                      const uint8_t **out_pkt, size_t *out_len) {
    if (!buf || !out_pkt || !out_len)
        return false;

    int idx = (int)seq & buf->mask;
    const rtc_nack_slot_t *slot = &buf->slots[idx];

    if (!slot->used || slot->seq != seq) {
        *out_pkt = NULL;
        *out_len = 0;
        return false;
    }

    *out_pkt = slot->data;
    *out_len = slot->len;
    return true;
}
