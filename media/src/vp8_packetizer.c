/*
 * VP8 RTP packetization/depacketization (RFC 7741).
 */
#include "vp8_packetizer.h"

#include <string.h>
#include <stdio.h>

int rtc_vp8_packetize(const uint8_t *frame, size_t frame_len, bool is_keyframe,
                      rtc_vp8_payload_t *payloads, int max_payloads, int *payload_count) {
    if (!frame || !payloads || !payload_count)
        return RTC_ERR_INVALID;
    if (frame_len == 0)
        return RTC_ERR_INVALID;

    *payload_count = 0;
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

        rtc_vp8_payload_t *p = &payloads[*payload_count];

        /* VP8 payload descriptor (1 byte) */
        uint8_t pd = 0;
        if (first)
            pd |= 0x10; /* S bit: start of partition */
        if (!is_keyframe)
            pd |= 0x20; /* N bit: non-reference frame */
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

int rtc_vp8_depacketize(rtc_vp8_depacketizer_t *d, const uint8_t *payload, size_t len,
                        uint32_t timestamp, bool marker, const uint8_t **frame_out,
                        size_t *frame_len, bool *is_keyframe) {
    if (!d || !payload || !frame_out || !frame_len || !is_keyframe)
        return RTC_ERR_INVALID;

    if (len < VP8_PD_SIZE)
        return RTC_ERR_INVALID;

    /* Parse VP8 payload descriptor */
    uint8_t pd = payload[0];
    bool s_bit = (pd & 0x10) != 0; /* Start of partition */
    bool n_bit = (pd & 0x20) != 0; /* Non-reference frame */
    bool x_bit = (pd & 0x80) != 0; /* Extension present */

    /* Skip extension bytes if present (simplified: just skip 1 byte) */
    size_t pd_len = VP8_PD_SIZE;
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

    const uint8_t *vp8_data = payload + pd_len;
    size_t vp8_len = len - pd_len;

    /* New frame detection: S-bit or different timestamp */
    if (s_bit || (!d->collecting) || (d->collecting && timestamp != d->frame_timestamp)) {
        if (s_bit) {
            /* Start a new frame */
            d->frame_len = 0;
            d->frame_timestamp = timestamp;
            d->collecting = true;
            d->got_start = true;
        } else if (d->collecting && timestamp != d->frame_timestamp) {
            /* Timestamp changed without seeing S-bit — dropped packet, reset */
            d->collecting = false;
            d->frame_len = 0;
            return RTC_ERR_GENERIC; /* need more data */
        } else {
            /* Not collecting and no S-bit — ignore orphan fragment */
            return RTC_ERR_GENERIC;
        }
    }

    if (!d->collecting || !d->got_start)
        return RTC_ERR_GENERIC;

    /* Append VP8 data to frame buffer */
    if (d->frame_len + vp8_len > VP8_MAX_FRAME_SIZE) {
        d->collecting = false;
        d->frame_len = 0;
        return RTC_ERR_NOMEM;
    }
    memcpy(d->frame_buf + d->frame_len, vp8_data, vp8_len);
    d->frame_len += vp8_len;

    /* Marker bit indicates last packet of frame */
    if (marker) {
        *frame_out = d->frame_buf;
        *frame_len = d->frame_len;
        /* VP8 keyframe: first byte of VP8 data has bit 0 = 0 for keyframe */
        *is_keyframe = !n_bit;

        d->collecting = false;
        d->frame_len = 0;
        return RTC_OK;
    }

    /* More fragments needed */
    return RTC_ERR_GENERIC;
}
