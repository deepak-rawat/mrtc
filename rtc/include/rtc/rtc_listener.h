/*
 * RTC listener API - shared UDP/TCP listening port for many logical transports.
 */
#ifndef RTC_LISTENER_H
#define RTC_LISTENER_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_listener.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_common.h"

typedef struct rtc_listener rtc_listener_t;

typedef struct {
    const char *listen_ip;
    const char *announced_ip;
    uint16_t udp_port;
    bool enable_udp;
    bool enable_tcp;
} rtc_listener_config_t;

#endif /* RTC_LISTENER_H */