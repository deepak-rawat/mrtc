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
    RTC_TRANSPORT_CANDIDATE_HOST,
} rtc_transport_candidate_type_t;

typedef struct {
    char foundation[32];
    char address[64];
    uint16_t port;
    char protocol[8];
    rtc_transport_candidate_type_t type;
} rtc_transport_candidate_t;

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