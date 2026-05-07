/*
 * Transport layer - Owns the UDP socket, runs an event-driven thread,
 * and demuxes incoming packets (STUN/DTLS/RTP) per RFC 7983.
 *
 * The transport provides:
 *  - Socket lifecycle (create, bind, close)
 *  - Background thread with platform-native I/O poller (epoll/kqueue/select)
 *  - Packet classification and callback dispatch
 *  - Timer management for protocol retransmissions
 */
#ifndef RTC_TRANSPORT_H
#define RTC_TRANSPORT_H

#include "rtc_common.h"
#include "rtc_poller.h"

#define RTC_TRANSPORT_MAX_TIMERS 16
#define RTC_TRANSPORT_BUF_SIZE   2048

/* Packet classification per RFC 7983 */
typedef enum {
    RTC_PKT_STUN,         /* first byte [0, 3]     */
    RTC_PKT_CHANNEL_DATA, /* first byte [64, 79] — TURN ChannelData */
    RTC_PKT_DTLS,         /* first byte [20, 63]   */
    RTC_PKT_RTP,          /* first byte [128, 191] */
    RTC_PKT_UNKNOWN,
} rtc_pkt_type_t;

/* Callback for received + classified packets (fires on transport thread) */
typedef void (*rtc_transport_recv_fn)(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                      const rtc_addr_t *from, void *user);

/* Timer callback */
typedef void (*rtc_timer_fn)(void *user);

/* Timer slot */
typedef struct {
    uint64_t deadline_ms; /* absolute time (rtc_time_ms) */
    rtc_timer_fn fn;
    void *user;
    bool active;
} rtc_timer_t;

/* Timer handle (index + generation for ABA safety) */
typedef int rtc_timer_id_t;

typedef struct rtc_transport {
    /* Socket */
    rtc_socket_t sock;
    rtc_addr_t local_addr;

    /* Poller */
    rtc_poller_t poller;

    /* Thread */
    rtc_thread_t thread;
    volatile bool running;
    rtc_mutex_t mutex;

    /* Packet callback */
    rtc_transport_recv_fn on_recv;
    void *recv_user;
    rtc_mutex_t recv_mutex; /* protects callback access */

    /* Timers */
    rtc_timer_t timers[RTC_TRANSPORT_MAX_TIMERS];
    int timer_count;

    /* Selected remote address (set by ICE after connectivity checks) */
    rtc_addr_t remote_addr;
    bool remote_addr_set;
} rtc_transport_t;

/*
 * Initialize transport: create UDP socket, bind to ephemeral port,
 * start background thread with I/O poller.
 */
int rtc_transport_init(rtc_transport_t *t);

/* Thread-safe: send data to a specific destination address */
int rtc_transport_send(rtc_transport_t *t, const uint8_t *data, size_t len, const rtc_addr_t *dest);

/* Thread-safe: send data to the selected remote (set by ICE) */
int rtc_transport_send_to_remote(rtc_transport_t *t, const uint8_t *data, size_t len);

/* Set the demuxed packet handler (fires on transport thread) */
void rtc_transport_set_recv_callback(rtc_transport_t *t, rtc_transport_recv_fn fn, void *user);

/* Set the selected remote address (called by ICE after connect) */
void rtc_transport_set_remote(rtc_transport_t *t, const rtc_addr_t *addr);

/*
 * Schedule a timer callback at absolute deadline (rtc_time_ms).
 * Returns timer ID >= 0, or < 0 on error.
 * Thread-safe.
 */
rtc_timer_id_t rtc_transport_add_timer(rtc_transport_t *t, uint64_t deadline_ms, rtc_timer_fn fn,
                                       void *user);

/* Cancel a pending timer. Thread-safe. */
void rtc_transport_cancel_timer(rtc_transport_t *t, rtc_timer_id_t id);

/* Get the underlying socket (for getsockname during ICE gathering) */
rtc_socket_t rtc_transport_get_socket(const rtc_transport_t *t);

/* Get the local bound address */
int rtc_transport_get_local_addr(rtc_transport_t *t, rtc_addr_t *out);

/* Stop thread, close socket, free resources */
void rtc_transport_close(rtc_transport_t *t);

#endif /* RTC_TRANSPORT_H */
