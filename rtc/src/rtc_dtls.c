/*
 * DTLS 1.2 transport using OpenSSL.
 *
 * Generates a self-signed ECDSA certificate, performs DTLS handshake
 * over UDP via BIO memory pair, and exports SRTP keying material.
 */
#include "rtc_dtls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/evp.h>

/* Generate a self-signed ECDSA certificate + key */
static int dtls_generate_cert(X509 **cert_out, EVP_PKEY **pkey_out, char *fp_out, size_t fp_size) {
    EVP_PKEY *pkey = NULL;
    X509 *x509 = NULL;
    int ret = RTC_ERR_SSL;

    if (!cert_out || !pkey_out || !fp_out || fp_size == 0)
        return RTC_ERR_INVALID;

    /* Generate EC key (P-256) */
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!pctx)
        goto fail;
    if (EVP_PKEY_keygen_init(pctx) <= 0)
        goto fail;
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0)
        goto fail;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0)
        goto fail;
    EVP_PKEY_CTX_free(pctx);
    pctx = NULL;

    /* Create self-signed X509 */
    x509 = X509_new();
    if (!x509)
        goto fail;
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"mrtc", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    /* Compute SHA-256 fingerprint */
    unsigned int flen = 0;
    uint8_t digest[EVP_MAX_MD_SIZE];
    if (!X509_digest(x509, EVP_sha256(), digest, &flen))
        goto fail;

    /* Format as colon-separated hex */
    size_t offset = 0;
    for (unsigned int i = 0; i < flen; i++) {
        if (i > 0 && offset < fp_size - 1)
            fp_out[offset++] = ':';
        int written = snprintf(fp_out + offset, fp_size - offset, "%02X", digest[i]);
        if (written > 0)
            offset += (size_t)written;
    }
    fp_out[offset] = '\0';

    ret = RTC_OK;
    *cert_out = x509;
    *pkey_out = pkey;
    x509 = NULL;
    pkey = NULL;

fail:
    if (pctx)
        EVP_PKEY_CTX_free(pctx);
    if (x509)
        X509_free(x509);
    if (pkey)
        EVP_PKEY_free(pkey);
    return ret;
}

static void dtls_close_ssl(rtc_dtls_transport_t *dtls) {
    if (dtls->ssl) {
        SSL_shutdown(dtls->ssl);
        SSL_free(dtls->ssl); /* also frees rbio and wbio */
        dtls->ssl = NULL;
        dtls->rbio = NULL;
        dtls->wbio = NULL;
    }
    if (dtls->ctx) {
        SSL_CTX_free(dtls->ctx);
        dtls->ctx = NULL;
    }
}

/* OpenSSL verify callback - accept any peer cert (we verify fingerprint separately) */
static int dtls_verify_cb(int preverify_ok, X509_STORE_CTX *ctx) {
    (void)preverify_ok;
    (void)ctx;
    return 1; /* always accept */
}

static int dtls_setup_ssl(rtc_dtls_transport_t *dtls, rtc_dtls_role_t role) {
    if (!dtls || !dtls->cert || !dtls->pkey)
        return RTC_ERR_INVALID;

    const SSL_METHOD *method =
        (role == RTC_DTLS_ROLE_CLIENT) ? DTLS_client_method() : DTLS_server_method();
    dtls->ctx = SSL_CTX_new(method);
    if (!dtls->ctx) {
        RTC_LOG_ERR("DTLS: SSL_CTX_new failed");
        return RTC_ERR_SSL;
    }

    if (SSL_CTX_set_tlsext_use_srtp(dtls->ctx, "SRTP_AES128_CM_SHA1_80") != 0) {
        RTC_LOG_ERR("DTLS: failed to set SRTP profile");
        dtls_close_ssl(dtls);
        return RTC_ERR_SSL;
    }

    SSL_CTX_set_verify(dtls->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       dtls_verify_cb);

    if (SSL_CTX_use_certificate(dtls->ctx, dtls->cert) != 1 ||
        SSL_CTX_use_PrivateKey(dtls->ctx, dtls->pkey) != 1 ||
        SSL_CTX_check_private_key(dtls->ctx) != 1) {
        RTC_LOG_ERR("DTLS: failed to install certificate");
        dtls_close_ssl(dtls);
        return RTC_ERR_SSL;
    }

    dtls->ssl = SSL_new(dtls->ctx);
    if (!dtls->ssl) {
        dtls_close_ssl(dtls);
        return RTC_ERR_SSL;
    }

    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        if (rbio)
            BIO_free(rbio);
        if (wbio)
            BIO_free(wbio);
        dtls_close_ssl(dtls);
        return RTC_ERR_SSL;
    }

    BIO_set_mem_eof_return(rbio, -1);
    BIO_set_mem_eof_return(wbio, -1);
    SSL_set_bio(dtls->ssl, rbio, wbio);
    dtls->rbio = rbio;
    dtls->wbio = wbio;

    if (role == RTC_DTLS_ROLE_CLIENT)
        SSL_set_connect_state(dtls->ssl);
    else
        SSL_set_accept_state(dtls->ssl);

    dtls->state = RTC_DTLS_STATE_NEW;
    dtls->role = role;
    dtls->srtp_keys_ready = false;
    memset(dtls->srtp_client_key, 0, sizeof(dtls->srtp_client_key));
    memset(dtls->srtp_client_salt, 0, sizeof(dtls->srtp_client_salt));
    memset(dtls->srtp_server_key, 0, sizeof(dtls->srtp_server_key));
    memset(dtls->srtp_server_salt, 0, sizeof(dtls->srtp_server_salt));
    return RTC_OK;
}

int rtc_dtls_init(rtc_dtls_transport_t *dtls, rtc_dtls_role_t role, rtc_dtls_send_fn send_fn,
                  void *user) {
    memset(dtls, 0, sizeof(*dtls));
    dtls->send_fn = send_fn;
    dtls->send_user = user;

    /* Generate certificate */
    int rc = dtls_generate_cert(&dtls->cert, &dtls->pkey, dtls->local_fingerprint,
                                sizeof(dtls->local_fingerprint));
    if (rc != RTC_OK)
        return rc;

    rc = dtls_setup_ssl(dtls, role);
    if (rc != RTC_OK) {
        rtc_dtls_close(dtls);
        return rc;
    }

    RTC_LOG_INFO("DTLS: initialized (role=%s, fingerprint=%s)",
                 role == RTC_DTLS_ROLE_CLIENT ? "client" : "server", dtls->local_fingerprint);
    return RTC_OK;
}

int rtc_dtls_set_role(rtc_dtls_transport_t *dtls, rtc_dtls_role_t role) {
    if (!dtls || !dtls->cert || !dtls->pkey)
        return RTC_ERR_INVALID;
    if (dtls->state != RTC_DTLS_STATE_NEW)
        return RTC_ERR_INVALID;
    if (dtls->role == role)
        return RTC_OK;

    dtls_close_ssl(dtls);
    int rc = dtls_setup_ssl(dtls, role);
    if (rc != RTC_OK) {
        dtls->state = RTC_DTLS_STATE_FAILED;
        return rc;
    }
    RTC_LOG_INFO("DTLS: role changed to %s (fingerprint=%s)",
                 role == RTC_DTLS_ROLE_CLIENT ? "client" : "server", dtls->local_fingerprint);
    return RTC_OK;
}

const char *rtc_dtls_get_fingerprint(const rtc_dtls_transport_t *dtls) {
    return dtls->local_fingerprint;
}

/* Flush outgoing BIO data via the send callback */
static int dtls_flush_wbio(rtc_dtls_transport_t *dtls) {
    uint8_t buf[2048];
    int pending;
    while ((pending = BIO_ctrl_pending(dtls->wbio)) > 0) {
        int n = BIO_read(dtls->wbio, buf, (int)sizeof(buf));
        if (n <= 0)
            break;
        int rc = dtls->send_fn(buf, (size_t)n, dtls->send_user);
        if (rc != RTC_OK)
            return rc;
    }
    return RTC_OK;
}

int rtc_dtls_handshake(rtc_dtls_transport_t *dtls) {
    dtls->state = RTC_DTLS_STATE_CONNECTING;

    int ret = SSL_do_handshake(dtls->ssl);
    dtls_flush_wbio(dtls);

    if (ret == 1) {
        dtls->state = RTC_DTLS_STATE_CONNECTED;
        RTC_LOG_INFO("DTLS: handshake complete");
        return RTC_OK;
    }

    int err = SSL_get_error(dtls->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return RTC_OK; /* handshake in progress */
    }

    RTC_LOG_ERR("DTLS: handshake error %d", err);
    dtls->state = RTC_DTLS_STATE_FAILED;
    return RTC_ERR_SSL;
}

int rtc_dtls_retransmit(rtc_dtls_transport_t *dtls) {
    if (dtls->state != RTC_DTLS_STATE_CONNECTING)
        return RTC_OK;

    /* DTLSv1_handle_timeout returns: 0=no timeout, 1=retransmit needed, -1=error */
    int ret = DTLSv1_handle_timeout(dtls->ssl);
    if (ret > 0) {
        RTC_LOG_DBG("DTLS: retransmitting flight");
        dtls_flush_wbio(dtls);
    }
    return RTC_OK;
}

int rtc_dtls_recv(rtc_dtls_transport_t *dtls, const uint8_t *data, size_t len) {
    /* Feed data into the read BIO */
    BIO_write(dtls->rbio, data, (int)len);

    if (dtls->state == RTC_DTLS_STATE_CONNECTING || dtls->state == RTC_DTLS_STATE_NEW) {
        /* Continue handshake */
        return rtc_dtls_handshake(dtls);
    }

    /* If connected, application data will be read by the caller (peer.c)
     * via SSL_read. We just need to flush any pending output. */
    dtls_flush_wbio(dtls);

    return RTC_OK;
}

int rtc_dtls_export_srtp_keys(rtc_dtls_transport_t *dtls) {
    if (dtls->state != RTC_DTLS_STATE_CONNECTED)
        return RTC_ERR_SSL;

    /*
     * Export keying material per RFC 5764.
     * Layout: client_key | server_key | client_salt | server_salt
     */
    size_t total = 2 * (RTC_SRTP_MASTER_KEY_LEN + RTC_SRTP_MASTER_SALT_LEN);
    uint8_t material[2 * (16 + 14)]; /* 60 bytes */

    if (SSL_export_keying_material(dtls->ssl, material, total, "EXTRACTOR-dtls_srtp", 19, NULL, 0,
                                   0) != 1) {
        RTC_LOG_ERR("DTLS: failed to export SRTP keys");
        return RTC_ERR_SSL;
    }

    size_t off = 0;
    memcpy(dtls->srtp_client_key, material + off, RTC_SRTP_MASTER_KEY_LEN);
    off += RTC_SRTP_MASTER_KEY_LEN;
    memcpy(dtls->srtp_server_key, material + off, RTC_SRTP_MASTER_KEY_LEN);
    off += RTC_SRTP_MASTER_KEY_LEN;
    memcpy(dtls->srtp_client_salt, material + off, RTC_SRTP_MASTER_SALT_LEN);
    off += RTC_SRTP_MASTER_SALT_LEN;
    memcpy(dtls->srtp_server_salt, material + off, RTC_SRTP_MASTER_SALT_LEN);

    dtls->srtp_keys_ready = true;
    RTC_LOG_INFO("DTLS: SRTP keying material exported");
    return RTC_OK;
}

void rtc_dtls_close(rtc_dtls_transport_t *dtls) {
    dtls_close_ssl(dtls);
    if (dtls->cert) {
        X509_free(dtls->cert);
        dtls->cert = NULL;
    }
    if (dtls->pkey) {
        EVP_PKEY_free(dtls->pkey);
        dtls->pkey = NULL;
    }
    dtls->state = RTC_DTLS_STATE_CLOSED;
}
