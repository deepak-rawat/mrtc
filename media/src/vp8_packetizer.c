/*
 * VP8 RTP packetization/depacketization (RFC 7741).
 */
#include "vp8_packetizer.h"

#include <string.h>
#include <stdio.h>

int rtc_vp8_packetize(const uint8_t *frame, size_t frame_len, bool is_keyframe,
                      video_payload_t *payloads, int max_payloads, int *payload_count) {
    if (!frame || !payloads || !payload_count)
        return RTC_ERR_INVALID;
    if (frame_len == 0)
        return RTC_ERR_INVALID;

    *payload_count = 0;
    (void)is_keyframe;
    size_t offset = 0;
    bool first = true;

    while (offset < frame_len) {
        if (*payload_count >= max_payloads)
            return RTC_ERR_NOMEM;

        size_t remaining = frame_len - offset;
        size_t frag_payload_len = remaining;
        if (frag_payload_len > VP8_MAX_FRAG_SIZE)
            frag_payload_len = VP8_MAX_FRAG_SIZE;

        bool last = (offset + frag_payload_len >= frame_len);

        video_payload_t *p = &payloads[*payload_count];

        /* VP8 payload descriptor (1 byte) */
        uint8_t pd = 0;
        if (first)
            pd |= 0x10; /* S bit: start of partition */
        p->data[0] = pd;

        /* VP8 frame data */
        memcpy(p->data + VP8_PD_SIZE, frame + offset, frag_payload_len);
        p->len = VP8_PD_SIZE + frag_payload_len;
        p->marker = last;

        offset += frag_payload_len;
        first = false;
        (*payload_count)++;
    }

    return RTC_OK;
}

int rtc_vp8_depacketizer_init(rtc_vp8_depacketizer_t *d) {
    if (!d)
        return RTC_ERR_INVALID;
    memset(d, 0, sizeof(*d));
    return RTC_OK;
}

int rtc_vp8_depacketize(rtc_vp8_depacketizer_t *d, const uint8_t *payload, size_t len, uint16_t seq,
                        uint32_t timestamp, bool marker, const uint8_t **frame_out,
                        size_t *frame_len, bool *is_keyframe) {
    if (!d || (!payload && len) || !frame_out || !frame_len || !is_keyframe)
        return RTC_ERR_INVALID;
    *frame_out = NULL;
    *frame_len = 0;
    *is_keyframe = false;

    /* Padding consumes sequence numbers, not VP8 frame boundaries. */
    if (!len) {
        if (!d->collecting || timestamp != d->frame_timestamp) {
            if (d->completed && seq == (uint16_t)(d->last_end_seq + 1))
                d->last_end_seq = seq;
            return RTC_ERR_GENERIC;
        }
        marker = false;
    }

    /* Parse VP8 payload descriptor */
    uint8_t pd = len ? payload[0] : 0;
    bool s_bit = (pd & 0x10) != 0 && (pd & 0x0f) == 0;
    bool x_bit = (pd & 0x80) != 0; /* Extension present */

    /* Skip extension bytes if present (simplified: just skip 1 byte) */
    size_t pd_len = len ? VP8_PD_SIZE : 0;
    if (x_bit) {
        if (len < 2)
            return RTC_ERR_INVALID;
        uint8_t ext = payload[1];
        pd_len = 2;
        /* Skip I, L, T, K extension bytes if flagged */
        if (ext & 0x80) {
            /* I: PictureID. RFC 7741 §4.2: if the high bit (M) of the first
             * PictureID byte is set, the ID is 15 bits over 2 bytes. */
            if (pd_len >= len)
                return RTC_ERR_INVALID;
            pd_len += (payload[pd_len] & 0x80) ? 2 : 1;
        }
        if (ext & 0x40)
            pd_len++; /* L: TL0PICIDX */
        if (ext & 0x20 || ext & 0x10)
            pd_len++; /* T/K: TID/KEYIDX */
        if (pd_len > len)
            return RTC_ERR_INVALID;
    }

    const uint8_t *vp8_data = len ? payload + pd_len : NULL;
    size_t vp8_len = len - pd_len;

    if ((len && !vp8_len) || vp8_len > VP8_MAX_FRAG_SIZE)
        return RTC_ERR_INVALID;
    if (d->initialized && timestamp != d->frame_timestamp &&
        (int32_t)(timestamp - d->frame_timestamp) < 0)
        return RTC_ERR_GENERIC;
    if (!d->initialized || timestamp != d->frame_timestamp) {
        if (d->collecting)
            d->dropped_frames++;
        memset(d->packets, 0, sizeof(d->packets));
        d->initialized = true;
        d->collecting = true;
        d->got_start = d->got_end = false;
        d->frame_timestamp = timestamp;
        d->frame_len = 0;
    }
    if (!d->collecting)
        return RTC_ERR_GENERIC;
    size_t slot = seq % VP8_MAX_FRAME_PACKETS;
    if (d->packets[slot].used) {
        if (d->packets[slot].seq == seq && d->packets[slot].len == vp8_len &&
            (!vp8_len || memcmp(d->packets[slot].data, vp8_data, vp8_len) == 0))
            return RTC_ERR_GENERIC;
        goto corrupt;
    }
    if (s_bit) {
        if (d->got_start && d->start_seq != seq)
            goto corrupt;
        d->got_start = true;
        d->start_seq = seq;
    }
    if (marker) {
        if (d->got_end && d->end_seq != seq)
            goto corrupt;
        d->got_end = true;
        d->end_seq = seq;
    }
    d->packets[slot].used = true;
    d->packets[slot].seq = seq;
    d->packets[slot].len = (uint16_t)vp8_len;
    if (vp8_len)
        memcpy(d->packets[slot].data, vp8_data, vp8_len);
    if (d->got_start && d->got_end) {
        unsigned count = (uint16_t)(d->end_seq - d->start_seq) + 1u;
        if (count > VP8_MAX_FRAME_PACKETS)
            goto corrupt;
        size_t total = 0;
        for (unsigned i = 0; i < count; i++) {
            uint16_t expected = (uint16_t)(d->start_seq + i);
            size_t index = expected % VP8_MAX_FRAME_PACKETS;
            if (!d->packets[index].used || d->packets[index].seq != expected)
                return RTC_ERR_GENERIC;
            if (total + d->packets[index].len > sizeof(d->frame_buf))
                goto corrupt;
            total += d->packets[index].len;
        }
        size_t offset = 0;
        for (unsigned i = 0; i < count; i++) {
            size_t index = (uint16_t)(d->start_seq + i) % VP8_MAX_FRAME_PACKETS;
            memcpy(d->frame_buf + offset, d->packets[index].data, d->packets[index].len);
            offset += d->packets[index].len;
        }
        d->frame_len = total;
        if (d->completed && d->start_seq != (uint16_t)(d->last_end_seq + 1))
            d->dropped_frames++;
        d->completed = true;
        d->last_end_seq = d->end_seq;
        *frame_out = d->frame_buf;
        *frame_len = total;
        *is_keyframe = (d->frame_buf[0] & 1) == 0;
        d->collecting = false;
        return RTC_OK;
    }
    return RTC_ERR_GENERIC;
corrupt:
    d->dropped_frames++;
    d->collecting = false;
    return RTC_ERR_INVALID;
}
