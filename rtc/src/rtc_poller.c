/*
 * Platform-specific I/O multiplexer with wake support.
 *
 * Backends: epoll+eventfd (Linux), kqueue+EVFILT_USER (macOS),
 *           select + UDP loopback wake socket (Windows / fallback).
 */
#include "rtc_poller.h"

#include <string.h>

static bool valid_interests(int events) {
    return events && !(events & ~(RTC_POLLER_EV_READ | RTC_POLLER_EV_WRITE));
}

int rtc_poller_add(rtc_poller_t *p, rtc_socket_t sock) {
    return rtc_poller_add_events(p, sock, RTC_POLLER_EV_READ);
}

#ifdef __linux__ /* epoll + eventfd backend */

#  include <sys/eventfd.h>
#  include <unistd.h>

int rtc_poller_init(rtc_poller_t *p) {
    p->epoll_fd = -1;
    p->wake_fd = -1;

    p->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epoll_fd < 0)
        return RTC_ERR_GENERIC;

    p->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (p->wake_fd < 0) {
        close(p->epoll_fd);
        p->epoll_fd = -1;
        return RTC_ERR_GENERIC;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = p->wake_fd;
    if (epoll_ctl(p->epoll_fd, EPOLL_CTL_ADD, p->wake_fd, &ev) != 0) {
        close(p->wake_fd);
        close(p->epoll_fd);
        p->wake_fd = -1;
        p->epoll_fd = -1;
        return RTC_ERR_GENERIC;
    }
    return RTC_OK;
}

static int poller_set(rtc_poller_t *p, rtc_socket_t sock, int events, int operation) {
    if (!p || sock == RTC_INVALID_SOCKET || !valid_interests(events))
        return RTC_ERR_INVALID;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = ((events & RTC_POLLER_EV_READ) ? EPOLLIN : 0) |
                ((events & RTC_POLLER_EV_WRITE) ? EPOLLOUT : 0);
    ev.data.fd = sock;
    return epoll_ctl(p->epoll_fd, operation, sock, &ev) == 0 ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_add_events(rtc_poller_t *p, rtc_socket_t sock, int events) {
    return poller_set(p, sock, events, EPOLL_CTL_ADD);
}

int rtc_poller_update(rtc_poller_t *p, rtc_socket_t sock, int events) {
    return poller_set(p, sock, events, EPOLL_CTL_MOD);
}

int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock) {
    return (epoll_ctl(p->epoll_fd, EPOLL_CTL_DEL, sock, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_wait(rtc_poller_t *p, rtc_poller_event_t *events, int max_events, int timeout_ms) {
    if (!events || max_events <= 0)
        return RTC_ERR_INVALID;

    struct epoll_event evs[RTC_POLLER_MAX_EVENTS];
    int cap = max_events < RTC_POLLER_MAX_EVENTS ? max_events : RTC_POLLER_MAX_EVENTS;
    int n = epoll_wait(p->epoll_fd, evs, cap, timeout_ms);
    if (n < 0)
        return RTC_ERR_GENERIC;

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].data.fd == p->wake_fd) {
            /* Drain the eventfd counter; further wakes between this
             * read() and the next wait() will set it again. */
            uint64_t drain;
            (void)read(p->wake_fd, &drain, sizeof(drain));
            continue;
        }
        if (out >= max_events)
            break;
        events[out].fd = evs[i].data.fd;
        events[out].events = ((evs[i].events & EPOLLIN) ? RTC_POLLER_EV_READ : 0) |
                             ((evs[i].events & EPOLLOUT) ? RTC_POLLER_EV_WRITE : 0) |
                             ((evs[i].events & (EPOLLERR | EPOLLHUP)) ? RTC_POLLER_EV_ERROR : 0);
        out++;
    }
    return out;
}

int rtc_poller_wake(rtc_poller_t *p) {
    /* Idempotent: eventfd counter saturates and any non-zero read drains. */
    uint64_t one = 1;
    ssize_t w = write(p->wake_fd, &one, sizeof(one));
    return (w == (ssize_t)sizeof(one)) ? RTC_OK : RTC_ERR_GENERIC;
}

void rtc_poller_close(rtc_poller_t *p) {
    if (p->wake_fd >= 0) {
        close(p->wake_fd);
        p->wake_fd = -1;
    }
    if (p->epoll_fd >= 0) {
        close(p->epoll_fd);
        p->epoll_fd = -1;
    }
}

#elif defined(__APPLE__) /* kqueue + EVFILT_USER backend */

#  include <unistd.h>

/* Identifier for our user event. Any uintptr_t value works since it's
 * scoped to (kq, filter) and we only use one user event per poller. */
#  define RTC_POLLER_WAKE_IDENT 1u

int rtc_poller_init(rtc_poller_t *p) {
    p->kq_fd = kqueue();
    if (p->kq_fd < 0)
        return RTC_ERR_GENERIC;

    /* Register a user-triggered event for wake(). EV_CLEAR makes the
     * event auto-rearm: it fires once per NOTE_TRIGGER. */
    struct kevent ev;
    EV_SET(&ev, RTC_POLLER_WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(p->kq_fd, &ev, 1, NULL, 0, NULL) != 0) {
        close(p->kq_fd);
        p->kq_fd = -1;
        return RTC_ERR_GENERIC;
    }
    return RTC_OK;
}

static int poller_set(rtc_poller_t *p, rtc_socket_t sock, int events, bool add) {
    if (!p || sock == RTC_INVALID_SOCKET || !valid_interests(events))
        return RTC_ERR_INVALID;
    struct kevent changes[2];
    unsigned flags = add ? EV_ADD : 0;
    EV_SET(&changes[0], (uintptr_t)sock, EVFILT_READ,
           flags | ((events & RTC_POLLER_EV_READ) ? EV_ENABLE : EV_DISABLE), 0, 0, NULL);
    EV_SET(&changes[1], (uintptr_t)sock, EVFILT_WRITE,
           flags | ((events & RTC_POLLER_EV_WRITE) ? EV_ENABLE : EV_DISABLE), 0, 0, NULL);
    return kevent(p->kq_fd, changes, 2, NULL, 0, NULL) == 0 ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_add_events(rtc_poller_t *p, rtc_socket_t sock, int events) {
    return poller_set(p, sock, events, true);
}

int rtc_poller_update(rtc_poller_t *p, rtc_socket_t sock, int events) {
    return poller_set(p, sock, events, false);
}

int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock) {
    struct kevent changes[2];
    EV_SET(&changes[0], (uintptr_t)sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], (uintptr_t)sock, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    return kevent(p->kq_fd, changes, 2, NULL, 0, NULL) == 0 ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_poller_wait(rtc_poller_t *p, rtc_poller_event_t *events, int max_events, int timeout_ms) {
    if (!events || max_events <= 0)
        return RTC_ERR_INVALID;

    struct kevent evs[RTC_POLLER_MAX_EVENTS];
    int cap = max_events < RTC_POLLER_MAX_EVENTS ? max_events : RTC_POLLER_MAX_EVENTS;

    int n;
    if (timeout_ms < 0) {
        n = kevent(p->kq_fd, NULL, 0, evs, cap, NULL);
    } else {
        struct timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        n = kevent(p->kq_fd, NULL, 0, evs, cap, &ts);
    }
    if (n < 0)
        return RTC_ERR_GENERIC;

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].filter == EVFILT_USER && evs[i].ident == RTC_POLLER_WAKE_IDENT) {
            /* EV_CLEAR resets the trigger automatically. */
            continue;
        }
        if (out >= max_events)
            break;
        events[out].fd = (rtc_socket_t)evs[i].ident;
        events[out].events = ((evs[i].filter == EVFILT_READ) ? RTC_POLLER_EV_READ : 0) |
                             ((evs[i].filter == EVFILT_WRITE) ? RTC_POLLER_EV_WRITE : 0) |
                             ((evs[i].flags & (EV_EOF | EV_ERROR)) ? RTC_POLLER_EV_ERROR : 0);
        out++;
    }
    return out;
}

int rtc_poller_wake(rtc_poller_t *p) {
    struct kevent ev;
    EV_SET(&ev, RTC_POLLER_WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    return (kevent(p->kq_fd, &ev, 1, NULL, 0, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

void rtc_poller_close(rtc_poller_t *p) {
    if (p->kq_fd >= 0) {
        close(p->kq_fd);
        p->kq_fd = -1;
    }
}

#else /* select + UDP loopback wake backend (Windows / fallback) */

#  ifdef _WIN32
#    include <windows.h>
#  else
#    include <unistd.h>
#  endif

/* Set up wake_sock: a UDP socket bound to 127.0.0.1:0. wake() sendto's a
 * single byte to it through wake_sender_sock; rtc_poller_wait drains. */
static int wake_pair_init(rtc_poller_t *p) {
    p->wake_sock = RTC_INVALID_SOCKET;
    p->wake_sender_sock = RTC_INVALID_SOCKET;

    p->wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p->wake_sock == RTC_INVALID_SOCKET)
        return RTC_ERR_SOCKET;

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    if (bind(p->wake_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        rtc_close_socket(p->wake_sock);
        p->wake_sock = RTC_INVALID_SOCKET;
        return RTC_ERR_SOCKET;
    }

    p->wake_addr.len = sizeof(struct sockaddr_in);
    if (getsockname(p->wake_sock, (struct sockaddr *)&p->wake_addr.addr, &p->wake_addr.len) != 0) {
        rtc_close_socket(p->wake_sock);
        p->wake_sock = RTC_INVALID_SOCKET;
        return RTC_ERR_SOCKET;
    }

    if (rtc_set_nonblocking(p->wake_sock) != RTC_OK) {
        rtc_close_socket(p->wake_sock);
        p->wake_sock = RTC_INVALID_SOCKET;
        return RTC_ERR_SOCKET;
    }

    /* Persistent sender socket so wake() doesn't have to create one each
     * call (cheap on POSIX, very expensive on Windows). */
    p->wake_sender_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p->wake_sender_sock == RTC_INVALID_SOCKET) {
        rtc_close_socket(p->wake_sock);
        p->wake_sock = RTC_INVALID_SOCKET;
        return RTC_ERR_SOCKET;
    }

    /* wake() must be safe to call from a thread other than the poll
     * thread. Sender doesn't need to be non-blocking \u2014 sendto on a 1-byte
     * UDP packet to loopback effectively never blocks. */

    /* Register wake_sock so wait() reports it. */
    p->interests[p->fd_count] = RTC_POLLER_EV_READ;
    p->fds[p->fd_count++] = p->wake_sock;
    return RTC_OK;
}

int rtc_poller_init(rtc_poller_t *p) {
    memset(p, 0, sizeof(*p));
    int rc = wake_pair_init(p);
    if (rc != RTC_OK)
        return rc;
    return RTC_OK;
}

int rtc_poller_add_events(rtc_poller_t *p, rtc_socket_t sock, int events) {
    if (!p || sock == RTC_INVALID_SOCKET || !valid_interests(events))
        return RTC_ERR_INVALID;
#  ifndef _WIN32
    if (sock < 0 || sock >= FD_SETSIZE)
        return RTC_ERR_INVALID;
#  endif
    if (p->fd_count >= RTC_POLLER_MAX_EVENTS)
        return RTC_ERR_NOMEM;
    for (int i = 0; i < p->fd_count; i++)
        if (p->fds[i] == sock)
            return RTC_ERR_INVALID;
    p->interests[p->fd_count] = events;
    p->fds[p->fd_count++] = sock;
    return RTC_OK;
}

int rtc_poller_update(rtc_poller_t *p, rtc_socket_t sock, int events) {
    if (!p || !valid_interests(events))
        return RTC_ERR_INVALID;
    for (int i = 0; i < p->fd_count; i++)
        if (p->fds[i] == sock) {
            p->interests[i] = events;
            return RTC_OK;
        }
    return RTC_ERR_INVALID;
}

int rtc_poller_remove(rtc_poller_t *p, rtc_socket_t sock) {
    for (int i = 0; i < p->fd_count; i++) {
        if (p->fds[i] == sock) {
            p->fds[i] = p->fds[--p->fd_count];
            p->interests[i] = p->interests[p->fd_count];
            return RTC_OK;
        }
    }
    return RTC_ERR_INVALID;
}

int rtc_poller_wait(rtc_poller_t *p, rtc_poller_event_t *events, int max_events, int timeout_ms) {
    if (!events || max_events <= 0)
        return RTC_ERR_INVALID;

    fd_set readfds, writefds, errorfds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);
#  ifndef _WIN32
    int max_fd = 0;
#  endif
    for (int i = 0; i < p->fd_count; i++) {
        if (p->interests[i] & RTC_POLLER_EV_READ)
            FD_SET(p->fds[i], &readfds);
        if (p->interests[i] & RTC_POLLER_EV_WRITE)
            FD_SET(p->fds[i], &writefds);
        FD_SET(p->fds[i], &errorfds);
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
    int n = select(0, &readfds, &writefds, &errorfds, tvp); /* nfds ignored on Windows */
#  else
    int n = select(max_fd + 1, &readfds, &writefds, &errorfds, tvp);
#  endif
    if (n < 0)
        return RTC_ERR_GENERIC;

    int out = 0;
    for (int i = 0; i < p->fd_count && n > 0; i++) {
        int ready = (FD_ISSET(p->fds[i], &readfds) ? RTC_POLLER_EV_READ : 0) |
                    (FD_ISSET(p->fds[i], &writefds) ? RTC_POLLER_EV_WRITE : 0) |
                    (FD_ISSET(p->fds[i], &errorfds) ? RTC_POLLER_EV_ERROR : 0);
        if (!ready)
            continue;
        n--;
        if (p->fds[i] == p->wake_sock) {
            /* Drain whatever wake bytes are pending. */
            uint8_t drain[64];
            for (;;) {
                int r = recv(p->wake_sock, (char *)drain, sizeof(drain), 0);
                if (r <= 0)
                    break;
            }
            continue;
        }
        if (out >= max_events)
            break;
        events[out].fd = p->fds[i];
        events[out].events = ready;
        out++;
    }
    return out;
}

int rtc_poller_wake(rtc_poller_t *p) {
    /* Single byte on loopback. Sending more would just inflate the drain
     * loop on the next wait(); one is enough since recv drains all. */
    const char b = 0;
    int sent = sendto(p->wake_sender_sock, &b, 1, 0, (const struct sockaddr *)&p->wake_addr.addr,
                      p->wake_addr.len);
    return (sent == 1) ? RTC_OK : RTC_ERR_GENERIC;
}

void rtc_poller_close(rtc_poller_t *p) {
    if (p->wake_sender_sock != RTC_INVALID_SOCKET) {
        rtc_close_socket(p->wake_sender_sock);
        p->wake_sender_sock = RTC_INVALID_SOCKET;
    }
    if (p->wake_sock != RTC_INVALID_SOCKET) {
        rtc_close_socket(p->wake_sock);
        p->wake_sock = RTC_INVALID_SOCKET;
    }
    p->fd_count = 0;
}

#endif
