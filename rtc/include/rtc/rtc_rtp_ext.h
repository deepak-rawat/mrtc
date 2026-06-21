/*
 * RTP Header Extensions (RFC 8285) — one-byte form.
 *
 * Only the one-byte header form is implemented; it covers all WebRTC
 * extensions used in practice (ids 1..14, lengths 1..16 bytes).
 *
 * Wire format (one-byte form):
 *   defined-by-profile = 0xBEDE
 *   per-element header byte: (id << 4) | (len - 1)
 *     id   1..14   (0 = invalid, 15 = stop)
 *     len  1..16   (encoded as len-1)
 *   followed by `len` data bytes
 *   padding to 4-byte boundary with zero bytes (id == 0)
 */
#ifndef RTC_RTP_EXT_H
#define RTC_RTP_EXT_H

#include "rtc_common.h"

#define RTC_RTP_EXT_PROFILE_ONE_BYTE 0xBEDE
#define RTC_RTP_EXT_MAX_ENTRIES      8
#define RTC_RTP_EXT_MAX_DATA         16

typedef struct rtc_rtp_ext {
    uint8_t id;  /* 1..14 */
    uint8_t len; /* 1..16 */
    uint8_t data[RTC_RTP_EXT_MAX_DATA];
} rtc_rtp_ext_t;

/* Write the full extension block: 4-byte (profile|length) header followed
 * by one-byte-form entries, padded to 4-byte boundary.
 * Returns total bytes written (multiple of 4), or negative error code.
 * cap is the capacity of `out`. */
int rtc_rtp_ext_write_block(uint8_t *out, size_t cap, const rtc_rtp_ext_t *exts, size_t count);

/* Parse the body of an extension block (excluding the 4-byte
 * profile|length header). ext_data points at the first one-byte-form
 * entry; ext_len is `length` (16-bit value from the block header) times 4.
 *
 * Fills out[] with at most *count entries. On success *count is the
 * actual number parsed. */
int rtc_rtp_ext_parse_body(const uint8_t *ext_data, size_t ext_len, rtc_rtp_ext_t *out,
                           size_t *count);

/* Lookup helper: find entry by id, return pointer (into out[]) or NULL. */
const rtc_rtp_ext_t *rtc_rtp_ext_find(const rtc_rtp_ext_t *exts, size_t count, uint8_t id);

/* Canonical WebRTC extension URIs. */
#define RTC_EXT_URI_ABS_SEND_TIME "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
#define RTC_EXT_URI_TRANSPORT_CC \
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
#define RTC_EXT_URI_AUDIO_LEVEL "urn:ietf:params:rtp-hdrext:ssrc-audio-level"
#define RTC_EXT_URI_MID         "urn:ietf:params:rtp-hdrext:sdes:mid"
#define RTC_EXT_URI_RID         "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"
#define RTC_EXT_URI_REPAIRED_RID \
    "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id"
#define RTC_EXT_URI_TOFFSET "urn:ietf:params:rtp-hdrext:toffset"

/* abs-send-time: 24-bit fixed-point, 6.18 format, seconds & (1 << 18) - 1 */
void rtc_rtp_ext_make_abs_send_time(rtc_rtp_ext_t *out, uint8_t id, uint64_t now_us);

/* transport-cc: 16-bit transport-wide sequence number, big-endian */
void rtc_rtp_ext_make_transport_cc(rtc_rtp_ext_t *out, uint8_t id, uint16_t seq);
uint16_t rtc_rtp_ext_read_transport_cc(const rtc_rtp_ext_t *ext);

/* SDES string extensions (MID, RID): the value is an ASCII string of 1..16
 * bytes carried verbatim in a one-byte extension. */
void rtc_rtp_ext_make_string(rtc_rtp_ext_t *out, uint8_t id, const char *s);
/* Copy the extension value into `buf` as a NUL-terminated string (cap includes
 * the NUL). Returns the string length, or 0 if absent/empty. */
size_t rtc_rtp_ext_read_string(const rtc_rtp_ext_t *ext, char *buf, size_t cap);
/* Parse an extension block body, find the entry with `id`, and copy its value
 * as a NUL-terminated string into `buf`. Returns the length, or 0 if absent. */
size_t rtc_rtp_ext_get_string(const uint8_t *ext_data, size_t ext_len, uint8_t id, char *buf,
                              size_t cap);

#endif /* RTC_RTP_EXT_H */
