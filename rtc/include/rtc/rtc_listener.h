/*
 * RTC listener API - shared UDP/TCP listening port for many logical transports.
 */
#ifndef RTC_LISTENER_H
#define RTC_LISTENER_H

#include "rtc_transport.h"
#include "rtc_worker.h"
#include "rtc_sdp.h"

typedef struct rtc_listener rtc_listener_t;

typedef struct {
    const char *listen_ip;
    const char *announced_ip;
    uint16_t udp_port;
    bool enable_udp;
    bool enable_tcp;
    const char *stun_server;
    uint16_t stun_port;
} rtc_listener_config_t;

typedef void (*rtc_listener_on_candidate_fn)(const rtc_transport_candidate_t *cand, void *user);
typedef void (*rtc_listener_on_gathering_done_fn)(void *user);

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
bool rtc_listener_gathering_complete(rtc_listener_t *listener);
void rtc_listener_set_on_candidate(rtc_listener_t *listener, rtc_listener_on_candidate_fn fn,
                                   void *user);
void rtc_listener_set_on_gathering_done(rtc_listener_t *listener,
                                        rtc_listener_on_gathering_done_fn fn, void *user);
int rtc_listener_candidate_to_ice(const rtc_transport_candidate_t *candidate, int local_pref,
                                  rtc_ice_candidate_t *out);
int rtc_listener_fill_sdp_candidates(rtc_listener_t *listener, rtc_sdp_t *sdp);
int rtc_listener_get_stats(rtc_listener_t *listener, rtc_listener_stats_t *out);

#endif /* RTC_LISTENER_H */
