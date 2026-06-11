/*
 * Process-default runtime for client peer connections.
 */
#include "rtc_client_runtime.h"

#include "rtc/rtc.h"
#include "rtc/rtc_client.h"

#include <stdbool.h>
#include <stdlib.h>

struct rtc_client_runtime {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    int ref_count;
};

static rtc_mutex_t g_runtime_lock;
static rtc_client_runtime_t *g_default_runtime;

static void client_runtime_destroy(rtc_client_runtime_t *runtime) {
    if (!runtime)
        return;
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

    return runtime;

fail:
    client_runtime_destroy(runtime);
    return NULL;
}

rtc_client_runtime_t *rtc_client_runtime_acquire(void) {
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
    bool destroy = false;
    rtc_mutex_lock(&g_runtime_lock);
    if (--runtime->ref_count == 0) {
        if (g_default_runtime == runtime)
            g_default_runtime = NULL;
        destroy = true;
    }
    rtc_mutex_unlock(&g_runtime_lock);

    if (destroy)
        client_runtime_destroy(runtime);
}

rtc_worker_t *rtc_client_runtime_worker(rtc_client_runtime_t *runtime) {
    return runtime->worker;
}

rtc_listener_t *rtc_client_runtime_listener(rtc_client_runtime_t *runtime) {
    return runtime->listener;
}

int rtc_client_init(void) {
    int rc = rtc_init();
    if (rc != RTC_OK)
        return rc;
    rc = rtc_mutex_init(&g_runtime_lock);
    if (rc != RTC_OK) {
        rtc_cleanup();
        return rc;
    }
    return RTC_OK;
}

void rtc_client_cleanup(void) {
    rtc_mutex_lock(&g_runtime_lock);
    rtc_client_runtime_t *leaked = g_default_runtime;
    rtc_mutex_unlock(&g_runtime_lock);

    if (leaked) {
        /* Leak the mutex too -- pending releases still need it. */
        RTC_LOG_WARN("Client cleanup with live peer connection(s); leaking runtime");
    } else {
        rtc_mutex_destroy(&g_runtime_lock);
    }
    rtc_cleanup();
}
