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

#define SRTP_MAX_KEY_LEN        32 /* room for AES-256-GCM; AES-128 uses 16 */
#define SRTP_MAX_SALT_LEN       14
#define SRTP_AUTH_TAG_LEN       10
#define SRTP_GCM_TAG_LEN        16 /* AEAD AES-GCM auth tag (RFC 7714) */
#define SRTP_GCM_SALT_LEN       12 /* AEAD AES-GCM salt (RFC 7714 §11) */
#define SRTP_GCM_IV_LEN         12 /* AEAD AES-GCM IV (RFC 7714 §8.1/§9.1) */
#define SRTP_MAX_PACKET         1500
#define SRTP_REPLAY_WINDOW_SIZE 128 /* RFC 3711 §3.3.2 (at least 64; we use 128) */

/* Negotiated SRTP protection profile (selected by DTLS-SRTP). */
typedef enum {
    RTC_SRTP_PROFILE_AES128_CM_SHA1_80 = 0, /* RFC 3711 AES-CM + HMAC-SHA1-80 */
    RTC_SRTP_PROFILE_AEAD_AES_128_GCM = 1,  /* RFC 7714 AES-128-GCM */
    RTC_SRTP_PROFILE_AEAD_AES_256_GCM = 2,  /* RFC 7714 AES-256-GCM */
} rtc_srtp_profile_t;

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

/* Per-SSRC stream state. RFC 3711 §3.2.3: the cryptographic context is keyed on
 * the SSRC — bundled WebRTC streams (audio + video on one transport) share a
 * single SRTP key but have independent sequence-number spaces, so each SSRC
 * needs its own rollover counter (ROC) and replay window. A single shared ROC
 * across SSRCs would mis-estimate rollover whenever two streams interleave and
 * corrupt the AES-CM IV. */
#define SRTP_REPLAY_MAX_STREAMS 8
typedef struct {
    uint32_t ssrc;
    bool in_use;
    uint64_t lru_tick;        /* set on each access; smallest is evicted when full */
    uint32_t roc;             /* rollover counter for this SSRC */
    uint16_t last_seq;        /* last sequence number seen for this SSRC */
    bool roc_init;            /* false until the first packet for this SSRC */
    rtc_srtp_replay_t replay; /* receive direction only */
} rtc_srtp_replay_entry_t;

/* Thread safety: rtc_srtp_protect / rtc_srtp_unprotect / rtc_srtp_protect_rtcp
 * / rtc_srtp_unprotect_rtcp may be called concurrently from any thread; the
 * context serializes them internally with `lock`. Required because the
 * encoder thread (RTP send) and transport thread (RTCP SR/RR/TWCC timers,
 * NACK retransmit) share the same sender context, and concurrent updates of
 * the per-SSRC rollover state / `srtcp_index` would otherwise cause IV reuse —
 * fatal for AES-CM. Init / close are not thread-safe and must not race. */
typedef struct {
    /* Serializes the four protect/unprotect entry points. */
    rtc_mutex_t lock;

    /* Negotiated profile, session key length (16 / 32) and salt length (14 for
     * CM, 12 for GCM). */
    rtc_srtp_profile_t profile;
    size_t key_len;
    size_t salt_len;

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

    /* SRTCP index counter (sender side; the index space is per-context). */
    uint32_t srtcp_index;

    /* Per-SSRC RTP rollover + replay. rtp_send carries the send-direction ROC;
     * rtp_replay / rtcp_replay carry the receive-direction ROC + replay window. */
    rtc_srtp_replay_entry_t rtp_send[SRTP_REPLAY_MAX_STREAMS];
    rtc_srtp_replay_entry_t rtp_replay[SRTP_REPLAY_MAX_STREAMS];
    rtc_srtp_replay_entry_t rtcp_replay[SRTP_REPLAY_MAX_STREAMS];
    uint64_t send_lru_tick;
    uint64_t replay_lru_tick;

    bool initialized;
} rtc_srtp_ctx_t;

/* Initialize SRTP context with master key + salt */
int rtc_srtp_init(rtc_srtp_ctx_t *ctx, const uint8_t *master_key, size_t key_len,
                  const uint8_t *master_salt, size_t salt_len);

/* Initialize with an explicit protection profile. rtc_srtp_init() is the
 * AES-CM-SHA1-80 shorthand. For AEAD_AES_128_GCM pass a 12-byte salt. */
int rtc_srtp_init_profile(rtc_srtp_ctx_t *ctx, rtc_srtp_profile_t profile,
                          const uint8_t *master_key, size_t key_len, const uint8_t *master_salt,
                          size_t salt_len);

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
