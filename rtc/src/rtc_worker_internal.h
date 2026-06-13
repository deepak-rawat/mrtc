/*
 * Internal worker runtime hooks.
 *
 * The worker owns the single event-loop thread for a runtime shard: it
 * drives the I/O poller, fires dynamic timers, and runs cross-thread
 * tasks. Listener and transport code use these hooks to register sockets
 * and to marshal state-mutating work onto the loop thread so that all
 * per-transport state stays single-threaded.
 */
#ifndef RTC_WORKER_INTERNAL_H
#define RTC_WORKER_INTERNAL_H

#include "rtc/rtc_worker.h"

/* Task executed on the worker's loop thread. */
typedef void (*rtc_worker_task_fn)(void *user);

/* Readiness callback for a registered socket; fires on the loop thread. */
typedef void (*rtc_worker_io_fn)(rtc_socket_t fd, void *user);

/*
 * Run fn(user) on the worker thread and block until it completes. If
 * called from the worker thread itself, fn runs inline (no deadlock).
 * If the worker has already stopped, fn runs inline on the caller as a
 * best-effort fallback so teardown paths still complete.
 */
int rtc_worker_invoke(rtc_worker_t *worker, rtc_worker_task_fn fn, void *user);

/* Run fn(user) on the worker thread asynchronously (does not wait). */
int rtc_worker_post(rtc_worker_t *worker, rtc_worker_task_fn fn, void *user);

/*
 * Register/unregister a socket for read-readiness on the worker poller.
 * The registration is performed on the loop thread; fn fires there when
 * the socket becomes readable. Safe to call from any thread.
 */
int rtc_worker_add_io(rtc_worker_t *worker, rtc_socket_t fd, rtc_worker_io_fn fn, void *user);
int rtc_worker_remove_io(rtc_worker_t *worker, rtc_socket_t fd);

/* True when called from the worker's loop thread. */
bool rtc_worker_on_loop_thread(rtc_worker_t *worker);

#endif /* RTC_WORKER_INTERNAL_H */
