/*
 * mrtc - Minimal RTC protocol library in C (for learning)
 *
 * Single include header that pulls in the public API.
 * Internal headers (ICE, DTLS, SRTP, transport, etc.) are in rtc/src/.
 * External dependency: OpenSSL (libssl, libcrypto)
 */
#ifndef RTC_H
#define RTC_H

/* ---- Public API headers ---- */
#include "rtc_common.h"
#include "rtc_stats.h"

#ifdef MRTC_ENABLE_CLIENT_API
#  include "rtc_track.h"
#  include "rtc_data_channel.h"
#  include "rtc_peer.h"
#endif

#ifdef MRTC_ENABLE_SFU_API
#  include "rtc_sfu.h"
#endif

/* Library init / cleanup (call once) */
int rtc_init(void);
void rtc_cleanup(void);

#endif /* RTC_H */
