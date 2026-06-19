/*
 * SFU media routing graph.
 *
 * The router is a thin owner of the worker + app data. SSRC -> producer
 * demultiplexing lives in each transport's RTP router (rtc_transport_bind_rtp),
 * so the router no longer keeps per-transport SSRC maps, mutexes, or a global
 * transport registry: producers bind directly into the transport demux and the
 * worker loop serializes bind / unbind / dispatch.
 */
#include "rtc/rtc_router.h"

#include "rtc_producer_internal.h"
#include "rtc_router_internal.h"

#include <stdatomic.h>
#include <stdlib.h>

struct rtc_router {
    rtc_worker_t *worker;
    void *app_data;
    _Atomic bool closed;
};

static void router_rtp_sink(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_producer_on_rtp((rtc_producer_t *)user, pkt);
}

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
    rtc_transport_t *transport = rtc_transport_create(router->worker, cfg);
    if (!transport)
        return NULL;
    rtc_transport_set_rtp_router(transport, router_rtp_sink, NULL, NULL);
    return transport;
}

rtc_worker_t *rtc_router_worker(rtc_router_t *router) {
    return router ? router->worker : NULL;
}

/* Bind a producer's SSRC into its transport's RTP router. Marshalled onto the
 * worker loop by the transport, so it is safe to call from the app thread while
 * media is flowing. The teardown contract is producers/consumers before their
 * transport, so the transport is always alive here. */
int rtc_router_register_producer(rtc_transport_t *transport, rtc_producer_t *producer,
                                 uint32_t ssrc) {
    if (!transport || !producer || ssrc == 0)
        return RTC_ERR_INVALID;
    return rtc_transport_bind_rtp(transport, ssrc, producer);
}

void rtc_router_unregister_producer(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport || ssrc == 0)
        return;
    rtc_transport_unbind_rtp(transport, ssrc);
}