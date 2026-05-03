/*
 * rtc_utils.c - Library init/cleanup only.
 * All other utilities (logging, threading, sockets, etc.) are in common/.
 */
#include "rtc/rtc_types.h"

#ifdef _WIN32
#  include <windows.h>
#endif

#include <openssl/ssl.h>

/* ---------- Library init / cleanup ---------- */
int rtc_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        RTC_LOG_ERR("WSAStartup failed");
        return RTC_ERR_SOCKET;
    }
#endif
    /* OpenSSL init (modern versions auto-init, but be explicit) */
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    return RTC_OK;
}

void rtc_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}
