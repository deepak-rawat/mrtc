/*
 * Common types, platform abstractions, and error codes for mrtc.
 */
#ifndef RTC_COMMON_H
#define RTC_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------- Platform socket abstraction ---------- */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET rtc_socket_t;
#  define RTC_INVALID_SOCKET INVALID_SOCKET
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <netdb.h>
typedef int rtc_socket_t;
#  define RTC_INVALID_SOCKET (-1)
#endif

/* ---------- Error codes ---------- */
typedef enum {
    RTC_OK = 0,
    RTC_ERR_GENERIC = -1,
    RTC_ERR_NOMEM = -2,
    RTC_ERR_SOCKET = -3,
    RTC_ERR_TIMEOUT = -4,
    RTC_ERR_INVALID = -5,
    RTC_ERR_SSL = -6,
    RTC_ERR_SRTP = -7,
    RTC_ERR_ICE = -8,
    RTC_ERR_SDP = -9,
} rtc_err_t;

/* ---------- Session description basics ---------- */
#define SDP_MAX_SIZE 8192

typedef enum {
    RTC_SDP_OFFER,
    RTC_SDP_ANSWER,
    RTC_SDP_PRANSWER,
} rtc_sdp_type_t;

/* ---------- Network address ---------- */
typedef struct {
    struct sockaddr_storage addr;
    socklen_t len;
} rtc_addr_t;

/* Helper to fill rtc_addr_t from ip string + port */
int rtc_addr_from_string(rtc_addr_t *out, const char *ip, uint16_t port);
/* Helper to format rtc_addr_t to string */
int rtc_addr_to_string(const rtc_addr_t *a, char *buf, size_t buflen, uint16_t *port_out);

/* ---------- Utility ---------- */
/* Portable close socket */
void rtc_close_socket(rtc_socket_t s);
/* Set socket non-blocking */
int rtc_set_nonblocking(rtc_socket_t s);
/* Generate cryptographic random bytes */
int rtc_random_bytes(uint8_t *buf, size_t len);
/* Generate a random alphanumeric string (null-terminated, len includes nul) */
int rtc_random_string(char *buf, size_t len);
/* Current monotonic time in milliseconds */
uint64_t rtc_time_ms(void);
/* Current monotonic time in microseconds */
uint64_t rtc_time_us(void);

/* ---------- Threading ---------- */
#ifdef _WIN32
typedef HANDLE rtc_thread_t;
typedef CRITICAL_SECTION rtc_mutex_t;
typedef CONDITION_VARIABLE rtc_cond_t;
#else
#  include <pthread.h>
typedef pthread_t rtc_thread_t;
typedef pthread_mutex_t rtc_mutex_t;
typedef pthread_cond_t rtc_cond_t;
#endif

typedef void *(*rtc_thread_fn)(void *arg);

int rtc_thread_create(rtc_thread_t *t, rtc_thread_fn fn, void *arg);
int rtc_thread_join(rtc_thread_t *t);

int rtc_mutex_init(rtc_mutex_t *m);
void rtc_mutex_destroy(rtc_mutex_t *m);
void rtc_mutex_lock(rtc_mutex_t *m);
void rtc_mutex_unlock(rtc_mutex_t *m);

int rtc_cond_init(rtc_cond_t *c);
void rtc_cond_destroy(rtc_cond_t *c);
void rtc_cond_signal(rtc_cond_t *c);
/* Wait with timeout in milliseconds. Returns RTC_OK or RTC_ERR_TIMEOUT. */
int rtc_cond_wait_timeout(rtc_cond_t *c, rtc_mutex_t *m, uint32_t timeout_ms);

/* ---------- Logging ---------- */
typedef enum {
    RTC_LOG_ERROR = 0,
    RTC_LOG_WARN = 1,
    RTC_LOG_INFO = 2,
    RTC_LOG_DEBUG = 3,
} rtc_log_level_t;

void rtc_set_log_level(rtc_log_level_t level);

/*
 * Direct log output to a file. Pass NULL to disable file logging.
 * If `log_to_stderr` is true, logs continue going to stderr as well.
 * Returns RTC_OK on success, RTC_ERR_GENERIC if the file cannot be opened.
 * The caller should call rtc_log_close() before exit to flush and close.
 */
int rtc_set_log_file(const char *path, bool log_to_stderr);
void rtc_log_close(void);

void rtc_log(rtc_log_level_t level, const char *fmt, ...);

#define RTC_LOG_ERR(...)  rtc_log(RTC_LOG_ERROR, __VA_ARGS__)
#define RTC_LOG_WARN(...) rtc_log(RTC_LOG_WARN, __VA_ARGS__)
#define RTC_LOG_INFO(...) rtc_log(RTC_LOG_INFO, __VA_ARGS__)
#define RTC_LOG_DBG(...)  rtc_log(RTC_LOG_DEBUG, __VA_ARGS__)

#endif /* RTC_COMMON_H */
