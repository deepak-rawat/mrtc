/*
 * SRTP (RFC 3711) - AES-128-CM with HMAC-SHA1-80.
 *
 * Minimal implementation: protect/unprotect RTP packets using
 * keying material derived from DTLS-SRTP.
 */
#ifndef RTC_SRTP_H
#define RTC_SRTP_H

#include "rtc_common.h"
#include <openssl/evp.h>

#define SRTP_MAX_KEY_LEN        16
#define SRTP_MAX_SALT_LEN       14
#define SRTP_AUTH_TAG_LEN       10
#define SRTP_MAX_PACKET         1500
#define SRTP_REPLAY_WINDOW_SIZE 128 /* RFC 3711 §3.3.2 (at least 64; we use 128) */

/* Key derivation labels (RFC 3711 section 4.3.1) */
#define SRTP_LABEL_RTP_ENCRYPTION  0x00
#define SRTP_LABEL_RTP_AUTH        0x01
#define SRTP_LABEL_RTP_SALT        0x02
#define SRTP_LABEL_RTCP_ENCRYPTION 0x03
#define SRTP_LABEL_RTCP_AUTH       0x04
#define SRTP_LABEL_RTCP_SALT       0x05

/* Sliding window for replay detection (RFC 3711 §3.3.2).
 * `highest_index` is the largest accepted packet index (48-bit for RTP,
 * 31-bit for RTCP); the bitmap covers indices
 * [highest_index - SRTP_REPLAY_WINDOW_SIZE + 1, highest_index]. Bit 0 is
 * highest_index itself, bit k is highest_index - k. */
typedef struct {
    uint64_t highest_index;
    uint64_t window[SRTP_REPLAY_WINDOW_SIZE / 64];
    bool initialized;
} rtc_srtp_replay_t;

typedef struct {
    /* Master key + salt (from DTLS export) */
    uint8_t master_key[SRTP_MAX_KEY_LEN];
    uint8_t master_salt[SRTP_MAX_SALT_LEN];

    /* Derived session keys (RTP) */
    uint8_t session_key[SRTP_MAX_KEY_LEN];
    uint8_t session_salt[SRTP_MAX_SALT_LEN];
    uint8_t session_auth_key[20]; /* HMAC-SHA1 key */

    /* Derived session keys (RTCP) */
    uint8_t rtcp_session_key[SRTP_MAX_KEY_LEN];
    uint8_t rtcp_session_salt[SRTP_MAX_SALT_LEN];
    uint8_t rtcp_session_auth_key[20];

    /* Rollover counter (RTP) */
    uint32_t roc;
    uint16_t last_seq;

    /* SRTCP index counter */
    uint32_t srtcp_index;

    /* Replay protection — receive direction only */
    rtc_srtp_replay_t rtp_replay;
    rtc_srtp_replay_t rtcp_replay;

    bool initialized;
} rtc_srtp_ctx_t;

/* Initialize SRTP context with master key + salt */
int rtc_srtp_init(rtc_srtp_ctx_t *ctx, const uint8_t *master_key, size_t key_len,
                  const uint8_t *master_salt, size_t salt_len);

/*
 * Protect an RTP packet in-place.
 * Input: buf contains a plain RTP packet of *len bytes.
 * Output: buf is overwritten with the SRTP packet, *len updated.
 * buf_cap is the total writable capacity of buf; the function rejects
 * (RTC_ERR_NOMEM) inputs where *len + SRTP_AUTH_TAG_LEN would overflow buf_cap.
 */
int rtc_srtp_protect(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len, size_t buf_cap);

/*
 * Unprotect an SRTP packet in-place.
 * Input: buf contains an SRTP packet of *len bytes.
 * Output: buf is overwritten with the plain RTP packet, *len updated.
 */
int rtc_srtp_unprotect(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len);

/*
 * Protect an RTCP packet in-place (SRTCP, RFC 3711 section 3.4).
 * Encrypts everything after the first 8 bytes (header + SSRC).
 * Appends 4-byte SRTCP index (with E-flag) + SRTP_AUTH_TAG_LEN auth tag.
 * buf_cap is the total writable capacity of buf; the function rejects
 * (RTC_ERR_NOMEM) inputs where *len + 4 + SRTP_AUTH_TAG_LEN would overflow.
 */
int rtc_srtp_protect_rtcp(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len, size_t buf_cap);

/*
 * Unprotect an SRTCP packet in-place.
 * Verifies auth tag, removes SRTCP index + tag, decrypts body.
 */
int rtc_srtp_unprotect_rtcp(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len);

/* Cleanup */
void rtc_srtp_close(rtc_srtp_ctx_t *ctx);

#endif /* RTC_SRTP_H */
