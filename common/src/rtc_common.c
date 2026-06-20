/*
 * Platform utilities, logging, socket helpers, and threading.
 */
#include "rtc/rtc_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

#include <openssl/rand.h>

static rtc_log_level_t g_log_level = RTC_LOG_INFO;
static FILE *g_log_file = NULL;
static bool g_log_stderr = true;

void rtc_set_log_level(rtc_log_level_t level) {
    g_log_level = level;
}

int rtc_set_log_file(const char *path, bool log_to_stderr) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_log_stderr = log_to_stderr;
    if (path) {
        g_log_file = fopen(path, "a");
        if (!g_log_file)
            return RTC_ERR_GENERIC;
    }
    return RTC_OK;
}

void rtc_log_close(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void rtc_log(rtc_log_level_t level, const char *fmt, ...) {
    if (level > g_log_level)
        return;

    static const char *labels[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[mrtc][%s] ", labels[level]);

    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    if (n >= (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = '\0';

    if (g_log_stderr)
        fwrite(buf, 1, n, stderr);
    if (g_log_file) {
        fwrite(buf, 1, n, g_log_file);
        fflush(g_log_file);
    }
}

void rtc_close_socket(rtc_socket_t s) {
    if (s == RTC_INVALID_SOCKET)
        return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

int rtc_set_nonblocking(rtc_socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return (ioctlsocket(s, FIONBIO, &mode) == 0) ? RTC_OK : RTC_ERR_SOCKET;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
        return RTC_ERR_SOCKET;
    return (fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0) ? RTC_OK : RTC_ERR_SOCKET;
#endif
}

int rtc_addr_from_string(rtc_addr_t *out, const char *ip, uint16_t port) {
    memset(out, 0, sizeof(*out));

    struct sockaddr_in *sin = (struct sockaddr_in *)&out->addr;
    if (inet_pton(AF_INET, ip, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        out->len = sizeof(struct sockaddr_in);
        return RTC_OK;
    }

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&out->addr;
    memset(sin6, 0, sizeof(*sin6));
    if (inet_pton(AF_INET6, ip, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        out->len = sizeof(struct sockaddr_in6);
        return RTC_OK;
    }

    return RTC_ERR_INVALID;
}

int rtc_addr_to_string(const rtc_addr_t *a, char *buf, size_t buflen, uint16_t *port_out) {
    const struct sockaddr *sa = (const struct sockaddr *)&a->addr;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, (socklen_t)buflen))
            return RTC_ERR_INVALID;
        if (port_out)
            *port_out = ntohs(sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        if (!inet_ntop(AF_INET6, &sin6->sin6_addr, buf, (socklen_t)buflen))
            return RTC_ERR_INVALID;
        if (port_out)
            *port_out = ntohs(sin6->sin6_port);
    } else {
        return RTC_ERR_INVALID;
    }
    return RTC_OK;
}

int rtc_random_bytes(uint8_t *buf, size_t len) {
    if (RAND_bytes(buf, (int)len) != 1)
        return RTC_ERR_GENERIC;
    return RTC_OK;
}

int rtc_random_string(char *buf, size_t len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (len == 0)
        return RTC_ERR_INVALID;

    uint8_t tmp[256];
    size_t need = len - 1;
    if (need > sizeof(tmp))
        need = sizeof(tmp);
    if (rtc_random_bytes(tmp, need) != RTC_OK)
        return RTC_ERR_GENERIC;

    for (size_t i = 0; i < need; i++) {
        buf[i] = charset[tmp[i] % (sizeof(charset) - 1)];
    }
    buf[need] = '\0';
    return RTC_OK;
}

uint64_t rtc_time_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

uint64_t rtc_time_us(void) {
#ifdef _WIN32
    /* QueryPerformanceCounter has microsecond+ resolution */
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

uint64_t rtc_time_unix_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* 100 ns ticks since 1601-01-01; 116444736000000000 ticks to 1970-01-01. */
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000ULL) / 10000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

#ifdef _WIN32

static DWORD WINAPI win_thread_wrapper(LPVOID arg) {
    typedef struct {
        rtc_thread_fn fn;
        void *arg;
    } wrapper_t;
    wrapper_t *w = (wrapper_t *)arg;
    rtc_thread_fn fn = w->fn;
    void *real_arg = w->arg;
    free(w);
    fn(real_arg);
    return 0;
}

int rtc_thread_create(rtc_thread_t *t, rtc_thread_fn fn, void *arg) {
    typedef struct {
        rtc_thread_fn fn;
        void *arg;
    } wrapper_t;
    wrapper_t *w = (wrapper_t *)malloc(sizeof(wrapper_t));
    if (!w)
        return RTC_ERR_NOMEM;
    w->fn = fn;
    w->arg = arg;
    *t = CreateThread(NULL, 0, win_thread_wrapper, w, 0, NULL);
    if (*t == NULL) {
        free(w);
        return RTC_ERR_GENERIC;
    }
    return RTC_OK;
}

int rtc_thread_join(rtc_thread_t *t) {
    if (*t == NULL)
        return RTC_OK;
    WaitForSingleObject(*t, INFINITE);
    CloseHandle(*t);
    *t = NULL;
    return RTC_OK;
}

int rtc_mutex_init(rtc_mutex_t *m) {
    InitializeCriticalSection(m);
    return RTC_OK;
}
void rtc_mutex_destroy(rtc_mutex_t *m) {
    DeleteCriticalSection(m);
}
void rtc_mutex_lock(rtc_mutex_t *m) {
    EnterCriticalSection(m);
}
void rtc_mutex_unlock(rtc_mutex_t *m) {
    LeaveCriticalSection(m);
}

int rtc_cond_init(rtc_cond_t *c) {
    InitializeConditionVariable(c);
    return RTC_OK;
}
void rtc_cond_destroy(rtc_cond_t *c) {
    (void)c; /* no-op on Windows */
}
void rtc_cond_signal(rtc_cond_t *c) {
    WakeConditionVariable(c);
}
void rtc_cond_broadcast(rtc_cond_t *c) {
    WakeAllConditionVariable(c);
}

int rtc_cond_wait_timeout(rtc_cond_t *c, rtc_mutex_t *m, uint32_t timeout_ms) {
    if (SleepConditionVariableCS(c, m, timeout_ms))
        return RTC_OK;
    return (GetLastError() == ERROR_TIMEOUT) ? RTC_ERR_TIMEOUT : RTC_ERR_GENERIC;
}

#else /* POSIX */

int rtc_thread_create(rtc_thread_t *t, rtc_thread_fn fn, void *arg) {
    return (pthread_create(t, NULL, fn, arg) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_thread_join(rtc_thread_t *t) {
    return (pthread_join(*t, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}

int rtc_mutex_init(rtc_mutex_t *m) {
    return (pthread_mutex_init(m, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}
void rtc_mutex_destroy(rtc_mutex_t *m) {
    pthread_mutex_destroy(m);
}
void rtc_mutex_lock(rtc_mutex_t *m) {
    pthread_mutex_lock(m);
}
void rtc_mutex_unlock(rtc_mutex_t *m) {
    pthread_mutex_unlock(m);
}

int rtc_cond_init(rtc_cond_t *c) {
    return (pthread_cond_init(c, NULL) == 0) ? RTC_OK : RTC_ERR_GENERIC;
}
void rtc_cond_destroy(rtc_cond_t *c) {
    pthread_cond_destroy(c);
}
void rtc_cond_signal(rtc_cond_t *c) {
    pthread_cond_signal(c);
}
void rtc_cond_broadcast(rtc_cond_t *c) {
    pthread_cond_broadcast(c);
}

int rtc_cond_wait_timeout(rtc_cond_t *c, rtc_mutex_t *m, uint32_t timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == 0)
        return RTC_OK;
    return RTC_ERR_TIMEOUT;
}

#endif
