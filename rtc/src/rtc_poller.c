/*
 * rtc_poller.c - Platform-specific I/O multiplexer.
 *
 * Backends: epoll (Linux), kqueue (macOS/BSD), select (Windows).
 */
#include "rtc_poller.h"

#include <string.h>

/* ================================================================== */
#ifdef __linux__ /* ---- epoll backend ---- */
/* ================================================================== */

int rtc_poller_init(rtc_poller_t *p) {
    p->epoll_fd = epoll_create1(0);
    return (p->epoll_fd >= 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_add(rtc_poller_t *p, rtc_socket_t sock) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = sock;
    return (epoll_ctl(p->epoll_fd, EPOLL_CTL_ADD, sock, &ev) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock) {
    return (epoll_ctl(p->epoll_fd, EPOLL_CTL_DEL, sock, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_wait(rtc_poller_t *p, int timeout_ms) {
    struct epoll_event events[RTC_POLLER_MAX_EVENTS];
    return epoll_wait(p->epoll_fd, events, RTC_POLLER_MAX_EVENTS, timeout_ms);
}

void rtc_poller_close(rtc_poller_t *p) {
    if (p->epoll_fd >= 0) {
        close(p->epoll_fd);
        p->epoll_fd = -1;
    }
}

/* ================================================================== */
#elif defined(__APPLE__) /* ---- kqueue backend ---- */
/* ================================================================== */

int rtc_poller_init(rtc_poller_t *p) {
    p->kq_fd = kqueue();
    return (p->kq_fd >= 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_add(rtc_poller_t *p, rtc_socket_t sock) {
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)sock, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    return (kevent(p->kq_fd, &ev, 1, NULL, 0, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock) {
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return (kevent(p->kq_fd, &ev, 1, NULL, 0, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_wait(rtc_poller_t *p, int timeout_ms) {
    struct kevent events[RTC_POLLER_MAX_EVENTS];
    if (timeout_ms < 0) {
        return kevent(p->kq_fd, NULL, 0, events, RTC_POLLER_MAX_EVENTS, NULL);
    }
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    return kevent(p->kq_fd, NULL, 0, events, RTC_POLLER_MAX_EVENTS, &ts);
}

void rtc_poller_close(rtc_poller_t *p) {
    if (p->kq_fd >= 0) {
        close(p->kq_fd);
        p->kq_fd = -1;
    }
}

/* ================================================================== */
#else                    /* ---- select backend (Windows / fallback) ---- */
/* ================================================================== */

int rtc_poller_init(rtc_poller_t *p) {
    memset(p, 0, sizeof(*p));
    return RTC_OK;
}

int rtc_poller_add(rtc_poller_t *p, rtc_socket_t sock) {
    if (p->fd_count >= RTC_POLLER_MAX_EVENTS)
        return RTC_ERR_NOMEM;
    p->fds[p->fd_count++] = sock;
    return RTC_OK;
}

int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock) {
    for (int i = 0; i < p->fd_count; i++) {
        if (p->fds[i] == sock) {
            p->fds[i] = p->fds[--p->fd_count];
            return RTC_OK;
        }
    }
    return RTC_ERR_INVALID;
}

int rtc_poller_wait(rtc_poller_t *p, int timeout_ms) {
    if (p->fd_count == 0) {
        /* No sockets — just sleep for the timeout */
        if (timeout_ms > 0) {
#  ifdef _WIN32
            Sleep((DWORD)timeout_ms);
#  else
            usleep((unsigned)(timeout_ms * 1000));
#  endif
        }
        return 0;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
#  ifndef _WIN32
    int max_fd = 0;
#  endif
    for (int i = 0; i < p->fd_count; i++) {
        FD_SET(p->fds[i], &readfds);
#  ifndef _WIN32
        if ((int)p->fds[i] > max_fd)
            max_fd = (int)p->fds[i];
#  endif
    }

    struct timeval tv, *tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

#  ifdef _WIN32
    int n = select(0, &readfds, NULL, NULL, tvp); /* nfds ignored on Windows */
#  else
    int n = select(max_fd + 1, &readfds, NULL, NULL, tvp);
#  endif
    return n;
}

void rtc_poller_close(rtc_poller_t *p) {
    p->fd_count = 0;
}

#endif
