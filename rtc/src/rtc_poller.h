/*
 * Platform I/O multiplexer abstraction.
 *
 * Backends:
 *   Linux  - epoll
 *   macOS  - kqueue
 *   Windows - select
 */
#ifndef RTC_POLLER_H
#define RTC_POLLER_H

#include "rtc_common.h"

#define RTC_POLLER_MAX_EVENTS 4

#ifdef __linux__
#  include <sys/epoll.h>
typedef struct {
    int epoll_fd;
} rtc_poller_t;
#elif defined(__APPLE__)
#  include <sys/event.h>
typedef struct {
    int kq_fd;
} rtc_poller_t;
#else /* Windows / fallback: select */
typedef struct {
    rtc_socket_t fds[RTC_POLLER_MAX_EVENTS];
    int fd_count;
} rtc_poller_t;
#endif

/* Initialize poller */
int rtc_poller_init(rtc_poller_t *p);

/* Add a socket for read-readiness monitoring */
int rtc_poller_add(rtc_poller_t *p, rtc_socket_t sock);

/* Remove a socket */
int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock);

/*
 * Wait for read readiness on any registered socket.
 * timeout_ms: max wait in milliseconds (0 = non-blocking, -1 = infinite).
 * Returns: number of ready sockets (>0), 0 on timeout, <0 on error.
 */
int rtc_poller_wait(rtc_poller_t *p, int timeout_ms);

/* Cleanup */
void rtc_poller_close(rtc_poller_t *p);

#endif /* RTC_POLLER_H */
