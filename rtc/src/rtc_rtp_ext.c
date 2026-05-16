/*
 * RTP Header Extensions (RFC 8285) — one-byte form.
 */
#include "rtc_rtp_ext.h"

#include <string.h>

int rtc_rtp_ext_write_block(uint8_t *out, size_t cap, const rtc_rtp_ext_t *exts, size_t count) {
    if (!out || (!exts && count > 0))
        return RTC_ERR_INVALID;
    if (cap < 4)
        return RTC_ERR_INVALID;

    /* Reserve 4 bytes for the profile|length header. */
    size_t body = 0;
    for (size_t i = 0; i < count; i++) {
        const rtc_rtp_ext_t *e = &exts[i];
        if (e->id < 1 || e->id > 14)
            return RTC_ERR_INVALID;
        if (e->len < 1 || e->len > RTC_RTP_EXT_MAX_DATA)
            return RTC_ERR_INVALID;
        if (4 + body + 1 + e->len > cap)
            return RTC_ERR_INVALID;
        out[4 + body] = (uint8_t)((e->id << 4) | (uint8_t)(e->len - 1));
        memcpy(out + 4 + body + 1, e->data, e->len);
        body += 1 + e->len;
    }
    /* Pad to 4-byte boundary with id=0 bytes (already zero from memset) */
    size_t pad = (4 - (body & 3)) & 3;
    if (4 + body + pad > cap)
        return RTC_ERR_INVALID;
    for (size_t i = 0; i < pad; i++)
        out[4 + body + i] = 0;

    size_t total_body = body + pad; /* multiple of 4 */
    uint16_t length_words = (uint16_t)(total_body / 4);

    out[0] = (RTC_RTP_EXT_PROFILE_ONE_BYTE >> 8) & 0xFF;
    out[1] = RTC_RTP_EXT_PROFILE_ONE_BYTE & 0xFF;
    out[2] = (length_words >> 8) & 0xFF;
    out[3] = length_words & 0xFF;

    return (int)(4 + total_body);
}

int rtc_rtp_ext_parse_body(const uint8_t *ext_data, size_t ext_len, rtc_rtp_ext_t *out,
                           size_t *count) {
    if (!out || !count)
        return RTC_ERR_INVALID;
    size_t cap = *count;
    size_t n = 0;
    size_t i = 0;
    while (i < ext_len) {
        uint8_t b = ext_data[i++];
        if (b == 0) /* padding */
            continue;
        uint8_t id = (b >> 4) & 0x0F;
        uint8_t len = (b & 0x0F) + 1;
        if (id == 15) /* stop */
            break;
        if (i + len > ext_len)
            return RTC_ERR_INVALID;
        if (n < cap && id >= 1 && id <= 14 && len <= RTC_RTP_EXT_MAX_DATA) {
            out[n].id = id;
            out[n].len = len;
            memcpy(out[n].data, ext_data + i, len);
            n++;
        }
        i += len;
    }
    *count = n;
    return RTC_OK;
}

const rtc_rtp_ext_t *rtc_rtp_ext_find(const rtc_rtp_ext_t *exts, size_t count, uint8_t id) {
    if (!exts)
        return NULL;
    for (size_t i = 0; i < count; i++)
        if (exts[i].id == id)
            return &exts[i];
    return NULL;
}

void rtc_rtp_ext_make_abs_send_time(rtc_rtp_ext_t *out, uint8_t id, uint64_t now_us) {
    /* 6.18 fixed-point seconds: floor(now_seconds * (1 << 18)) & 0x00FFFFFF */
    /* now_us * (1<<18) / 1e6 = now_us * 262144 / 1000000 */
    uint64_t ticks = (now_us * 262144ULL) / 1000000ULL;
    uint32_t v = (uint32_t)(ticks & 0x00FFFFFFu);
    out->id = id;
    out->len = 3;
    out->data[0] = (v >> 16) & 0xFF;
    out->data[1] = (v >> 8) & 0xFF;
    out->data[2] = v & 0xFF;
}

void rtc_rtp_ext_make_transport_cc(rtc_rtp_ext_t *out, uint8_t id, uint16_t seq) {
    out->id = id;
    out->len = 2;
    out->data[0] = (seq >> 8) & 0xFF;
    out->data[1] = seq & 0xFF;
}

uint16_t rtc_rtp_ext_read_transport_cc(const rtc_rtp_ext_t *ext) {
    if (!ext || ext->len < 2)
        return 0;
    return ((uint16_t)ext->data[0] << 8) | ext->data[1];
}
