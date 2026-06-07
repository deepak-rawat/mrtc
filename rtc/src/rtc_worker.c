/*
 * rtc_worker.c - SFU/client runtime worker skeleton.
 *
 * The worker will become the owner of packet I/O, command queues, and dynamic
 * timers. This first slice establishes lifecycle and timer-scheduler ownership
 * without changing the existing packet I/O thread model.
 */
#include "rtc/rtc_worker.h"

#include "rtc_timer_sched.h"
#include "rtc_worker_internal.h"

#include <stdlib.h>
#include <string.h>

#define RTC_WORKER_NAME_MAX 64

struct rtc_worker {
    char name[RTC_WORKER_NAME_MAX];
    int max_packet_batch;
    int timer_resolution_ms;
    bool closed;
    bool running;
    bool thread_started;
    rtc_thread_t thread;
    rtc_mutex_t mutex;
    rtc_cond_t cond;
    bool sync_ready;
    rtc_timer_sched_t timers;
};

static void *worker_thread_fn(void *arg) {
    rtc_worker_t *worker = (rtc_worker_t *)arg;
    for (;;) {
        rtc_timer_sched_fn fn = NULL;
        void *timer_user = NULL;

        rtc_mutex_lock(&worker->mutex);
        for (;;) {
            if (!worker->running) {
                rtc_mutex_unlock(&worker->mutex);
                return NULL;
            }

            uint64_t now = rtc_time_ms();
            if (rtc_timer_sched_pop_due(&worker->timers, now, &fn, &timer_user))
                break;

            int timeout = rtc_timer_sched_next_timeout_ms(&worker->timers, now, 100);
            if (timeout < worker->timer_resolution_ms)
                timeout = worker->timer_resolution_ms;
            rtc_cond_wait_timeout(&worker->cond, &worker->mutex, (uint32_t)timeout);
        }
        rtc_mutex_unlock(&worker->mutex);

        if (fn)
            fn(timer_user);
    }
}

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
    if (rtc_mutex_init(&worker->mutex) != RTC_OK) {
        rtc_timer_sched_close(&worker->timers);
        free(worker);
        return NULL;
    }
    if (rtc_cond_init(&worker->cond) != RTC_OK) {
        rtc_mutex_destroy(&worker->mutex);
        rtc_timer_sched_close(&worker->timers);
        free(worker);
        return NULL;
    }
    worker->sync_ready = true;
    worker->running = true;
    if (rtc_thread_create(&worker->thread, worker_thread_fn, worker) != RTC_OK) {
        rtc_cond_destroy(&worker->cond);
        rtc_mutex_destroy(&worker->mutex);
        rtc_timer_sched_close(&worker->timers);
        free(worker);
        return NULL;
    }
    worker->thread_started = true;
    return worker;
}

void rtc_worker_close(rtc_worker_t *worker) {
    if (!worker)
        return;
    bool join_thread = false;
    if (worker->sync_ready) {
        rtc_mutex_lock(&worker->mutex);
        if (!worker->closed) {
            worker->closed = true;
            worker->running = false;
            if (worker->thread_started) {
                worker->thread_started = false;
                join_thread = true;
            }
            rtc_cond_signal(&worker->cond);
        }
        rtc_mutex_unlock(&worker->mutex);
    } else {
        worker->closed = true;
    }
    if (join_thread)
        rtc_thread_join(&worker->thread);
}

void rtc_worker_destroy(rtc_worker_t *worker) {
    if (!worker)
        return;
    rtc_worker_close(worker);
    if (worker->sync_ready) {
        rtc_cond_destroy(&worker->cond);
        rtc_mutex_destroy(&worker->mutex);
    }
    rtc_timer_sched_close(&worker->timers);
    free(worker);
}

int rtc_worker_get_stats(rtc_worker_t *worker, rtc_worker_stats_t *out) {
    if (!worker || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (worker->sync_ready)
        rtc_mutex_lock(&worker->mutex);
    out->closed = worker->closed;
    out->timers_pending = rtc_timer_sched_pending_count(&worker->timers);
    if (worker->sync_ready)
        rtc_mutex_unlock(&worker->mutex);
    return RTC_OK;
}

rtc_worker_timer_t rtc_worker_add_timer(rtc_worker_t *worker, uint64_t deadline_ms,
                                        rtc_worker_timer_fn fn, void *user) {
    if (!worker || !fn || !worker->sync_ready)
        return RTC_WORKER_TIMER_INVALID;

    rtc_mutex_lock(&worker->mutex);
    if (worker->closed || !worker->running) {
        rtc_mutex_unlock(&worker->mutex);
        return RTC_WORKER_TIMER_INVALID;
    }
    rtc_worker_timer_t timer = rtc_timer_sched_add(&worker->timers, deadline_ms, fn, user);
    if (timer != RTC_WORKER_TIMER_INVALID)
        rtc_cond_signal(&worker->cond);
    rtc_mutex_unlock(&worker->mutex);
    return timer;
}

void rtc_worker_cancel_timer(rtc_worker_t *worker, rtc_worker_timer_t timer) {
    if (!worker || !worker->sync_ready || timer == RTC_WORKER_TIMER_INVALID)
        return;
    rtc_mutex_lock(&worker->mutex);
    rtc_timer_sched_cancel(&worker->timers, timer);
    rtc_cond_signal(&worker->cond);
    rtc_mutex_unlock(&worker->mutex);
}