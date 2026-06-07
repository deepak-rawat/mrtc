/*
 * RTC transport API - one logical encrypted endpoint transport.
 */
#ifndef RTC_PUBLIC_TRANSPORT_H
#define RTC_PUBLIC_TRANSPORT_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_transport.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_common.h"

#define RTC_ICE_UFRAG_MAX 8
#define RTC_ICE_PWD_MAX   24

typedef struct rtc_transport rtc_transport_t;
typedef struct rtc_listener rtc_listener_t;

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
    char username_fragment[RTC_ICE_UFRAG_MAX];
    char password[RTC_ICE_PWD_MAX];
    rtc_ice_mode_t mode;
} rtc_ice_parameters_t;

typedef struct {
    rtc_listener_t *listener;
    rtc_ice_mode_t ice_mode;
    bool enable_sctp;
    bool enable_twcc;
    uint32_t initial_outgoing_bitrate_bps;
} rtc_transport_config_t;

typedef struct {
    bool closed;
    rtc_ice_mode_t ice_mode;
    uint64_t packets_received;
    uint64_t bytes_received;
} rtc_transport_stats_t;

int rtc_transport_get_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out);
int rtc_transport_get_stats(rtc_transport_t *transport, rtc_transport_stats_t *out);
void rtc_transport_close(rtc_transport_t *transport);
void rtc_transport_destroy(rtc_transport_t *transport);

#endif /* RTC_PUBLIC_TRANSPORT_H */