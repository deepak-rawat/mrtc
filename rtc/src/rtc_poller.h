/*
 * Platform I/O multiplexer abstraction.
 *
 * Backends:
 *   Linux   - epoll  + eventfd (wake)
 *   macOS   - kqueue + EVFILT_USER (wake)
 *   Windows - select + UDP self-loopback (wake)
 *
 * The wake mechanism lets another thread interrupt a blocked
 * rtc_poller_wait() so newly-scheduled timers or shutdown requests are
 * observed promptly. Wake events are drained internally and never
 * surface in the events[] array.
 */
#ifndef RTC_POLLER_H
#define RTC_POLLER_H

#include "rtc_common.h"

/* Capacity of the events[] array passed to rtc_poller_wait(). Bumped from
 * 4 so a single epoll_wait/kevent syscall can report many ready fds even
 * if we only watch one socket today \u2014 future-proofs against adding
 * per-peer sockets or a TCP listener to the same poller. */
#define RTC_POLLER_MAX_EVENTS 64

/* Event flags reported in rtc_poller_event_t::events. Bitmask so a future
 * write-readiness flag can be added without breaking the API. */
#define RTC_POLLER_EV_READ 0x01

typedef struct {
    rtc_socket_t fd;
    int events; /* bitmask of RTC_POLLER_EV_* */
} rtc_poller_event_t;

#ifdef __linux__
#  include <sys/epoll.h>
typedef struct {
    int epoll_fd;
    int wake_fd; /* eventfd */
} rtc_poller_t;
#elif defined(__APPLE__)
#  include <sys/event.h>
typedef struct {
    int kq_fd;
    /* macOS wake = EVFILT_USER with this ident. No fd needed. */
} rtc_poller_t;
#else /* Windows / fallback: select + wake socket */
typedef struct {
    rtc_socket_t fds[RTC_POLLER_MAX_EVENTS];
    int fd_count;
    rtc_socket_t wake_sock;        /* receives wake "ping" packets */
    rtc_addr_t wake_addr;          /* bound address of wake_sock */
    rtc_socket_t wake_sender_sock; /* fixed sender socket reused per wake() */
} rtc_poller_t;
#endif

/* Initialize poller (including the wake mechanism). */
int rtc_poller_init(rtc_poller_t *p);

/* Add a socket for read-readiness monitoring. */
int rtc_poller_add(rtc_poller_t *p, rtc_socket_t sock);

/* Remove a socket. */
int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock);

/*
 * Wait for read readiness on any registered socket. Wake events are
 * drained internally and do NOT appear in events[].
 *
 *   events    - caller-allocated array of at least max_events entries.
 *   max_events- capacity of events[]; must be > 0.
 *   timeout_ms- max wait in milliseconds (0 = non-blocking, -1 = infinite).
 *
 * Returns: number of events written to events[] (>=0), <0 on error.
 *          0 means the wait returned without any socket being ready
 *          (timeout or wake-only).
 */
int rtc_poller_wait(rtc_poller_t *p, rtc_poller_event_t *events, int max_events, int timeout_ms);

/*
 * Wake a blocked rtc_poller_wait() so it returns promptly. Idempotent;
 * multiple concurrent wakes coalesce into a single wake-up. Thread-safe.
 */
int rtc_poller_wake(rtc_poller_t *p);

/* Cleanup */
void rtc_poller_close(rtc_poller_t *p);

#endif /* RTC_POLLER_H */
