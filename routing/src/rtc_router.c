/*
 * SFU media routing graph skeleton.
 */
#include "rtc/rtc_router.h"
#include "rtc/rtc_u32_map.h"
#include "rtc/rtc_vec.h"

#include "rtc_producer_internal.h"
#include "rtc_router_internal.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    rtc_transport_t *transport;
    rtc_router_t *router;
    rtc_u32_map_t producers_by_ssrc;
    rtc_mutex_t mutex;
    bool ready;
} rtc_router_transport_t;

struct rtc_router {
    rtc_worker_t *worker;
    void *app_data;
    rtc_vec_t transports;
    rtc_mutex_t transports_mutex;
    bool transports_ready;
    _Atomic bool closed;
};

static rtc_vec_t g_router_transports;
static rtc_mutex_t g_router_transports_mutex;
static bool g_router_transports_ready;

static int router_global_init(void) {
    if (g_router_transports_ready)
        return RTC_OK;
    int rc = rtc_vec_init(&g_router_transports, sizeof(rtc_router_transport_t *));
    if (rc != RTC_OK)
        return rc;
    rc = rtc_mutex_init(&g_router_transports_mutex);
    if (rc != RTC_OK) {
        rtc_vec_free(&g_router_transports);
        return rc;
    }
    g_router_transports_ready = true;
    return RTC_OK;
}

static rtc_router_transport_t *router_find_transport_locked(rtc_transport_t *transport) {
    size_t count = rtc_vec_len(&g_router_transports);
    for (size_t i = 0; i < count; i++) {
        rtc_router_transport_t **slot =
            (rtc_router_transport_t **)rtc_vec_at(&g_router_transports, i);
        if (slot && *slot && (*slot)->transport == transport)
            return *slot;
    }
    return NULL;
}

static rtc_router_transport_t *router_find_transport(rtc_transport_t *transport) {
    if (!transport || !g_router_transports_ready)
        return NULL;
    rtc_mutex_lock(&g_router_transports_mutex);
    rtc_router_transport_t *ctx = router_find_transport_locked(transport);
    rtc_mutex_unlock(&g_router_transports_mutex);
    return ctx;
}

static void router_on_rtp(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_router_transport_t *ctx = (rtc_router_transport_t *)user;
    if (!ctx || !pkt)
        return;

    rtc_mutex_lock(&ctx->mutex);
    rtc_producer_t *producer =
        (rtc_producer_t *)rtc_u32_map_get(&ctx->producers_by_ssrc, pkt->header.ssrc);
    rtc_mutex_unlock(&ctx->mutex);

    if (producer)
        rtc_producer_on_rtp(producer, pkt);
}

static void router_transport_destroy(rtc_router_transport_t *ctx) {
    if (!ctx)
        return;
    /* The transport is owned by the caller and, per the teardown
     * convention, destroyed before the router. Once destroyed it no
     * longer dispatches RTP, so we must NOT touch it here (doing so would
     * be a use-after-free). We only release the router-side context. */
    if (ctx->ready) {
        rtc_mutex_destroy(&ctx->mutex);
        rtc_u32_map_free(&ctx->producers_by_ssrc);
    }
    free(ctx);
}

rtc_router_t *rtc_router_create(rtc_worker_t *worker, const rtc_router_config_t *cfg) {
    if (!worker)
        return NULL;
    rtc_router_t *router = (rtc_router_t *)calloc(1, sizeof(*router));
    if (!router)
        return NULL;
    router->worker = worker;
    router->app_data = cfg ? cfg->app_data : NULL;
    if (rtc_vec_init(&router->transports, sizeof(rtc_router_transport_t *)) != RTC_OK ||
        rtc_mutex_init(&router->transports_mutex) != RTC_OK) {
        rtc_vec_free(&router->transports);
        free(router);
        return NULL;
    }
    router->transports_ready = true;
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
    if (router->transports_ready) {
        rtc_mutex_lock(&router->transports_mutex);
        size_t count = rtc_vec_len(&router->transports);
        for (size_t i = 0; i < count; i++) {
            rtc_router_transport_t **slot =
                (rtc_router_transport_t **)rtc_vec_at(&router->transports, i);
            if (!slot || !*slot)
                continue;
            rtc_router_transport_t *ctx = *slot;
            if (g_router_transports_ready) {
                rtc_mutex_lock(&g_router_transports_mutex);
                size_t global_count = rtc_vec_len(&g_router_transports);
                for (size_t j = 0; j < global_count; j++) {
                    rtc_router_transport_t **global_slot =
                        (rtc_router_transport_t **)rtc_vec_at(&g_router_transports, j);
                    if (global_slot && *global_slot == ctx) {
                        rtc_vec_swap_remove(&g_router_transports, j);
                        break;
                    }
                }
                rtc_mutex_unlock(&g_router_transports_mutex);
            }
            router_transport_destroy(ctx);
        }
        rtc_mutex_unlock(&router->transports_mutex);
        rtc_mutex_destroy(&router->transports_mutex);
        rtc_vec_free(&router->transports);
    }
    free(router);
}

rtc_transport_t *rtc_router_create_transport(rtc_router_t *router,
                                             const rtc_transport_config_t *cfg) {
    if (!router || atomic_load_explicit(&router->closed, memory_order_acquire))
        return NULL;
    if (router_global_init() != RTC_OK)
        return NULL;

    rtc_transport_t *transport = rtc_transport_create(router->worker, cfg);
    if (!transport)
        return NULL;

    rtc_router_transport_t *ctx = (rtc_router_transport_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        rtc_transport_destroy(transport);
        return NULL;
    }
    ctx->transport = transport;
    ctx->router = router;
    if (rtc_u32_map_init(&ctx->producers_by_ssrc) != RTC_OK ||
        rtc_mutex_init(&ctx->mutex) != RTC_OK) {
        router_transport_destroy(ctx);
        rtc_transport_destroy(transport);
        return NULL;
    }
    ctx->ready = true;

    rtc_mutex_lock(&router->transports_mutex);
    int rc = rtc_vec_push(&router->transports, &ctx);
    rtc_mutex_unlock(&router->transports_mutex);
    if (rc != RTC_OK) {
        router_transport_destroy(ctx);
        rtc_transport_destroy(transport);
        return NULL;
    }

    rtc_mutex_lock(&g_router_transports_mutex);
    rc = rtc_vec_push(&g_router_transports, &ctx);
    rtc_mutex_unlock(&g_router_transports_mutex);
    if (rc != RTC_OK) {
        router_transport_destroy(ctx);
        rtc_transport_destroy(transport);
        return NULL;
    }

    rtc_transport_on_rtp(transport, router_on_rtp, ctx);
    return transport;
}

rtc_worker_t *rtc_router_worker(rtc_router_t *router) {
    return router ? router->worker : NULL;
}

int rtc_router_register_producer(rtc_transport_t *transport, rtc_producer_t *producer,
                                 uint32_t ssrc) {
    if (!transport || !producer || ssrc == 0)
        return RTC_ERR_INVALID;
    rtc_router_transport_t *ctx = router_find_transport(transport);
    if (!ctx)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&ctx->mutex);
    int rc = rtc_u32_map_set(&ctx->producers_by_ssrc, ssrc, producer);
    rtc_mutex_unlock(&ctx->mutex);
    return rc;
}

void rtc_router_unregister_producer(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport || ssrc == 0)
        return;
    rtc_router_transport_t *ctx = router_find_transport(transport);
    if (!ctx)
        return;
    rtc_mutex_lock(&ctx->mutex);
    rtc_u32_map_remove(&ctx->producers_by_ssrc, ssrc);
    rtc_mutex_unlock(&ctx->mutex);
}