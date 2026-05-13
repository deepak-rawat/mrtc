/*
 * rtc_stun.c - STUN (RFC 5389) minimal implementation.
 *
 * Supports Binding Request/Response, MESSAGE-INTEGRITY, FINGERPRINT,
 * XOR-MAPPED-ADDRESS, and ICE-specific attributes.
 */
#include "rtc/rtc_stun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>

/* Compute HMAC-SHA1 using modern EVP_MAC API. Key is treated as a raw byte
 * buffer of length key_len — it is NOT NUL-terminated and may contain zero
 * bytes (required for TURN long-term credentials, RFC 5389 §10.2). */
static int stun_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                          uint8_t *out, size_t *out_len) {
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac)
        return RTC_ERR_GENERIC;

    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!ctx)
        return RTC_ERR_GENERIC;

    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, "SHA1", 0),
                           OSSL_PARAM_construct_end()};

    int ret = RTC_ERR_GENERIC;
    if (EVP_MAC_init(ctx, key, key_len, params) && EVP_MAC_update(ctx, data, data_len) &&
        EVP_MAC_final(ctx, out, out_len, *out_len))
        ret = RTC_OK;

    EVP_MAC_CTX_free(ctx);
    return ret;
}

/* CRC-32 lookup table for FINGERPRINT (IEEE 802.3, reflected, poly 0xEDB88320).
 * Precomputed at compile time to avoid double-checked-init races (RFC 5389
 * §15.5 FINGERPRINT is computed on every STUN message). */
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

/* Write big-endian helpers */
static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}
static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}
static void write_u64(uint8_t *p, uint64_t v) {
    write_u32(p, (uint32_t)(v >> 32));
    write_u32(p + 4, (uint32_t)v);
}

static uint16_t read_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}
static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Append an attribute to the buffer */
static int append_attr(uint8_t *buf, size_t *pos, size_t maxlen, uint16_t type,
                       const uint8_t *value, uint16_t vlen) {
    /* Padded length (attributes must be 4-byte aligned) */
    uint16_t padded = (vlen + 3) & ~3;
    if (*pos + 4 + padded > maxlen)
        return RTC_ERR_NOMEM;

    write_u16(buf + *pos, type);
    *pos += 2;
    write_u16(buf + *pos, vlen);
    *pos += 2;
    if (vlen > 0)
        memcpy(buf + *pos, value, vlen);
    /* Zero padding */
    for (size_t i = vlen; i < padded; i++)
        buf[*pos + i] = 0;
    *pos += padded;
    return RTC_OK;
}

int rtc_stun_build_binding_request(rtc_stun_msg_t *msg, const char *username, const char *password,
                                   uint32_t priority, bool use_candidate, uint64_t tie_breaker,
                                   bool controlling) {
    memset(msg, 0, sizeof(*msg));
    msg->type = STUN_BINDING_REQUEST;

    /* Generate transaction ID */
    rtc_random_bytes(msg->txn_id, STUN_TXN_ID_SIZE);

    /* Build header */
    uint8_t *buf = msg->buf;
    size_t pos = 0;

    /* Leave header space, fill later */
    pos = STUN_HEADER_SIZE;

    /* USERNAME (if ICE) */
    if (username) {
        size_t ulen = strlen(username);
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_USERNAME, (const uint8_t *)username,
                        (uint16_t)ulen) != RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* PRIORITY */
    if (priority > 0) {
        uint8_t pval[4];
        write_u32(pval, priority);
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_PRIORITY, pval, 4) != RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* USE-CANDIDATE */
    if (use_candidate) {
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_USE_CANDIDATE, NULL, 0) != RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* ICE-CONTROLLING or ICE-CONTROLLED */
    if (tie_breaker > 0) {
        uint8_t tb[8];
        write_u64(tb, tie_breaker);
        uint16_t attr = controlling ? STUN_ATTR_ICE_CONTROLLING : STUN_ATTR_ICE_CONTROLLED;
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, attr, tb, 8) != RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* MESSAGE-INTEGRITY */
    if (password) {
        /* Update header with length up to and including MESSAGE-INTEGRITY */
        size_t mi_msg_len = pos - STUN_HEADER_SIZE + 24; /* 4 attr header + 20 HMAC */
        write_u16(buf + 0, STUN_BINDING_REQUEST);
        write_u16(buf + 2, (uint16_t)mi_msg_len);
        write_u32(buf + 4, STUN_MAGIC_COOKIE);
        memcpy(buf + 8, msg->txn_id, STUN_TXN_ID_SIZE);

        /* Compute HMAC-SHA1 */
        size_t hmac_len = 20;
        uint8_t hmac[20];
        stun_hmac_sha1((const uint8_t *)password, strlen(password), buf, pos, hmac, &hmac_len);

        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_MESSAGE_INTEGRITY, hmac, 20) !=
            RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* FINGERPRINT */
    {
        /* Update header with length up to and including FINGERPRINT */
        size_t fp_msg_len = pos - STUN_HEADER_SIZE + 8; /* 4 attr header + 4 CRC */
        write_u16(buf + 0, STUN_BINDING_REQUEST);
        write_u16(buf + 2, (uint16_t)fp_msg_len);
        write_u32(buf + 4, STUN_MAGIC_COOKIE);
        memcpy(buf + 8, msg->txn_id, STUN_TXN_ID_SIZE);

        uint32_t crc = crc32_calc(buf, pos) ^ 0x5354554E; /* XOR with "STUN" magic */
        uint8_t fp[4];
        write_u32(fp, crc);
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_FINGERPRINT, fp, 4) != RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* Final header */
    msg->length = (uint16_t)(pos - STUN_HEADER_SIZE);
    write_u16(buf + 0, msg->type);
    write_u16(buf + 2, msg->length);
    write_u32(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, msg->txn_id, STUN_TXN_ID_SIZE);

    msg->buf_len = pos;
    return RTC_OK;
}

int rtc_stun_parse(rtc_stun_msg_t *msg, const uint8_t *data, size_t len) {
    if (len < STUN_HEADER_SIZE)
        return RTC_ERR_INVALID;

    msg->type = read_u16(data);
    msg->length = read_u16(data + 2);
    uint32_t cookie = read_u32(data + 4);
    if (cookie != STUN_MAGIC_COOKIE)
        return RTC_ERR_INVALID;

    memcpy(msg->txn_id, data + 8, STUN_TXN_ID_SIZE);

    if ((size_t)(STUN_HEADER_SIZE + msg->length) > len)
        return RTC_ERR_INVALID;

    /* Copy raw buffer for attribute extraction */
    size_t total = STUN_HEADER_SIZE + msg->length;
    if (total > STUN_MAX_MSG_SIZE)
        total = STUN_MAX_MSG_SIZE;
    memcpy(msg->buf, data, total);
    msg->buf_len = total;

    return RTC_OK;
}

int rtc_stun_get_mapped_address(const rtc_stun_msg_t *msg, rtc_addr_t *addr) {
    const uint8_t *buf = msg->buf;
    size_t pos = STUN_HEADER_SIZE;
    size_t end = msg->buf_len;

    while (pos + 4 <= end) {
        uint16_t atype = read_u16(buf + pos);
        uint16_t alen = read_u16(buf + pos + 2);
        uint16_t padded = (alen + 3) & ~3;
        pos += 4;

        if (pos + alen > end)
            break;

        if (atype == STUN_ATTR_XOR_MAPPED_ADDRESS || atype == STUN_ATTR_MAPPED_ADDRESS) {
            /* Minimum is 8 bytes (IPv4: 1 reserved + 1 family + 2 port + 4 addr).
             * IPv6 requires 20 bytes (1 + 1 + 2 + 16). Validate family-specific
             * length before reading the address bytes to prevent OOB read. */
            if (alen < 8)
                return RTC_ERR_INVALID;

            uint8_t family = buf[pos + 1];
            uint16_t port = read_u16(buf + pos + 2);
            memset(addr, 0, sizeof(*addr));

            if (atype == STUN_ATTR_XOR_MAPPED_ADDRESS) {
                port ^= (STUN_MAGIC_COOKIE >> 16) & 0xFFFF;
            }

            if (family == 0x01) { /* IPv4 */
                struct sockaddr_in *sin = (struct sockaddr_in *)&addr->addr;
                sin->sin_family = AF_INET;
                sin->sin_port = htons(port);
                memcpy(&sin->sin_addr, buf + pos + 4, 4);
                if (atype == STUN_ATTR_XOR_MAPPED_ADDRESS) {
                    uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    ip ^= STUN_MAGIC_COOKIE;
                    sin->sin_addr.s_addr = htonl(ip);
                }
                addr->len = sizeof(struct sockaddr_in);
            } else if (family == 0x02) { /* IPv6 */
                if (alen < 20)
                    return RTC_ERR_INVALID;
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr->addr;
                sin6->sin6_family = AF_INET6;
                sin6->sin6_port = htons(port);
                memcpy(&sin6->sin6_addr, buf + pos + 4, 16);
                if (atype == STUN_ATTR_XOR_MAPPED_ADDRESS) {
                    uint8_t *a = sin6->sin6_addr.s6_addr;
                    uint8_t xor_key[16];
                    write_u32(xor_key, STUN_MAGIC_COOKIE);
                    memcpy(xor_key + 4, msg->txn_id, 12);
                    for (int i = 0; i < 16; i++)
                        a[i] ^= xor_key[i];
                }
                addr->len = sizeof(struct sockaddr_in6);
            } else {
                return RTC_ERR_INVALID;
            }
            return RTC_OK;
        }
        pos += padded;
    }
    return RTC_ERR_INVALID; /* attribute not found */
}

int rtc_stun_verify_integrity_key(const uint8_t *data, size_t len, const uint8_t *key,
                                  size_t key_len) {
    /* Find MESSAGE-INTEGRITY attribute */
    if (len < STUN_HEADER_SIZE + 24)
        return RTC_ERR_INVALID;

    size_t pos = STUN_HEADER_SIZE;
    size_t msg_len_claim = read_u16(data + 2);
    size_t end = STUN_HEADER_SIZE + msg_len_claim;
    if (end > len)
        end = len;

    while (pos + 4 <= end) {
        uint16_t atype = read_u16(data + pos);
        uint16_t alen = read_u16(data + pos + 2);
        uint16_t padded = (alen + 3) & ~3;

        if (atype == STUN_ATTR_MESSAGE_INTEGRITY) {
            if (alen != 20)
                return RTC_ERR_INVALID;

            /* The HMAC is computed over the message up to (but not including) the MI attr,
               with the length field adjusted to include the MI attr. */
            uint8_t tmp[STUN_MAX_MSG_SIZE];
            size_t hmac_input_len = pos; /* everything before this attribute */
            if (hmac_input_len > sizeof(tmp))
                return RTC_ERR_INVALID;
            memcpy(tmp, data, hmac_input_len);

            /* Adjust length: from header, length = (pos - 20) + 24 */
            uint16_t adj_len = (uint16_t)(pos - STUN_HEADER_SIZE + 24);
            write_u16(tmp + 2, adj_len);

            size_t hmac_len = 20;
            uint8_t computed[20];
            stun_hmac_sha1(key, key_len, tmp, hmac_input_len, computed, &hmac_len);

            if (memcmp(computed, data + pos + 4, 20) == 0)
                return RTC_OK;
            else
                return RTC_ERR_INVALID;
        }
        pos += 4 + padded;
    }
    return RTC_ERR_INVALID; /* MI not found */
}

int rtc_stun_verify_integrity(const uint8_t *data, size_t len, const char *password) {
    if (!password)
        return RTC_ERR_INVALID;
    return rtc_stun_verify_integrity_key(data, len, (const uint8_t *)password, strlen(password));
}

int rtc_stun_binding(const char *server_ip, uint16_t server_port, rtc_socket_t sock,
                     rtc_addr_t *mapped) {
    rtc_stun_msg_t req, resp;

    /* Build simple binding request (no ICE attributes) */
    int rc = rtc_stun_build_binding_request(&req, NULL, NULL, 0, false, 0, false);
    if (rc != RTC_OK)
        return rc;

    /* Destination */
    rtc_addr_t dest;
    rc = rtc_addr_from_string(&dest, server_ip, server_port);
    if (rc != RTC_OK)
        return rc;

    /* Send */
    ssize_t sent = sendto(sock, (const char *)req.buf, (int)req.buf_len, 0,
                          (struct sockaddr *)&dest.addr, dest.len);
    if (sent < 0) {
        RTC_LOG_ERR("STUN sendto failed");
        return RTC_ERR_SOCKET;
    }

    /* Wait for response (with timeout) */
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec = 3;
    tv.tv_usec = 0;

    int sel = select((int)sock + 1, &fds, NULL, NULL, &tv);
    if (sel <= 0) {
        RTC_LOG_ERR("STUN timeout");
        return RTC_ERR_TIMEOUT;
    }

    uint8_t buf[STUN_MAX_MSG_SIZE];
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    if (n < STUN_HEADER_SIZE)
        return RTC_ERR_SOCKET;

    rc = rtc_stun_parse(&resp, buf, (size_t)n);
    if (rc != RTC_OK)
        return rc;

    if (resp.type != STUN_BINDING_RESPONSE) {
        RTC_LOG_ERR("STUN: unexpected response type 0x%04x", resp.type);
        return RTC_ERR_INVALID;
    }

    /* Check transaction ID matches */
    if (memcmp(resp.txn_id, req.txn_id, STUN_TXN_ID_SIZE) != 0) {
        RTC_LOG_ERR("STUN: transaction ID mismatch");
        return RTC_ERR_INVALID;
    }

    return rtc_stun_get_mapped_address(&resp, mapped);
}

/* ================================================================== */
/*  TURN extensions (RFC 5766)                                        */
/* ================================================================== */

int rtc_stun_build_request(rtc_stun_msg_t *msg, uint16_t method, const char *username,
                           const char *password) {
    (void)username;
    (void)password;
    memset(msg, 0, sizeof(*msg));
    msg->type = method;
    rtc_random_bytes(msg->txn_id, STUN_TXN_ID_SIZE);

    /* Write header placeholder */
    uint8_t *buf = msg->buf;
    write_u16(buf + 0, method);
    write_u16(buf + 2, 0);
    write_u32(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, msg->txn_id, STUN_TXN_ID_SIZE);
    msg->buf_len = STUN_HEADER_SIZE;
    msg->length = 0;
    return RTC_OK;
}

/* Helper to append attr to a message being built */
static int msg_append_attr(rtc_stun_msg_t *msg, uint16_t type, const uint8_t *value,
                           uint16_t vlen) {
    return append_attr(msg->buf, &msg->buf_len, STUN_MAX_MSG_SIZE, type, value, vlen);
}

static void msg_update_header(rtc_stun_msg_t *msg) {
    msg->length = (uint16_t)(msg->buf_len - STUN_HEADER_SIZE);
    write_u16(msg->buf + 2, msg->length);
}

int rtc_stun_add_requested_transport(rtc_stun_msg_t *msg, uint8_t protocol) {
    uint8_t val[4] = {protocol, 0, 0, 0};
    int rc = msg_append_attr(msg, STUN_ATTR_REQUESTED_TRANSPORT, val, 4);
    if (rc == RTC_OK)
        msg_update_header(msg);
    return rc;
}

int rtc_stun_add_lifetime(rtc_stun_msg_t *msg, uint32_t seconds) {
    uint8_t val[4];
    write_u32(val, seconds);
    int rc = msg_append_attr(msg, STUN_ATTR_LIFETIME, val, 4);
    if (rc == RTC_OK)
        msg_update_header(msg);
    return rc;
}

int rtc_stun_add_xor_peer_address(rtc_stun_msg_t *msg, const rtc_addr_t *peer) {
    uint8_t val[20];
    const struct sockaddr *sa = (const struct sockaddr *)&peer->addr;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        val[0] = 0;
        val[1] = 0x01; /* IPv4 */
        uint16_t port = ntohs(sin->sin_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
        write_u16(val + 2, port);
        uint32_t ip = ntohl(sin->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;
        write_u32(val + 4, ip);
        int rc = msg_append_attr(msg, STUN_ATTR_XOR_PEER_ADDRESS, val, 8);
        if (rc == RTC_OK)
            msg_update_header(msg);
        return rc;
    }
    return RTC_ERR_INVALID;
}

int rtc_stun_add_channel_number(rtc_stun_msg_t *msg, uint16_t channel) {
    uint8_t val[4];
    write_u16(val, channel);
    write_u16(val + 2, 0); /* RFFU */
    int rc = msg_append_attr(msg, STUN_ATTR_CHANNEL_NUMBER, val, 4);
    if (rc == RTC_OK)
        msg_update_header(msg);
    return rc;
}

int rtc_stun_add_username(rtc_stun_msg_t *msg, const char *username) {
    int rc = msg_append_attr(msg, STUN_ATTR_USERNAME, (const uint8_t *)username,
                             (uint16_t)strlen(username));
    if (rc == RTC_OK)
        msg_update_header(msg);
    return rc;
}

int rtc_stun_add_realm(rtc_stun_msg_t *msg, const char *realm) {
    int rc = msg_append_attr(msg, STUN_ATTR_REALM, (const uint8_t *)realm, (uint16_t)strlen(realm));
    if (rc == RTC_OK)
        msg_update_header(msg);
    return rc;
}

int rtc_stun_add_nonce(rtc_stun_msg_t *msg, const char *nonce) {
    int rc = msg_append_attr(msg, STUN_ATTR_NONCE, (const uint8_t *)nonce, (uint16_t)strlen(nonce));
    if (rc == RTC_OK)
        msg_update_header(msg);
    return rc;
}

int rtc_stun_finalize_key(rtc_stun_msg_t *msg, const uint8_t *key, size_t key_len) {
    uint8_t *buf = msg->buf;
    size_t pos = msg->buf_len;

    if (key && key_len > 0) {
        /* Update header length to include MI */
        uint16_t mi_len = (uint16_t)(pos - STUN_HEADER_SIZE + 24);
        write_u16(buf + 2, mi_len);

        size_t hmac_len = 20;
        uint8_t hmac[20];
        stun_hmac_sha1(key, key_len, buf, pos, hmac, &hmac_len);
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_MESSAGE_INTEGRITY, hmac, 20) !=
            RTC_OK)
            return RTC_ERR_NOMEM;
    }

    /* FINGERPRINT */
    {
        uint16_t fp_len = (uint16_t)(pos - STUN_HEADER_SIZE + 8);
        write_u16(buf + 2, fp_len);
        uint32_t crc = crc32_calc(buf, pos) ^ 0x5354554E;
        uint8_t fp[4];
        write_u32(fp, crc);
        if (append_attr(buf, &pos, STUN_MAX_MSG_SIZE, STUN_ATTR_FINGERPRINT, fp, 4) != RTC_OK)
            return RTC_ERR_NOMEM;
    }

    msg->buf_len = pos;
    msg->length = (uint16_t)(pos - STUN_HEADER_SIZE);
    write_u16(buf + 0, msg->type);
    write_u16(buf + 2, msg->length);
    return RTC_OK;
}

int rtc_stun_finalize(rtc_stun_msg_t *msg, const char *password) {
    if (password)
        return rtc_stun_finalize_key(msg, (const uint8_t *)password, strlen(password));
    return rtc_stun_finalize_key(msg, NULL, 0);
}

const uint8_t *rtc_stun_find_attr(const rtc_stun_msg_t *msg, uint16_t attr_type,
                                  uint16_t *out_len) {
    const uint8_t *buf = msg->buf;
    size_t pos = STUN_HEADER_SIZE;
    size_t end = msg->buf_len;

    while (pos + 4 <= end) {
        uint16_t atype = read_u16(buf + pos);
        uint16_t alen = read_u16(buf + pos + 2);
        uint16_t padded = (alen + 3) & ~3;
        if (pos + 4 + alen > end)
            break;
        if (atype == attr_type) {
            if (out_len)
                *out_len = alen;
            return buf + pos + 4;
        }
        pos += 4 + padded;
    }
    return NULL;
}

int rtc_stun_get_relayed_address(const rtc_stun_msg_t *msg, rtc_addr_t *addr) {
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(msg, STUN_ATTR_XOR_RELAYED_ADDR, &alen);
    if (!val || alen < 8)
        return RTC_ERR_INVALID;

    memset(addr, 0, sizeof(*addr));
    uint8_t family = val[1];
    uint16_t port = read_u16(val + 2) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);

    if (family == 0x01) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&addr->addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        uint32_t ip = read_u32(val + 4) ^ STUN_MAGIC_COOKIE;
        sin->sin_addr.s_addr = htonl(ip);
        addr->len = sizeof(struct sockaddr_in);
        return RTC_OK;
    }
    return RTC_ERR_INVALID;
}

int rtc_stun_get_lifetime(const rtc_stun_msg_t *msg, uint32_t *seconds) {
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(msg, STUN_ATTR_LIFETIME, &alen);
    if (!val || alen < 4)
        return RTC_ERR_INVALID;
    *seconds = read_u32(val);
    return RTC_OK;
}

int rtc_stun_get_error_code(const rtc_stun_msg_t *msg, int *code) {
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(msg, STUN_ATTR_ERROR_CODE, &alen);
    if (!val || alen < 4)
        return RTC_ERR_INVALID;
    int class_num = val[2] & 0x07;
    int number = val[3];
    *code = class_num * 100 + number;
    return RTC_OK;
}

int rtc_stun_get_realm(const rtc_stun_msg_t *msg, char *buf, size_t buflen) {
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(msg, STUN_ATTR_REALM, &alen);
    if (!val)
        return RTC_ERR_INVALID;
    size_t n = alen < buflen - 1 ? alen : buflen - 1;
    memcpy(buf, val, n);
    buf[n] = '\0';
    return RTC_OK;
}

int rtc_stun_get_nonce(const rtc_stun_msg_t *msg, char *buf, size_t buflen) {
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(msg, STUN_ATTR_NONCE, &alen);
    if (!val)
        return RTC_ERR_INVALID;
    size_t n = alen < buflen - 1 ? alen : buflen - 1;
    memcpy(buf, val, n);
    buf[n] = '\0';
    return RTC_OK;
}

int rtc_stun_long_term_key(const char *username, const char *realm, const char *password,
                           uint8_t key[16]) {
    /* key = MD5(username:realm:password) */
    char input[512];
    int n = snprintf(input, sizeof(input), "%s:%s:%s", username, realm, password);
    if (n <= 0 || (size_t)n >= sizeof(input))
        return RTC_ERR_INVALID;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return RTC_ERR_GENERIC;

    unsigned int md_len = 16;
    int ok = EVP_DigestInit_ex(ctx, EVP_md5(), NULL) && EVP_DigestUpdate(ctx, input, (size_t)n) &&
             EVP_DigestFinal_ex(ctx, key, &md_len);
    EVP_MD_CTX_free(ctx);
    return ok ? RTC_OK : RTC_ERR_GENERIC;
}

/* ---- ChannelData framing ---- */

int rtc_turn_build_channel_data(uint8_t *buf, size_t buflen, uint16_t channel, const uint8_t *data,
                                size_t len, size_t *out_len) {
    size_t padded = (len + 3) & ~(size_t)3;
    if (TURN_CHANNEL_DATA_HEADER + padded > buflen)
        return RTC_ERR_NOMEM;
    if (channel < TURN_CHANNEL_MIN || channel > TURN_CHANNEL_MAX)
        return RTC_ERR_INVALID;

    write_u16(buf, channel);
    write_u16(buf + 2, (uint16_t)len);
    memcpy(buf + 4, data, len);
    /* Zero padding */
    for (size_t i = len; i < padded; i++)
        buf[4 + i] = 0;
    *out_len = TURN_CHANNEL_DATA_HEADER + padded;
    return RTC_OK;
}

int rtc_turn_parse_channel_data(const uint8_t *buf, size_t buflen, uint16_t *channel,
                                const uint8_t **data, size_t *data_len) {
    if (buflen < TURN_CHANNEL_DATA_HEADER)
        return RTC_ERR_INVALID;
    *channel = read_u16(buf);
    *data_len = read_u16(buf + 2);
    if (*channel < TURN_CHANNEL_MIN || *channel > TURN_CHANNEL_MAX)
        return RTC_ERR_INVALID;
    if (TURN_CHANNEL_DATA_HEADER + *data_len > buflen)
        return RTC_ERR_INVALID;
    *data = buf + TURN_CHANNEL_DATA_HEADER;
    return RTC_OK;
}
