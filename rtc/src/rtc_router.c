/*
 * rtc_router.c - SFU media routing graph skeleton.
 */
#include "rtc/rtc_router.h"

#include "rtc_router_internal.h"
#include "rtc_transport_internal.h"

#include <stdatomic.h>
#include <stdlib.h>

struct rtc_router {
    rtc_worker_t *worker;
    void *app_data;
    _Atomic bool closed;
};

rtc_router_t *rtc_router_create(rtc_worker_t *worker, const rtc_router_config_t *cfg) {
    if (!worker)
        return NULL;
    rtc_router_t *router = (rtc_router_t *)calloc(1, sizeof(*router));
    if (!router)
        return NULL;
    router->worker = worker;
    router->app_data = cfg ? cfg->app_data : NULL;
    return router;
}

void rtc_router_close(rtc_router_t *router) {
    if (!router)
        return;
    atomic_store_explicit(&router->closed, true, memory_order_release);
}

void rtc_router_destroy(rtc_router_t *router) {
    if (!router)
        return;
    rtc_router_close(router);
    free(router);
}

rtc_transport_t *rtc_router_create_transport(rtc_router_t *router,
                                             const rtc_transport_config_t *cfg) {
    if (!router || atomic_load_explicit(&router->closed, memory_order_acquire))
        return NULL;
    return rtc_transport_create_internal(router, cfg);
}

rtc_worker_t *rtc_router_worker(rtc_router_t *router) {
    return router ? router->worker : NULL;
}