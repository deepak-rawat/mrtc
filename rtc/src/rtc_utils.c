/*
 * rtc_utils.c - Library init/cleanup only.
 * All other utilities (logging, threading, sockets, etc.) are in common/.
 */
#include "rtc/rtc_common.h"

#ifdef MRTC_ENABLE_CLIENT_API
#  include "rtc_client_runtime.h"
#endif

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

#ifdef MRTC_ENABLE_CLIENT_API
    int rc = rtc_client_runtime_global_init();
    if (rc != RTC_OK) {
#  ifdef _WIN32
        WSACleanup();
#  endif
        return rc;
    }
#endif
    return RTC_OK;
}

void rtc_cleanup(void) {
#ifdef MRTC_ENABLE_CLIENT_API
    rtc_client_runtime_global_cleanup();
#endif
#ifdef _WIN32
    WSACleanup();
#endif
}
