/*
 * RTC listener API - shared UDP/TCP listening port for many logical transports.
 */
#ifndef RTC_LISTENER_H
#define RTC_LISTENER_H

#if !defined(MRTC_ENABLE_SFU_API) && !defined(MRTC_ENABLE_RUNTIME_TRANSPORT)
#  error "rtc_listener.h requires MRTC_ENABLE_SFU_API or MRTC_ENABLE_RUNTIME_TRANSPORT"
#endif

#include "rtc_transport.h"
#include "rtc_worker.h"

typedef struct rtc_listener rtc_listener_t;

typedef struct {
    const char *listen_ip;
    const char *announced_ip;
    uint16_t udp_port;
    bool enable_udp;
    bool enable_tcp;
} rtc_listener_config_t;

typedef struct {
    bool closed;
    rtc_addr_t local_addr;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t packets_unhandled;
} rtc_listener_stats_t;

rtc_listener_t *rtc_listener_create(rtc_worker_t *worker, const rtc_listener_config_t *cfg);
void rtc_listener_close(rtc_listener_t *listener);
void rtc_listener_destroy(rtc_listener_t *listener);

int rtc_listener_get_local_addr(rtc_listener_t *listener, rtc_addr_t *out);
int rtc_listener_get_candidates(rtc_listener_t *listener, rtc_transport_candidate_t *out,
                                int *count);
int rtc_listener_get_stats(rtc_listener_t *listener, rtc_listener_stats_t *out);

#endif /* RTC_LISTENER_H */