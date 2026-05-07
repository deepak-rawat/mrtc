/*
 * DTLS 1.2 (RFC 6347) transport using OpenSSL.
 *
 * Provides the DTLS handshake over the ICE transport, and exports
 * keying material for SRTP.
 */
#ifndef RTC_DTLS_H
#define RTC_DTLS_H

#include "rtc_common.h"
#include <openssl/ssl.h>

#define RTC_DTLS_FINGERPRINT_SIZE 96 /* SHA-256 hex string + colons */
#define RTC_SRTP_MASTER_KEY_LEN   16
#define RTC_SRTP_MASTER_SALT_LEN  14

typedef enum {
    RTC_DTLS_STATE_NEW,
    RTC_DTLS_STATE_CONNECTING,
    RTC_DTLS_STATE_CONNECTED,
    RTC_DTLS_STATE_FAILED,
    RTC_DTLS_STATE_CLOSED,
} rtc_dtls_state_t;

typedef enum {
    RTC_DTLS_ROLE_CLIENT,
    RTC_DTLS_ROLE_SERVER,
} rtc_dtls_role_t;

/* Callback for sending DTLS packets via ICE transport */
typedef int (*rtc_dtls_send_fn)(const uint8_t *data, size_t len, void *user);

typedef struct {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio; /* reading: we write received data here */
    BIO *wbio; /* writing: SSL writes outgoing data here */

    rtc_dtls_state_t state;
    rtc_dtls_role_t role;

    /* Callback to send data */
    rtc_dtls_send_fn send_fn;
    void *send_user;

    /* Local certificate fingerprint (SHA-256) */
    char local_fingerprint[RTC_DTLS_FINGERPRINT_SIZE];

    /* SRTP keying material (populated after handshake) */
    uint8_t srtp_client_key[RTC_SRTP_MASTER_KEY_LEN];
    uint8_t srtp_client_salt[RTC_SRTP_MASTER_SALT_LEN];
    uint8_t srtp_server_key[RTC_SRTP_MASTER_KEY_LEN];
    uint8_t srtp_server_salt[RTC_SRTP_MASTER_SALT_LEN];
    bool srtp_keys_ready;
} rtc_dtls_transport_t;

/* Initialize DTLS transport. Generates a self-signed certificate. */
int rtc_dtls_init(rtc_dtls_transport_t *dtls, rtc_dtls_role_t role, rtc_dtls_send_fn send_fn,
                  void *user);

/* Get the local certificate fingerprint (SHA-256 hex) */
const char *rtc_dtls_get_fingerprint(const rtc_dtls_transport_t *dtls);

/* Start DTLS handshake. Client calls this to initiate. */
int rtc_dtls_handshake(rtc_dtls_transport_t *dtls);

/* Feed received DTLS data. Drives the handshake / decryption. */
int rtc_dtls_recv(rtc_dtls_transport_t *dtls, const uint8_t *data, size_t len);

/* After handshake: export SRTP keying material */
int rtc_dtls_export_srtp_keys(rtc_dtls_transport_t *dtls);

/* Check DTLS retransmission timer. Call periodically during handshake.
 * Retransmits flight if OpenSSL's internal timer has expired. */
int rtc_dtls_retransmit(rtc_dtls_transport_t *dtls);

/* Close and free */
void rtc_dtls_close(rtc_dtls_transport_t *dtls);

#endif /* RTC_DTLS_H */
