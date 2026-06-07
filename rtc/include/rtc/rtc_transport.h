/*
 * RTC transport API - one logical encrypted endpoint transport.
 */
#ifndef RTC_PUBLIC_TRANSPORT_H
#define RTC_PUBLIC_TRANSPORT_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_transport.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_common.h"

typedef struct rtc_transport rtc_transport_t;

typedef enum {
    RTC_ICE_MODE_FULL,
    RTC_ICE_MODE_LITE,
} rtc_ice_mode_t;

typedef struct {
    const char *username_fragment;
    const char *password;
    rtc_ice_mode_t mode;
} rtc_ice_parameters_t;

#endif /* RTC_PUBLIC_TRANSPORT_H */