/*
 * rtc_client_runtime.c - Process-default runtime for client peer connections.
 */
#include "rtc_client_runtime.h"

#include "rtc/rtc_common.h"

#include <stdbool.h>
#include <stdlib.h>

struct rtc_client_runtime {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_router_t *router;
    int ref_count;
};

static rtc_mutex_t g_runtime_lock;
static bool g_runtime_lock_ready;
static rtc_client_runtime_t *g_default_runtime;

static void client_runtime_destroy(rtc_client_runtime_t *runtime) {
    if (!runtime)
        return;
    if (runtime->router)
        rtc_router_destroy(runtime->router);
    if (runtime->listener)
        rtc_listener_destroy(runtime->listener);
    if (runtime->worker)
        rtc_worker_destroy(runtime->worker);
    free(runtime);
}

static rtc_client_runtime_t *client_runtime_create(void) {
    rtc_client_runtime_t *runtime = (rtc_client_runtime_t *)calloc(1, sizeof(*runtime));
    if (!runtime)
        return NULL;

    runtime->worker = rtc_worker_create(NULL);
    if (!runtime->worker)
        goto fail;

    runtime->listener = rtc_listener_create(runtime->worker, NULL);
    if (!runtime->listener)
        goto fail;

    runtime->router = rtc_router_create(runtime->worker, NULL);
    if (!runtime->router)
        goto fail;

    return runtime;

fail:
    client_runtime_destroy(runtime);
    return NULL;
}

int rtc_client_runtime_global_init(void) {
    if (g_runtime_lock_ready)
        return RTC_OK;
    int rc = rtc_mutex_init(&g_runtime_lock);
    if (rc != RTC_OK)
        return rc;
    g_runtime_lock_ready = true;
    return RTC_OK;
}

void rtc_client_runtime_global_cleanup(void) {
    rtc_client_runtime_t *runtime = NULL;

    if (g_runtime_lock_ready) {
        rtc_mutex_lock(&g_runtime_lock);
        runtime = g_default_runtime;
        g_default_runtime = NULL;
        rtc_mutex_unlock(&g_runtime_lock);
    }

    if (runtime) {
        if (runtime->ref_count > 0)
            RTC_LOG_WARN("Client runtime cleanup with %d live peer connection(s)",
                         runtime->ref_count);
        client_runtime_destroy(runtime);
    }

    if (g_runtime_lock_ready) {
        rtc_mutex_destroy(&g_runtime_lock);
        g_runtime_lock_ready = false;
    }
}

rtc_client_runtime_t *rtc_client_runtime_acquire(void) {
    if (!g_runtime_lock_ready && rtc_client_runtime_global_init() != RTC_OK)
        return NULL;

    rtc_mutex_lock(&g_runtime_lock);
    if (!g_default_runtime) {
        g_default_runtime = client_runtime_create();
        if (!g_default_runtime) {
            rtc_mutex_unlock(&g_runtime_lock);
            return NULL;
        }
    }
    g_default_runtime->ref_count++;
    rtc_client_runtime_t *runtime = g_default_runtime;
    rtc_mutex_unlock(&g_runtime_lock);
    return runtime;
}

void rtc_client_runtime_release(rtc_client_runtime_t *runtime) {
    if (!runtime || !g_runtime_lock_ready)
        return;

    bool destroy = false;
    rtc_mutex_lock(&g_runtime_lock);
    if (runtime->ref_count > 0)
        runtime->ref_count--;
    if (runtime->ref_count == 0 && g_default_runtime == runtime) {
        g_default_runtime = NULL;
        destroy = true;
    }
    rtc_mutex_unlock(&g_runtime_lock);

    if (destroy)
        client_runtime_destroy(runtime);
}

rtc_worker_t *rtc_client_runtime_worker(rtc_client_runtime_t *runtime) {
    return runtime ? runtime->worker : NULL;
}

rtc_listener_t *rtc_client_runtime_listener(rtc_client_runtime_t *runtime) {
    return runtime ? runtime->listener : NULL;
}

rtc_router_t *rtc_client_runtime_router(rtc_client_runtime_t *runtime) {
    return runtime ? runtime->router : NULL;
}