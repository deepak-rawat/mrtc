/*
 * SRTP (RFC 3711) AES-128-CM with HMAC-SHA1-80.
 *
 * Implements key derivation, encryption/decryption, and authentication
 * for RTP packets using AES-128 Counter Mode.
 */
#include "rtc_srtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>

/* HMAC-SHA1 helper using modern EVP_MAC API */
static int srtp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data1,
                          size_t data1_len, const uint8_t *data2, size_t data2_len, uint8_t *out,
                          size_t out_size) {
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac)
        return RTC_ERR_SRTP;

    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!ctx)
        return RTC_ERR_SRTP;

    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, "SHA1", 0),
                           OSSL_PARAM_construct_end()};

    int ret = RTC_ERR_SRTP;
    if (EVP_MAC_init(ctx, key, key_len, params) && EVP_MAC_update(ctx, data1, data1_len) &&
        (data2_len == 0 || EVP_MAC_update(ctx, data2, data2_len))) {
        size_t outl = out_size;
        if (EVP_MAC_final(ctx, out, &outl, out_size))
            ret = RTC_OK;
    }

    EVP_MAC_CTX_free(ctx);
    return ret;
}

/*
 * Replay window check (RFC 3711 §3.3.2).
 * `index` is the packet's reconstructed 48-bit RTP index (ROC<<16 | seq) or
 * 31-bit SRTCP index. Returns RTC_OK if the packet is fresh (and updates the
 * window on success); RTC_ERR_SRTP for duplicate or too-old packets.
 * Must be called AFTER auth-tag verification to avoid spoofed-replay DoS.
 */
static int srtp_replay_check(rtc_srtp_replay_t *r, uint64_t index) {
    if (!r->initialized) {
        r->initialized = true;
        r->highest_index = index;
        r->window[0] = 1ULL; /* bit 0 = highest */
        r->window[1] = 0;
        return RTC_OK;
    }

    if (index > r->highest_index) {
        /* New highest: shift window left by delta, set new bit 0 */
        uint64_t delta = index - r->highest_index;
        if (delta >= SRTP_REPLAY_WINDOW_SIZE) {
            /* Big jump — clear the whole window. */
            r->window[0] = 1ULL;
            r->window[1] = 0;
        } else if (delta >= 64) {
            r->window[1] = r->window[0] << (delta - 64);
            r->window[0] = 0;
            r->window[0] |= 1ULL;
        } else {
            /* delta in [1,63] */
            uint64_t hi_shift =
                (r->window[1] << delta) | (delta == 0 ? 0 : (r->window[0] >> (64 - delta)));
            r->window[1] = hi_shift;
            r->window[0] = (r->window[0] << delta) | 1ULL;
        }
        r->highest_index = index;
        return RTC_OK;
    }

    /* index <= highest_index: check if within window */
    uint64_t delta = r->highest_index - index;
    if (delta >= SRTP_REPLAY_WINDOW_SIZE) {
        RTC_LOG_WARN("SRTP: packet too old (delta=%llu) — replay rejected",
                     (unsigned long long)delta);
        return RTC_ERR_SRTP;
    }

    uint64_t bit_index = delta;
    uint64_t mask = 1ULL << (bit_index & 63);
    uint64_t *word = &r->window[bit_index >> 6];
    if (*word & mask) {
        RTC_LOG_WARN("SRTP: duplicate packet (index=%llu) — replay rejected",
                     (unsigned long long)index);
        return RTC_ERR_SRTP;
    }
    *word |= mask;
    return RTC_OK;
}

/*
 * Find the per-SSRC stream entry for `ssrc` without allocating. Returns NULL if
 * the SSRC has no entry yet. Used on the receive path to estimate the ROC for
 * auth-tag verification before a (potentially spoofed) packet is trusted, so a
 * forged SSRC cannot churn the table prior to authentication.
 */
static rtc_srtp_replay_entry_t *srtp_find_ssrc(rtc_srtp_replay_entry_t *table, size_t n,
                                               uint32_t ssrc) {
    for (size_t i = 0; i < n; i++) {
        if (table[i].in_use && table[i].ssrc == ssrc)
            return &table[i];
    }
    return NULL;
}

/*
 * Find (or allocate) the per-SSRC stream entry for `ssrc` in a table.
 * On overflow evicts the least-recently-used entry. Never fails.
 */
static rtc_srtp_replay_entry_t *srtp_entry_for_ssrc(rtc_srtp_replay_entry_t *table, size_t n,
                                                    uint64_t *lru_tick, uint32_t ssrc) {
    rtc_srtp_replay_entry_t *free_slot = NULL;
    rtc_srtp_replay_entry_t *lru_slot = &table[0];
    for (size_t i = 0; i < n; i++) {
        if (table[i].in_use && table[i].ssrc == ssrc) {
            table[i].lru_tick = ++(*lru_tick);
            return &table[i];
        }
        if (!table[i].in_use && !free_slot)
            free_slot = &table[i];
        if (table[i].lru_tick < lru_slot->lru_tick)
            lru_slot = &table[i];
    }

    rtc_srtp_replay_entry_t *slot = free_slot ? free_slot : lru_slot;
    if (slot == lru_slot && slot->in_use) {
        RTC_LOG_WARN("SRTP: stream table full, evicting SSRC=0x%08x for SSRC=0x%08x", slot->ssrc,
                     ssrc);
    }
    memset(slot, 0, sizeof(*slot));
    slot->ssrc = ssrc;
    slot->in_use = true;
    slot->lru_tick = ++(*lru_tick);
    return slot;
}

/*
 * SRTP Key Derivation Function (RFC 3711, Section 4.3.1)
 *
 * derived_key = AES-CM(master_key, master_salt XOR (label << 48))
 */
static int srtp_kdf(const uint8_t *master_key, const uint8_t *master_salt, size_t salt_len,
                    uint8_t label, uint8_t *out, size_t out_len) {
    /* Build the "x" value: salt XOR (label || index) padded to 14 bytes */
    uint8_t x[14];
    memset(x, 0, sizeof(x));
    if (salt_len > 14)
        salt_len = 14;
    memcpy(x, master_salt, salt_len);
    x[7] ^= label; /* label goes at byte 7 (after 48-bit shift for index=0) */

    /* Use AES-128-ECB to generate keystream blocks */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return RTC_ERR_NOMEM;

    /* AES-CM: encrypt counter blocks with master_key */
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));
    memcpy(iv, x, 14);
    /* iv[14..15] = 0 (block counter starts at 0) */

    size_t produced = 0;
    uint16_t block_count = 0;

    while (produced < out_len) {
        /* Set counter in iv[14..15] */
        iv[14] = (block_count >> 8) & 0xFF;
        iv[15] = block_count & 0xFF;

        /* AES-ECB always emits a full 16-byte block; derive into a scratch
         * block and copy only the bytes still requested so a non-multiple-of-16
         * out_len (e.g. 14-byte salt, 20-byte HMAC key) never overruns out. */
        uint8_t block[16];
        int outl = 0;
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, master_key, NULL);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_EncryptUpdate(ctx, block, &outl, iv, 16);
        EVP_EncryptFinal_ex(ctx, block + outl, &outl);

        size_t remaining = out_len - produced;
        size_t copy = remaining < 16 ? remaining : 16;
        memcpy(out + produced, block, copy);

        produced += copy;
        block_count++;
    }

    EVP_CIPHER_CTX_free(ctx);
    return RTC_OK;
}

int rtc_srtp_init(rtc_srtp_ctx_t *ctx, const uint8_t *master_key, size_t key_len,
                  const uint8_t *master_salt, size_t salt_len) {
    memset(ctx, 0, sizeof(*ctx));

    /* Init the per-context lock before any field that protect/unprotect
     * touches. Failure here is fatal — without the lock the context is
     * unsafe to use from multiple threads, which is the documented contract. */
    if (rtc_mutex_init(&ctx->lock) != RTC_OK)
        return RTC_ERR_GENERIC;

    if (key_len > SRTP_MAX_KEY_LEN)
        key_len = SRTP_MAX_KEY_LEN;
    if (salt_len > SRTP_MAX_SALT_LEN)
        salt_len = SRTP_MAX_SALT_LEN;

    memcpy(ctx->master_key, master_key, key_len);
    memcpy(ctx->master_salt, master_salt, salt_len);

    /* Derive session keys */
    int rc;
    rc = srtp_kdf(master_key, master_salt, salt_len, SRTP_LABEL_RTP_ENCRYPTION, ctx->session_key,
                  SRTP_MAX_KEY_LEN);
    if (rc != RTC_OK)
        return rc;

    rc = srtp_kdf(master_key, master_salt, salt_len, SRTP_LABEL_RTP_SALT, ctx->session_salt,
                  SRTP_MAX_SALT_LEN);
    if (rc != RTC_OK)
        return rc;

    uint8_t auth_key[20];
    rc = srtp_kdf(master_key, master_salt, salt_len, SRTP_LABEL_RTP_AUTH, auth_key, 20);
    if (rc != RTC_OK)
        return rc;
    memcpy(ctx->session_auth_key, auth_key, 20);

    /* Derive RTCP session keys (labels 0x03-0x05) */
    rc = srtp_kdf(master_key, master_salt, salt_len, SRTP_LABEL_RTCP_ENCRYPTION,
                  ctx->rtcp_session_key, SRTP_MAX_KEY_LEN);
    if (rc != RTC_OK)
        return rc;

    rc = srtp_kdf(master_key, master_salt, salt_len, SRTP_LABEL_RTCP_SALT, ctx->rtcp_session_salt,
                  SRTP_MAX_SALT_LEN);
    if (rc != RTC_OK)
        return rc;

    uint8_t rtcp_auth_key[20];
    rc = srtp_kdf(master_key, master_salt, salt_len, SRTP_LABEL_RTCP_AUTH, rtcp_auth_key, 20);
    if (rc != RTC_OK)
        return rc;
    memcpy(ctx->rtcp_session_auth_key, rtcp_auth_key, 20);

    ctx->initialized = true;
    RTC_LOG_INFO("SRTP: context initialized");
    return RTC_OK;
}

/*
 * AES-128-CM encryption/decryption (same operation - XOR with keystream).
 *
 * IV construction (RFC 3711 Section 4.1.1):
 *   IV = (session_salt XOR (SSRC || packet_index)) padded to 16 bytes
 */
static int srtp_aes_cm(const uint8_t *session_key, const uint8_t *session_salt, uint32_t ssrc,
                       uint32_t roc, uint16_t seq, uint8_t *data, size_t len) {
    /* Build IV */
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));

    /* SSRC at bytes 4..7 */
    iv[4] = (ssrc >> 24) & 0xFF;
    iv[5] = (ssrc >> 16) & 0xFF;
    iv[6] = (ssrc >> 8) & 0xFF;
    iv[7] = ssrc & 0xFF;

    /* Packet index (ROC || SEQ) at bytes 8..13 */
    iv[8] = (roc >> 24) & 0xFF;
    iv[9] = (roc >> 16) & 0xFF;
    iv[10] = (roc >> 8) & 0xFF;
    iv[11] = roc & 0xFF;
    iv[12] = (seq >> 8) & 0xFF;
    iv[13] = seq & 0xFF;

    /* XOR with session salt */
    for (int i = 0; i < 14; i++)
        iv[i] ^= session_salt[i];

    /* AES-128-CTR with the constructed IV */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return RTC_ERR_NOMEM;

    /* Use AES-128-CTR (OpenSSL handles the counter increment) */
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, session_key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return RTC_ERR_SRTP;
    }

    int outl = 0;
    EVP_EncryptUpdate(ctx, data, &outl, data, (int)len);
    EVP_EncryptFinal_ex(ctx, data + outl, &outl);
    EVP_CIPHER_CTX_free(ctx);

    return RTC_OK;
}

int rtc_srtp_protect(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len, size_t buf_cap) {
    if (!ctx->initialized)
        return RTC_ERR_SRTP;
    if (*len < 12)
        return RTC_ERR_INVALID; /* minimum RTP header */
    /* Ensure room to append 80-bit auth tag without writing past buf_cap. */
    if (*len > buf_cap || buf_cap - *len < SRTP_AUTH_TAG_LEN)
        return RTC_ERR_NOMEM;

    rtc_mutex_lock(&ctx->lock);

    /* Parse RTP header for SSRC and sequence number */
    uint16_t seq = ((uint16_t)buf[2] << 8) | buf[3];
    uint32_t ssrc =
        ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) | buf[11];

    /* Per-SSRC rollover: bundled streams share this context but have
     * independent sequence spaces, so the ROC must be tracked per SSRC. */
    rtc_srtp_replay_entry_t *st =
        srtp_entry_for_ssrc(ctx->rtp_send, SRTP_REPLAY_MAX_STREAMS, &ctx->send_lru_tick, ssrc);
    if (!st->roc_init) {
        st->roc_init = true;
    } else if (seq < st->last_seq && (st->last_seq - seq) > 0x8000) {
        st->roc++;
    }
    st->last_seq = seq;
    uint32_t roc = st->roc;

    /* Determine header length (handle CSRC and extension) */
    size_t hdr_len = 12;
    uint8_t cc = buf[0] & 0x0F;
    hdr_len += (size_t)cc * 4;
    if (buf[0] & 0x10) { /* extension bit */
        if (hdr_len + 4 > *len) {
            rtc_mutex_unlock(&ctx->lock);
            return RTC_ERR_INVALID;
        }
        uint16_t ext_len = ((uint16_t)buf[hdr_len + 2] << 8) | buf[hdr_len + 3];
        hdr_len += 4 + (size_t)ext_len * 4;
    }
    if (hdr_len > *len) {
        rtc_mutex_unlock(&ctx->lock);
        return RTC_ERR_INVALID;
    }

    /* Encrypt payload (not header) */
    int rc = srtp_aes_cm(ctx->session_key, ctx->session_salt, ssrc, roc, seq, buf + hdr_len,
                         *len - hdr_len);
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return rc;
    }

    /* Compute authentication tag: HMAC-SHA1 over (header + encrypted payload + ROC) */
    uint8_t roc_buf[4];
    roc_buf[0] = (roc >> 24) & 0xFF;
    roc_buf[1] = (roc >> 16) & 0xFF;
    roc_buf[2] = (roc >> 8) & 0xFF;
    roc_buf[3] = roc & 0xFF;

    uint8_t hmac[20];
    rc = srtp_hmac_sha1(ctx->session_auth_key, 20, buf, *len, roc_buf, 4, hmac, sizeof(hmac));
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return rc;
    }

    /* Append truncated (80-bit / 10 byte) tag */
    memcpy(buf + *len, hmac, SRTP_AUTH_TAG_LEN);
    *len += SRTP_AUTH_TAG_LEN;

    rtc_mutex_unlock(&ctx->lock);
    return RTC_OK;
}

int rtc_srtp_unprotect(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len) {
    if (!ctx->initialized)
        return RTC_ERR_SRTP;
    if (*len < 12 + SRTP_AUTH_TAG_LEN)
        return RTC_ERR_INVALID;

    rtc_mutex_lock(&ctx->lock);

    size_t srtp_len = *len - SRTP_AUTH_TAG_LEN;
    uint8_t *tag = buf + srtp_len;

    /* Parse RTP header */
    uint16_t seq = ((uint16_t)buf[2] << 8) | buf[3];
    uint32_t ssrc =
        ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) | buf[11];

    /* Estimate ROC per-SSRC. Use a non-allocating lookup here: a spoofed SSRC
     * must not churn the stream table before the auth tag is verified. */
    rtc_srtp_replay_entry_t *known = srtp_find_ssrc(ctx->rtp_replay, SRTP_REPLAY_MAX_STREAMS, ssrc);
    uint32_t roc = known ? known->roc : 0;
    if (known && known->roc_init && seq < known->last_seq && (known->last_seq - seq) > 0x8000)
        roc = known->roc + 1;

    /* Verify authentication tag */
    uint8_t roc_buf[4];
    roc_buf[0] = (roc >> 24) & 0xFF;
    roc_buf[1] = (roc >> 16) & 0xFF;
    roc_buf[2] = (roc >> 8) & 0xFF;
    roc_buf[3] = roc & 0xFF;

    uint8_t hmac[20];
    int hrc =
        srtp_hmac_sha1(ctx->session_auth_key, 20, buf, srtp_len, roc_buf, 4, hmac, sizeof(hmac));
    if (hrc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return hrc;
    }

    if (memcmp(hmac, tag, SRTP_AUTH_TAG_LEN) != 0) {
        rtc_mutex_unlock(&ctx->lock);
        RTC_LOG_ERR("SRTP: authentication failed");
        return RTC_ERR_SRTP;
    }

    /* Replay check (RFC 3711 §3.3.2) — after auth verify to avoid spoofed
     * replay rejection. Packet index = (ROC << 16) | seq. State is per-SSRC:
     * bundled streams share a key but not a sequence space. */
    uint64_t pkt_index = ((uint64_t)roc << 16) | seq;
    rtc_srtp_replay_entry_t *st = srtp_entry_for_ssrc(ctx->rtp_replay, SRTP_REPLAY_MAX_STREAMS,
                                                      &ctx->replay_lru_tick, ssrc);
    int replay_rc = srtp_replay_check(&st->replay, pkt_index);
    if (replay_rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return replay_rc;
    }

    /* Determine header length */
    size_t hdr_len = 12;
    uint8_t cc = buf[0] & 0x0F;
    hdr_len += (size_t)cc * 4;
    if (buf[0] & 0x10) {
        if (hdr_len + 4 > srtp_len) {
            rtc_mutex_unlock(&ctx->lock);
            return RTC_ERR_INVALID;
        }
        uint16_t ext_len = ((uint16_t)buf[hdr_len + 2] << 8) | buf[hdr_len + 3];
        hdr_len += 4 + (size_t)ext_len * 4;
    }
    if (hdr_len > srtp_len) {
        rtc_mutex_unlock(&ctx->lock);
        return RTC_ERR_INVALID;
    }

    /* Decrypt payload */
    int rc = srtp_aes_cm(ctx->session_key, ctx->session_salt, ssrc, roc, seq, buf + hdr_len,
                         srtp_len - hdr_len);
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return rc;
    }

    /* Update per-SSRC rollover state. */
    if (!st->roc_init || seq > st->last_seq || (st->last_seq - seq) > 0x8000) {
        st->last_seq = seq;
        st->roc = roc;
        st->roc_init = true;
    }

    *len = srtp_len;
    rtc_mutex_unlock(&ctx->lock);
    return RTC_OK;
}

/*
 * SRTCP AES-128-CM (RFC 3711 Section 3.4)
 *
 * IV for RTCP: session_salt XOR (SSRC || srtcp_index), padded to 16 bytes.
 * Unlike SRTP, the "packet index" is the 31-bit SRTCP index (no ROC/seq).
 */
static int srtcp_aes_cm(const uint8_t *session_key, const uint8_t *session_salt, uint32_t ssrc,
                        uint32_t srtcp_index, uint8_t *data, size_t len) {
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));

    /* SSRC at bytes 4..7 */
    iv[4] = (ssrc >> 24) & 0xFF;
    iv[5] = (ssrc >> 16) & 0xFF;
    iv[6] = (ssrc >> 8) & 0xFF;
    iv[7] = ssrc & 0xFF;

    /* SRTCP index at bytes 8..11 (31 bits, no E flag here) */
    iv[8] = (srtcp_index >> 24) & 0xFF;
    iv[9] = (srtcp_index >> 16) & 0xFF;
    iv[10] = (srtcp_index >> 8) & 0xFF;
    iv[11] = srtcp_index & 0xFF;

    for (int i = 0; i < 14; i++)
        iv[i] ^= session_salt[i];

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return RTC_ERR_NOMEM;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, session_key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return RTC_ERR_SRTP;
    }

    int outl = 0;
    EVP_EncryptUpdate(ctx, data, &outl, data, (int)len);
    EVP_EncryptFinal_ex(ctx, data + outl, &outl);
    EVP_CIPHER_CTX_free(ctx);

    return RTC_OK;
}

int rtc_srtp_protect_rtcp(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len, size_t buf_cap) {
    if (!ctx->initialized)
        return RTC_ERR_SRTP;
    if (*len < 8)
        return RTC_ERR_INVALID; /* minimum: RTCP header (4) + SSRC (4) */
    /* SRTCP appends 4-byte index + 10-byte auth tag. */
    if (*len > buf_cap || buf_cap - *len < 4 + SRTP_AUTH_TAG_LEN)
        return RTC_ERR_NOMEM;

    rtc_mutex_lock(&ctx->lock);

    /* Parse SSRC from byte 4..7 of RTCP packet */
    uint32_t ssrc =
        ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];

    uint32_t index = ctx->srtcp_index & 0x7FFFFFFFu;

    /* Encrypt everything after the 8-byte header+SSRC prefix */
    if (*len > 8) {
        int rc = srtcp_aes_cm(ctx->rtcp_session_key, ctx->rtcp_session_salt, ssrc, index, buf + 8,
                              *len - 8);
        if (rc != RTC_OK) {
            rtc_mutex_unlock(&ctx->lock);
            return rc;
        }
    }

    /* Append 4-byte SRTCP index with E-flag (bit 31 = 1 means encrypted) */
    uint32_t e_index = index | 0x80000000u;
    size_t off = *len;
    buf[off + 0] = (e_index >> 24) & 0xFF;
    buf[off + 1] = (e_index >> 16) & 0xFF;
    buf[off + 2] = (e_index >> 8) & 0xFF;
    buf[off + 3] = e_index & 0xFF;
    size_t auth_len = *len + 4; /* RTCP packet + SRTCP index */

    /* Compute auth tag: HMAC-SHA1 over (RTCP packet + SRTCP index) */
    uint8_t hmac[20];
    int rc =
        srtp_hmac_sha1(ctx->rtcp_session_auth_key, 20, buf, auth_len, NULL, 0, hmac, sizeof(hmac));
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return rc;
    }

    /* Append truncated 80-bit auth tag */
    memcpy(buf + auth_len, hmac, SRTP_AUTH_TAG_LEN);
    *len = auth_len + SRTP_AUTH_TAG_LEN;

    ctx->srtcp_index++;
    rtc_mutex_unlock(&ctx->lock);
    return RTC_OK;
}

int rtc_srtp_unprotect_rtcp(rtc_srtp_ctx_t *ctx, uint8_t *buf, size_t *len) {
    if (!ctx->initialized)
        return RTC_ERR_SRTP;
    /* Minimum: 8 (header+SSRC) + 4 (SRTCP index) + 10 (auth tag) */
    if (*len < 8 + 4 + SRTP_AUTH_TAG_LEN)
        return RTC_ERR_INVALID;

    rtc_mutex_lock(&ctx->lock);

    size_t total = *len;
    size_t auth_start = total - SRTP_AUTH_TAG_LEN;
    uint8_t *tag = buf + auth_start;

    /* SRTCP index is the 4 bytes before the auth tag */
    size_t idx_start = auth_start - 4;
    uint32_t e_index = ((uint32_t)buf[idx_start] << 24) | ((uint32_t)buf[idx_start + 1] << 16) |
                       ((uint32_t)buf[idx_start + 2] << 8) | buf[idx_start + 3];
    bool encrypted = (e_index & 0x80000000u) != 0;
    uint32_t index = e_index & 0x7FFFFFFFu;

    /* Verify auth tag over (RTCP packet + SRTCP index) */
    size_t auth_input_len = auth_start; /* everything before the tag */
    uint8_t hmac[20];
    int rc = srtp_hmac_sha1(ctx->rtcp_session_auth_key, 20, buf, auth_input_len, NULL, 0, hmac,
                            sizeof(hmac));
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return rc;
    }

    if (memcmp(hmac, tag, SRTP_AUTH_TAG_LEN) != 0) {
        rtc_mutex_unlock(&ctx->lock);
        RTC_LOG_ERR("SRTCP: authentication failed");
        return RTC_ERR_SRTP;
    }

    /* Replay check (RFC 3711 §3.3.2) — after auth verify. Per-SSRC: the
     * SRTCP index space is independent for each sender. */
    uint32_t sender_ssrc =
        ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    rtc_srtp_replay_entry_t *st = srtp_entry_for_ssrc(ctx->rtcp_replay, SRTP_REPLAY_MAX_STREAMS,
                                                      &ctx->replay_lru_tick, sender_ssrc);
    int replay_rc = srtp_replay_check(&st->replay, (uint64_t)index);
    if (replay_rc != RTC_OK) {
        rtc_mutex_unlock(&ctx->lock);
        return replay_rc;
    }

    /* The plaintext RTCP packet length is everything before the SRTCP index */
    size_t rtcp_len = idx_start;

    /* Decrypt if E-flag set */
    if (encrypted && rtcp_len > 8) {
        uint32_t ssrc =
            ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
        rc = srtcp_aes_cm(ctx->rtcp_session_key, ctx->rtcp_session_salt, ssrc, index, buf + 8,
                          rtcp_len - 8);
        if (rc != RTC_OK) {
            rtc_mutex_unlock(&ctx->lock);
            return rc;
        }
    }

    *len = rtcp_len;
    rtc_mutex_unlock(&ctx->lock);
    return RTC_OK;
}

void rtc_srtp_close(rtc_srtp_ctx_t *ctx) {
    /* Destroy lock only if init ran (otherwise the field is zeroed memory,
     * which is UB to DeleteCriticalSection on Windows). */
    if (ctx->initialized)
        rtc_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}
