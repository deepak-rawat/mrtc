/*
 * rtc_worker.c - SFU/client runtime worker skeleton.
 *
 * The worker will become the owner of packet I/O, command queues, and dynamic
 * timers. This first slice establishes lifecycle and timer-scheduler ownership
 * without changing the existing packet I/O thread model.
 */
#include "rtc/rtc_worker.h"

#include "rtc_timer_sched.h"

#include <stdlib.h>
#include <string.h>

#define RTC_WORKER_NAME_MAX 64

struct rtc_worker {
    char name[RTC_WORKER_NAME_MAX];
    int max_packet_batch;
    int timer_resolution_ms;
    bool closed;
    rtc_timer_sched_t timers;
};

static void worker_apply_config(rtc_worker_t *worker, const rtc_worker_config_t *cfg) {
    worker->max_packet_batch = 64;
    worker->timer_resolution_ms = 1;
    if (!cfg)
        return;

    if (cfg->name) {
        size_t len = strlen(cfg->name);
        if (len >= sizeof(worker->name))
            len = sizeof(worker->name) - 1;
        memcpy(worker->name, cfg->name, len);
        worker->name[len] = '\0';
    }
    if (cfg->max_packet_batch > 0)
        worker->max_packet_batch = cfg->max_packet_batch;
    if (cfg->timer_resolution_ms > 0)
        worker->timer_resolution_ms = cfg->timer_resolution_ms;
}

rtc_worker_t *rtc_worker_create(const rtc_worker_config_t *cfg) {
    rtc_worker_t *worker = (rtc_worker_t *)calloc(1, sizeof(*worker));
    if (!worker)
        return NULL;
    worker_apply_config(worker, cfg);
    if (rtc_timer_sched_init(&worker->timers) != RTC_OK) {
        free(worker);
        return NULL;
    }
    return worker;
}

void rtc_worker_close(rtc_worker_t *worker) {
    if (!worker)
        return;
    worker->closed = true;
}

void rtc_worker_destroy(rtc_worker_t *worker) {
    if (!worker)
        return;
    rtc_worker_close(worker);
    rtc_timer_sched_close(&worker->timers);
    free(worker);
}

int rtc_worker_get_stats(rtc_worker_t *worker, rtc_worker_stats_t *out) {
    if (!worker || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->closed = worker->closed;
    out->timers_pending = rtc_timer_sched_pending_count(&worker->timers);
    return RTC_OK;
}