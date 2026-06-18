/*
 * Internal shared runtime for client peer connections.
 */
#ifndef RTC_CLIENT_RUNTIME_H
#define RTC_CLIENT_RUNTIME_H

#include "rtc/rtc_listener.h"
#include "rtc/rtc_worker.h"

typedef struct rtc_client_runtime rtc_client_runtime_t;

typedef struct {
    char stun_server[256];
    uint16_t stun_port;
} rtc_client_runtime_config_t;

typedef void (*rtc_client_runtime_candidate_fn)(const rtc_transport_candidate_t *cand, void *user);
typedef void (*rtc_client_runtime_gathering_done_fn)(void *user);

rtc_client_runtime_t *rtc_client_runtime_acquire(const rtc_client_runtime_config_t *config);
void rtc_client_runtime_release(rtc_client_runtime_t *runtime);

int rtc_client_runtime_register_peer(rtc_client_runtime_t *runtime,
                                     rtc_client_runtime_candidate_fn on_candidate,
                                     rtc_client_runtime_gathering_done_fn on_done, void *user);
void rtc_client_runtime_unregister_peer(rtc_client_runtime_t *runtime, void *user);

rtc_worker_t *rtc_client_runtime_worker(rtc_client_runtime_t *runtime);
rtc_listener_t *rtc_client_runtime_listener(rtc_client_runtime_t *runtime);

#endif /* RTC_CLIENT_RUNTIME_H */
