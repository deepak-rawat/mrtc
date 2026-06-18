/*
 * Process-default runtime for client peer connections.
 */
#include "rtc_client_runtime.h"

#include "rtc/rtc.h"
#include "rtc/rtc_client.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RTC_CLIENT_RUNTIME_MAX_PEERS 64

typedef struct {
    rtc_client_runtime_candidate_fn on_candidate;
    rtc_client_runtime_gathering_done_fn on_done;
    void *user;
} runtime_peer_entry_t;

struct rtc_client_runtime {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_client_runtime_config_t config;
    rtc_mutex_t peer_mutex;
    bool peer_mutex_ready;
    runtime_peer_entry_t peers[RTC_CLIENT_RUNTIME_MAX_PEERS];
    int peer_count;
    int ref_count;
    struct rtc_client_runtime *next;
};

static rtc_mutex_t g_runtime_lock;
static rtc_client_runtime_t *g_runtimes;

static bool runtime_config_equal(const rtc_client_runtime_config_t *a,
                                 const rtc_client_runtime_config_t *b) {
    uint16_t aport = a->stun_port ? a->stun_port : 3478;
    uint16_t bport = b->stun_port ? b->stun_port : 3478;
    return aport == bport && strcmp(a->stun_server, b->stun_server) == 0;
}

static void runtime_on_listener_candidate(const rtc_transport_candidate_t *cand, void *user) {
    rtc_client_runtime_t *runtime = (rtc_client_runtime_t *)user;
    runtime_peer_entry_t peers[RTC_CLIENT_RUNTIME_MAX_PEERS];
    int count = 0;
    rtc_mutex_lock(&runtime->peer_mutex);
    count = runtime->peer_count;
    for (int i = 0; i < count; i++)
        peers[i] = runtime->peers[i];
    rtc_mutex_unlock(&runtime->peer_mutex);

    for (int i = 0; i < count; i++) {
        if (peers[i].on_candidate)
            peers[i].on_candidate(cand, peers[i].user);
    }
}

static void runtime_on_listener_done(void *user) {
    rtc_client_runtime_t *runtime = (rtc_client_runtime_t *)user;
    runtime_peer_entry_t peers[RTC_CLIENT_RUNTIME_MAX_PEERS];
    int count = 0;
    rtc_mutex_lock(&runtime->peer_mutex);
    count = runtime->peer_count;
    for (int i = 0; i < count; i++)
        peers[i] = runtime->peers[i];
    rtc_mutex_unlock(&runtime->peer_mutex);

    for (int i = 0; i < count; i++) {
        if (peers[i].on_done)
            peers[i].on_done(peers[i].user);
    }
}

static void client_runtime_destroy(rtc_client_runtime_t *runtime) {
    if (!runtime)
        return;
    if (runtime->listener)
        rtc_listener_destroy(runtime->listener);
    if (runtime->worker)
        rtc_worker_destroy(runtime->worker);
    if (runtime->peer_mutex_ready)
        rtc_mutex_destroy(&runtime->peer_mutex);
    free(runtime);
}

static rtc_client_runtime_t *client_runtime_create(const rtc_client_runtime_config_t *config) {
    rtc_client_runtime_t *runtime = (rtc_client_runtime_t *)calloc(1, sizeof(*runtime));
    if (!runtime)
        return NULL;
    if (config)
        runtime->config = *config;
    if (runtime->config.stun_port == 0)
        runtime->config.stun_port = 3478;

    if (rtc_mutex_init(&runtime->peer_mutex) != RTC_OK)
        goto fail;
    runtime->peer_mutex_ready = true;

    runtime->worker = rtc_worker_create(NULL);
    if (!runtime->worker)
        goto fail;

    rtc_listener_config_t listener_cfg;
    memset(&listener_cfg, 0, sizeof(listener_cfg));
    listener_cfg.enable_udp = true;
    if (runtime->config.stun_server[0] != '\0') {
        listener_cfg.stun_server = runtime->config.stun_server;
        listener_cfg.stun_port = runtime->config.stun_port;
    }
    runtime->listener = rtc_listener_create(runtime->worker, &listener_cfg);
    if (!runtime->listener)
        goto fail;

    rtc_listener_set_on_candidate(runtime->listener, runtime_on_listener_candidate, runtime);
    rtc_listener_set_on_gathering_done(runtime->listener, runtime_on_listener_done, runtime);

    return runtime;

fail:
    client_runtime_destroy(runtime);
    return NULL;
}

rtc_client_runtime_t *rtc_client_runtime_acquire(const rtc_client_runtime_config_t *config) {
    rtc_client_runtime_config_t desired;
    memset(&desired, 0, sizeof(desired));
    if (config)
        desired = *config;
    if (desired.stun_port == 0)
        desired.stun_port = 3478;

    rtc_mutex_lock(&g_runtime_lock);
    for (rtc_client_runtime_t *it = g_runtimes; it; it = it->next) {
        if (runtime_config_equal(&it->config, &desired)) {
            it->ref_count++;
            rtc_mutex_unlock(&g_runtime_lock);
            return it;
        }
    }

    rtc_client_runtime_t *runtime = client_runtime_create(&desired);
    if (!runtime) {
        rtc_mutex_unlock(&g_runtime_lock);
        return NULL;
    }
    runtime->ref_count = 1;
    runtime->next = g_runtimes;
    g_runtimes = runtime;
    rtc_mutex_unlock(&g_runtime_lock);
    return runtime;
}

void rtc_client_runtime_release(rtc_client_runtime_t *runtime) {
    bool destroy = false;
    rtc_mutex_lock(&g_runtime_lock);
    if (--runtime->ref_count == 0) {
        rtc_client_runtime_t **slot = &g_runtimes;
        while (*slot && *slot != runtime)
            slot = &(*slot)->next;
        if (*slot == runtime)
            *slot = runtime->next;
        destroy = true;
    }
    rtc_mutex_unlock(&g_runtime_lock);

    if (destroy)
        client_runtime_destroy(runtime);
}

int rtc_client_runtime_register_peer(rtc_client_runtime_t *runtime,
                                     rtc_client_runtime_candidate_fn on_candidate,
                                     rtc_client_runtime_gathering_done_fn on_done, void *user) {
    if (!runtime || !user)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&runtime->peer_mutex);
    for (int i = 0; i < runtime->peer_count; i++) {
        if (runtime->peers[i].user == user) {
            runtime->peers[i].on_candidate = on_candidate;
            runtime->peers[i].on_done = on_done;
            rtc_mutex_unlock(&runtime->peer_mutex);
            return RTC_OK;
        }
    }
    if (runtime->peer_count >= RTC_CLIENT_RUNTIME_MAX_PEERS) {
        rtc_mutex_unlock(&runtime->peer_mutex);
        return RTC_ERR_NOMEM;
    }
    runtime->peers[runtime->peer_count++] = (runtime_peer_entry_t){on_candidate, on_done, user};
    rtc_mutex_unlock(&runtime->peer_mutex);
    return RTC_OK;
}

void rtc_client_runtime_unregister_peer(rtc_client_runtime_t *runtime, void *user) {
    if (!runtime || !user)
        return;
    rtc_mutex_lock(&runtime->peer_mutex);
    for (int i = 0; i < runtime->peer_count; i++) {
        if (runtime->peers[i].user != user)
            continue;
        runtime->peers[i] = runtime->peers[runtime->peer_count - 1];
        runtime->peer_count--;
        break;
    }
    rtc_mutex_unlock(&runtime->peer_mutex);
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
    rtc_client_runtime_t *leaked = g_runtimes;
    rtc_mutex_unlock(&g_runtime_lock);

    if (leaked) {
        /* Leak the mutex too -- pending releases still need it. */
        RTC_LOG_WARN("Client cleanup with live peer connection(s); leaking runtime");
    } else {
        rtc_mutex_destroy(&g_runtime_lock);
    }
    rtc_cleanup();
}
