/*
 * STUN (RFC 5389) - minimal implementation for ICE candidate gathering.
 *
 * Supports:
 *  - Binding Request / Success Response
 *  - MAPPED-ADDRESS, XOR-MAPPED-ADDRESS attributes
 *  - MESSAGE-INTEGRITY (HMAC-SHA1) and FINGERPRINT
 */
#ifndef RTC_STUN_H
#define RTC_STUN_H

#include "rtc_common.h"

/* STUN message types */
#define STUN_BINDING_REQUEST        0x0001
#define STUN_BINDING_RESPONSE       0x0101
#define STUN_BINDING_ERROR_RESPONSE 0x0111

/* STUN attribute types */
#define STUN_ATTR_MAPPED_ADDRESS     0x0001
#define STUN_ATTR_USERNAME           0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY  0x0008
#define STUN_ATTR_ERROR_CODE         0x0009
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020
#define STUN_ATTR_PRIORITY           0x0024
#define STUN_ATTR_USE_CANDIDATE      0x0025
#define STUN_ATTR_ICE_CONTROLLED     0x8029
#define STUN_ATTR_ICE_CONTROLLING    0x802A
#define STUN_ATTR_FINGERPRINT        0x8028
#define STUN_ATTR_SOFTWARE           0x8022

/* TURN method types (RFC 5766) */
#define STUN_METHOD_ALLOCATE     0x0003
#define STUN_METHOD_REFRESH      0x0004
#define STUN_METHOD_CREATE_PERM  0x0008
#define STUN_METHOD_CHANNEL_BIND 0x0009

/* TURN response types */
#define STUN_ALLOCATE_RESPONSE       0x0103
#define STUN_ALLOCATE_ERROR_RESPONSE 0x0113
#define STUN_REFRESH_RESPONSE        0x0104
#define STUN_CREATE_PERM_RESPONSE    0x0108
#define STUN_CHANNEL_BIND_RESPONSE   0x0109

/* TURN attribute types */
#define STUN_ATTR_CHANNEL_NUMBER      0x000C
#define STUN_ATTR_LIFETIME            0x000D
#define STUN_ATTR_XOR_PEER_ADDRESS    0x0012
#define STUN_ATTR_DATA                0x0013
#define STUN_ATTR_REALM               0x0014
#define STUN_ATTR_NONCE               0x0015
#define STUN_ATTR_XOR_RELAYED_ADDR    0x0016
#define STUN_ATTR_REQUESTED_TRANSPORT 0x0019

#define STUN_MAGIC_COOKIE 0x2112A442
#define STUN_HEADER_SIZE  20
#define STUN_TXN_ID_SIZE  12
#define STUN_MAX_MSG_SIZE 1500

/* STUN message structure */
typedef struct {
    uint16_t type;
    uint16_t length; /* payload length (not including 20-byte header) */
    uint8_t txn_id[STUN_TXN_ID_SIZE];
    uint8_t buf[STUN_MAX_MSG_SIZE]; /* serialized buffer */
    size_t buf_len;                 /* total serialized length */
} rtc_stun_msg_t;

/*
 * Build a STUN Binding Request.
 * If username/password are non-NULL, MESSAGE-INTEGRITY and FINGERPRINT are added.
 */
int rtc_stun_build_binding_request(rtc_stun_msg_t *msg, const char *username, const char *password,
                                   uint32_t priority, bool use_candidate, uint64_t tie_breaker,
                                   bool controlling);

/*
 * Parse a received STUN message.  On success, fills msg->type, txn_id, etc.
 * Returns RTC_OK or error.
 */
int rtc_stun_parse(rtc_stun_msg_t *msg, const uint8_t *data, size_t len);

/*
 * Extract XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS) from a parsed response.
 */
int rtc_stun_get_mapped_address(const rtc_stun_msg_t *msg, rtc_addr_t *addr);

/*
 * Verify MESSAGE-INTEGRITY on a parsed message using a NUL-terminated password.
 * Equivalent to rtc_stun_verify_integrity_key(data, len, password, strlen(password)).
 * Do NOT use with binary keys that may contain NUL bytes (e.g. TURN long-term
 * keys, which are 16-byte MD5 digests).
 */
int rtc_stun_verify_integrity(const uint8_t *data, size_t len, const char *password);

/*
 * Verify MESSAGE-INTEGRITY on a parsed message using a raw byte key.
 * Required for binary keys such as TURN long-term credentials (RFC 5389 §10.2:
 * key = MD5(username ":" realm ":" password), 16 raw bytes).
 */
int rtc_stun_verify_integrity_key(const uint8_t *data, size_t len, const uint8_t *key,
                                  size_t key_len);

/*
 * Build a STUN message with given method type and optional attributes.
 * Generic builder for TURN methods (Allocate, Refresh, CreatePermission, ChannelBind).
 * If username/password provided, adds MESSAGE-INTEGRITY + FINGERPRINT.
 */
int rtc_stun_build_request(rtc_stun_msg_t *msg, uint16_t method, const char *username,
                           const char *password);

/* Add REQUESTED-TRANSPORT attribute (for Allocate: 17 = UDP) */
int rtc_stun_add_requested_transport(rtc_stun_msg_t *msg, uint8_t protocol);

/* Add LIFETIME attribute (seconds) */
int rtc_stun_add_lifetime(rtc_stun_msg_t *msg, uint32_t seconds);

/* Add XOR-PEER-ADDRESS attribute */
int rtc_stun_add_xor_peer_address(rtc_stun_msg_t *msg, const rtc_addr_t *peer);

/* Add CHANNEL-NUMBER attribute */
int rtc_stun_add_channel_number(rtc_stun_msg_t *msg, uint16_t channel);

/* Add USERNAME attribute */
int rtc_stun_add_username(rtc_stun_msg_t *msg, const char *username);

/* Add REALM attribute */
int rtc_stun_add_realm(rtc_stun_msg_t *msg, const char *realm);

/* Add NONCE attribute */
int rtc_stun_add_nonce(rtc_stun_msg_t *msg, const char *nonce);

/* Finalize a message built with rtc_stun_build_request (adds integrity + fingerprint).
 * Uses a NUL-terminated password as the HMAC key.  Do NOT use with binary keys
 * that may contain NUL bytes — use rtc_stun_finalize_key() instead. */
int rtc_stun_finalize(rtc_stun_msg_t *msg, const char *password);

/* Finalize a message using a raw byte key (e.g. TURN long-term credentials,
 * which are a 16-byte MD5 digest that may contain NUL bytes). */
int rtc_stun_finalize_key(rtc_stun_msg_t *msg, const uint8_t *key, size_t key_len);

/* Extract a specific attribute from a parsed message. Returns pointer + length, or NULL. */
const uint8_t *rtc_stun_find_attr(const rtc_stun_msg_t *msg, uint16_t attr_type, uint16_t *out_len);

/* Extract XOR-RELAYED-ADDRESS from a parsed TURN Allocate response */
int rtc_stun_get_relayed_address(const rtc_stun_msg_t *msg, rtc_addr_t *addr);

/* Extract LIFETIME from a parsed TURN response */
int rtc_stun_get_lifetime(const rtc_stun_msg_t *msg, uint32_t *seconds);

/* Extract ERROR-CODE from a parsed STUN error response */
int rtc_stun_get_error_code(const rtc_stun_msg_t *msg, int *code);

/* Extract REALM from a parsed message */
int rtc_stun_get_realm(const rtc_stun_msg_t *msg, char *buf, size_t buflen);

/* Extract NONCE from a parsed message */
int rtc_stun_get_nonce(const rtc_stun_msg_t *msg, char *buf, size_t buflen);

/* Compute long-term credential key: MD5(username:realm:password) */
int rtc_stun_long_term_key(const char *username, const char *realm, const char *password,
                           uint8_t key[16]);

/* ---- ChannelData framing (not STUN, but TURN-related) ---- */
#define TURN_CHANNEL_DATA_HEADER 4
#define TURN_CHANNEL_MIN         0x4000
#define TURN_CHANNEL_MAX         0x7FFF

/* Build a ChannelData frame: [channel:2][length:2][data:len] */
int rtc_turn_build_channel_data(uint8_t *buf, size_t buflen, uint16_t channel, const uint8_t *data,
                                size_t len, size_t *out_len);

/* Parse a ChannelData frame */
int rtc_turn_parse_channel_data(const uint8_t *buf, size_t buflen, uint16_t *channel,
                                const uint8_t **data, size_t *data_len);

/*
 * Simple blocking STUN binding request to a server.
 * Returns the server-reflexive address in *mapped.
 */
int rtc_stun_binding(const char *server_ip, uint16_t server_port, rtc_socket_t sock,
                     rtc_addr_t *mapped);

#endif /* RTC_STUN_H */
