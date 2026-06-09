/*
 * rtc_client_runtime.h - Internal shared runtime for client peer connections.
 */
#ifndef RTC_CLIENT_RUNTIME_H
#define RTC_CLIENT_RUNTIME_H

#include "rtc/rtc_listener.h"
#include "rtc/rtc_router.h"
#include "rtc/rtc_worker.h"

typedef struct rtc_client_runtime rtc_client_runtime_t;

int rtc_client_runtime_global_init(void);
void rtc_client_runtime_global_cleanup(void);

rtc_client_runtime_t *rtc_client_runtime_acquire(void);
void rtc_client_runtime_release(rtc_client_runtime_t *runtime);

rtc_worker_t *rtc_client_runtime_worker(rtc_client_runtime_t *runtime);
rtc_listener_t *rtc_client_runtime_listener(rtc_client_runtime_t *runtime);
rtc_router_t *rtc_client_runtime_router(rtc_client_runtime_t *runtime);

#endif /* RTC_CLIENT_RUNTIME_H */