/*
 * mrtc - Minimal RTC protocol library in C
 *
 * External dependency: OpenSSL (libssl, libcrypto)
 */
#ifndef RTC_H
#define RTC_H

#include "rtc_common.h"

int rtc_init(void);
void rtc_cleanup(void);

#endif /* RTC_H */
