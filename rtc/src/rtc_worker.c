/*
 * SFU/client runtime worker: the single event-loop thread.
 *
 * The worker owns the I/O poller, the dynamic timer scheduler, and a
 * cross-thread task queue. Its loop thread is the sole place that drains
 * registered sockets, fires timers, and runs marshalled tasks, so all
 * per-transport protocol state stays single-threaded without per-object
 * locks.
 */
#include "rtc/rtc_worker.h"

#include "rtc_poller.h"
#include "rtc_timer_sched.h"
#include "rtc_worker_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RTC_WORKER_NAME_MAX   64
#define RTC_WORKER_DEFAULT_IO 8

typedef struct {
    rtc_socket_t fd;
    rtc_worker_io_fn fn;
    void *user;
} worker_io_source_t;

typedef struct worker_task {
    rtc_worker_task_fn fn;
    void *user;
    bool sync;         /* true: stack-allocated by invoke; false: heap from post */
    _Atomic bool done; /* sync only: set when the loop finishes running fn */
    struct worker_task *next;
} worker_task_t;

struct rtc_worker {
    char name[RTC_WORKER_NAME_MAX];
    int max_packet_batch;
    int timer_resolution_ms;
    bool closed;
    _Atomic bool running;
    bool thread_started;
    rtc_thread_t thread;
    rtc_mutex_t mutex;
    rtc_cond_t cond; /* signals sync-invoke completion */
    rtc_timer_sched_t timers;
    rtc_poller_t poller;

    /* I/O sources: touched only on the loop thread (registration is
     * marshalled there), so no lock is needed to read/write this table. */
    worker_io_source_t *io;
    int io_cap;
    int io_count;

    /* Cross-thread task queue, guarded by mutex. */
    worker_task_t *task_head;
    worker_task_t *task_tail;
};

/* Identifies the calling thread as the worker loop thread so invoke can
 * run inline instead of deadlocking when called from a worker callback. */
static _Thread_local rtc_worker_t *tls_current_worker;

bool rtc_worker_on_loop_thread(rtc_worker_t *worker) {
    return worker && tls_current_worker == worker;
}

/* --- Task queue (caller holds worker->mutex) --- */

static void worker_enqueue(rtc_worker_t *worker, worker_task_t *task) {
    task->next = NULL;
    if (worker->task_tail)
        worker->task_tail->next = task;
    else
        worker->task_head = task;
    worker->task_tail = task;
}

static worker_task_t *worker_dequeue(rtc_worker_t *worker) {
    worker_task_t *task = worker->task_head;
    if (task) {
        worker->task_head = task->next;
        if (!worker->task_head)
            worker->task_tail = NULL;
    }
    return task;
}

/* Run all queued tasks to completion. Runs on the loop thread (and once
 * more after the loop exits so any late sync invoker is released). */
static void worker_run_tasks(rtc_worker_t *worker) {
    for (;;) {
        rtc_mutex_lock(&worker->mutex);
        worker_task_t *task = worker_dequeue(worker);
        rtc_mutex_unlock(&worker->mutex);
        if (!task)
            return;

        task->fn(task->user);

        if (task->sync) {
            rtc_mutex_lock(&worker->mutex);
            atomic_store_explicit(&task->done, true, memory_order_release);
            rtc_cond_broadcast(&worker->cond);
            rtc_mutex_unlock(&worker->mutex);
        } else {
            free(task);
        }
    }
}

static void *worker_thread_fn(void *arg) {
    rtc_worker_t *worker = (rtc_worker_t *)arg;
    tls_current_worker = worker;
    rtc_poller_event_t evs[RTC_POLLER_MAX_EVENTS];

    while (atomic_load_explicit(&worker->running, memory_order_acquire)) {
        rtc_mutex_lock(&worker->mutex);
        int timeout = rtc_timer_sched_next_timeout_ms(&worker->timers, rtc_time_ms(), 100);
        rtc_mutex_unlock(&worker->mutex);

        int n = rtc_poller_wait(&worker->poller, evs, RTC_POLLER_MAX_EVENTS, timeout);

        /* Drain ready sockets. The io table is loop-thread-only. */
        for (int e = 0; e < n; e++) {
            if (!(evs[e].events & RTC_POLLER_EV_READ))
                continue;
            for (int i = 0; i < worker->io_count; i++) {
                if (worker->io[i].fd == evs[e].fd) {
                    worker->io[i].fn(evs[e].fd, worker->io[i].user);
                    break;
                }
            }
        }

        /* Run marshalled tasks (registration, control ops, teardown). */
        worker_run_tasks(worker);

        /* Fire due timers. Pop under the lock, run unlocked so callbacks
         * may schedule/cancel timers without recursing on the mutex. */
        for (;;) {
            rtc_timer_sched_fn fn = NULL;
            void *timer_user = NULL;
            rtc_mutex_lock(&worker->mutex);
            bool due = rtc_timer_sched_pop_due(&worker->timers, rtc_time_ms(), &fn, &timer_user);
            rtc_mutex_unlock(&worker->mutex);
            if (!due)
                break;
            fn(timer_user);
        }
    }

    /* Release any sync invoker that raced shutdown. */
    worker_run_tasks(worker);
    tls_current_worker = NULL;
    return NULL;
}

static void worker_apply_config(rtc_worker_t *worker, const rtc_worker_config_t *cfg) {
    worker->max_packet_batch = 64;
    worker->timer_resolution_ms = 1;
    worker->io_cap = RTC_WORKER_DEFAULT_IO;
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
    if (cfg->max_io_sources > 0)
        worker->io_cap = cfg->max_io_sources;
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
    if (rtc_poller_init(&worker->poller) != RTC_OK) {
        rtc_cond_destroy(&worker->cond);
        rtc_mutex_destroy(&worker->mutex);
        rtc_timer_sched_close(&worker->timers);
        free(worker);
        return NULL;
    }
    worker->io = (worker_io_source_t *)calloc((size_t)worker->io_cap, sizeof(*worker->io));
    if (!worker->io) {
        rtc_poller_close(&worker->poller);
        rtc_cond_destroy(&worker->cond);
        rtc_mutex_destroy(&worker->mutex);
        rtc_timer_sched_close(&worker->timers);
        free(worker);
        return NULL;
    }
    atomic_store_explicit(&worker->running, true, memory_order_release);
    if (rtc_thread_create(&worker->thread, worker_thread_fn, worker) != RTC_OK) {
        atomic_store_explicit(&worker->running, false, memory_order_relaxed);
        free(worker->io);
        rtc_poller_close(&worker->poller);
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
    rtc_mutex_lock(&worker->mutex);
    if (!worker->closed) {
        worker->closed = true;
        atomic_store_explicit(&worker->running, false, memory_order_release);
        if (worker->thread_started) {
            worker->thread_started = false;
            join_thread = true;
        }
    }
    rtc_mutex_unlock(&worker->mutex);
    if (join_thread) {
        rtc_poller_wake(&worker->poller);
        rtc_thread_join(&worker->thread);
    }
}

void rtc_worker_destroy(rtc_worker_t *worker) {
    if (!worker)
        return;
    rtc_worker_close(worker);

    /* Drop any async tasks still queued (sync tasks were drained on the
     * loop thread before it exited). */
    worker_task_t *task = worker->task_head;
    while (task) {
        worker_task_t *next = task->next;
        if (!task->sync)
            free(task);
        task = next;
    }

    rtc_poller_close(&worker->poller);
    rtc_cond_destroy(&worker->cond);
    rtc_mutex_destroy(&worker->mutex);
    rtc_timer_sched_close(&worker->timers);
    free(worker->io);
    free(worker);
}

int rtc_worker_get_stats(rtc_worker_t *worker, rtc_worker_stats_t *out) {
    if (!worker || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    rtc_mutex_lock(&worker->mutex);
    out->closed = worker->closed;
    out->timers_pending = rtc_timer_sched_pending_count(&worker->timers);
    rtc_mutex_unlock(&worker->mutex);
    return RTC_OK;
}

rtc_worker_timer_t rtc_worker_add_timer(rtc_worker_t *worker, uint64_t deadline_ms,
                                        rtc_worker_timer_fn fn, void *user) {
    if (!worker || !fn)
        return RTC_WORKER_TIMER_INVALID;

    rtc_mutex_lock(&worker->mutex);
    if (worker->closed) {
        rtc_mutex_unlock(&worker->mutex);
        return RTC_WORKER_TIMER_INVALID;
    }
    rtc_worker_timer_t timer = rtc_timer_sched_add(&worker->timers, deadline_ms, fn, user);
    rtc_mutex_unlock(&worker->mutex);
    /* Wake the loop so it observes the new (possibly sooner) deadline. */
    if (timer != RTC_WORKER_TIMER_INVALID)
        rtc_poller_wake(&worker->poller);
    return timer;
}

void rtc_worker_cancel_timer(rtc_worker_t *worker, rtc_worker_timer_t timer) {
    if (!worker || timer == RTC_WORKER_TIMER_INVALID)
        return;
    rtc_mutex_lock(&worker->mutex);
    rtc_timer_sched_cancel(&worker->timers, timer);
    rtc_mutex_unlock(&worker->mutex);
}

int rtc_worker_post(rtc_worker_t *worker, rtc_worker_task_fn fn, void *user) {
    if (!worker || !fn)
        return RTC_ERR_INVALID;
    worker_task_t *task = (worker_task_t *)calloc(1, sizeof(*task));
    if (!task)
        return RTC_ERR_NOMEM;
    task->fn = fn;
    task->user = user;
    task->sync = false;

    rtc_mutex_lock(&worker->mutex);
    if (worker->closed) {
        rtc_mutex_unlock(&worker->mutex);
        free(task);
        return RTC_ERR_INVALID;
    }
    worker_enqueue(worker, task);
    rtc_poller_wake(&worker->poller);
    rtc_mutex_unlock(&worker->mutex);
    return RTC_OK;
}

int rtc_worker_invoke(rtc_worker_t *worker, rtc_worker_task_fn fn, void *user) {
    if (!worker || !fn)
        return RTC_ERR_INVALID;
    if (rtc_worker_on_loop_thread(worker)) {
        fn(user);
        return RTC_OK;
    }

    worker_task_t task;
    task.fn = fn;
    task.user = user;
    task.sync = true;
    task.next = NULL;
    atomic_store_explicit(&task.done, false, memory_order_relaxed);

    rtc_mutex_lock(&worker->mutex);
    if (worker->closed || !atomic_load_explicit(&worker->running, memory_order_acquire)) {
        /* Worker stopped: run inline as a best-effort fallback. */
        rtc_mutex_unlock(&worker->mutex);
        fn(user);
        return RTC_OK;
    }
    worker_enqueue(worker, &task);
    rtc_poller_wake(&worker->poller);
    while (!atomic_load_explicit(&task.done, memory_order_acquire))
        rtc_cond_wait_timeout(&worker->cond, &worker->mutex, 100);
    rtc_mutex_unlock(&worker->mutex);
    return RTC_OK;
}

typedef struct {
    rtc_worker_t *worker;
    rtc_socket_t fd;
    rtc_worker_io_fn fn;
    void *user;
    int rc;
} worker_io_add_ctx_t;

static void worker_do_add_io(void *user) {
    worker_io_add_ctx_t *ctx = (worker_io_add_ctx_t *)user;
    rtc_worker_t *worker = ctx->worker;
    if (worker->io_count >= worker->io_cap) {
        ctx->rc = RTC_ERR_NOMEM;
        return;
    }
    ctx->rc = rtc_poller_add(&worker->poller, ctx->fd);
    if (ctx->rc != RTC_OK)
        return;
    worker->io[worker->io_count].fd = ctx->fd;
    worker->io[worker->io_count].fn = ctx->fn;
    worker->io[worker->io_count].user = ctx->user;
    worker->io_count++;
}

int rtc_worker_add_io(rtc_worker_t *worker, rtc_socket_t fd, rtc_worker_io_fn fn, void *user) {
    if (!worker || fd == RTC_INVALID_SOCKET || !fn)
        return RTC_ERR_INVALID;
    worker_io_add_ctx_t ctx = {worker, fd, fn, user, RTC_OK};
    rtc_worker_invoke(worker, worker_do_add_io, &ctx);
    return ctx.rc;
}

typedef struct {
    rtc_worker_t *worker;
    rtc_socket_t fd;
    int rc;
} worker_io_remove_ctx_t;

static void worker_do_remove_io(void *user) {
    worker_io_remove_ctx_t *ctx = (worker_io_remove_ctx_t *)user;
    rtc_worker_t *worker = ctx->worker;
    for (int i = 0; i < worker->io_count; i++) {
        if (worker->io[i].fd == ctx->fd) {
            rtc_poller_remove(&worker->poller, ctx->fd);
            worker->io[i] = worker->io[worker->io_count - 1];
            worker->io_count--;
            ctx->rc = RTC_OK;
            return;
        }
    }
    ctx->rc = RTC_ERR_INVALID;
}

int rtc_worker_remove_io(rtc_worker_t *worker, rtc_socket_t fd) {
    if (!worker || fd == RTC_INVALID_SOCKET)
        return RTC_ERR_INVALID;
    worker_io_remove_ctx_t ctx = {worker, fd, RTC_OK};
    rtc_worker_invoke(worker, worker_do_remove_io, &ctx);
    return ctx.rc;
}
