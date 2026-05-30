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

#include <stdatomic.h>

#define RTC_TRANSPORT_MAX_TIMERS 16

/* Recv buffer sized for jumbo frames so DTLS records / RTP packets near
 * the jumbo MTU are not silently truncated. */
#define RTC_TRANSPORT_BUF_SIZE   9216

/* Max packets drained per poll wakeup before yielding to timers.
 * Bounds worst-case timer lateness under packet bursts. */
#define RTC_TRANSPORT_RECV_BATCH 16

/* Packet classification per RFC 7983 */
typedef enum {
    RTC_PKT_STUN,         /* first byte [0, 3]     */
    RTC_PKT_CHANNEL_DATA, /* first byte [64, 79] — TURN ChannelData */
    RTC_PKT_DTLS,         /* first byte [20, 63]   */
    RTC_PKT_RTP,          /* first byte [128, 191], byte[1] PT < 200 */
    RTC_PKT_RTCP,         /* first byte [128, 191], byte[1] PT 200-204 */
    RTC_PKT_UNKNOWN,
} rtc_pkt_type_t;

/* Callback for received + classified packets (fires on transport thread).
 *
 * CONTRACT: must complete quickly (target < 1 ms). The transport thread
 * also drives timer scheduling for protocol retransmissions (DTLS, ICE,
 * RTCP, TWCC), so any time spent inside on_recv directly delays timer
 * firing and back-pressures the receive path.
 *
 * Offload heavy work (video decoding, file I/O, application logic) to a
 * worker thread by enqueuing in this callback and returning immediately.
 */
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
    /* Socket. Atomic so concurrent senders that race with close see
     * RTC_INVALID_SOCKET and return an error instead of using a freed fd. */
    _Atomic rtc_socket_t sock;
    rtc_addr_t local_addr;

    /* Poller */
    rtc_poller_t poller;

    /* Thread */
    rtc_thread_t thread;
    _Atomic bool running; /* set false from any thread to stop poller loop */
    rtc_mutex_t mutex;

    /* Packet callback. Installed by rtc_transport_init() before the
     * background thread starts; never changed. Read unlocked on the hot
     * path. */
    rtc_transport_recv_fn on_recv;
    void *recv_user;

    /* Timers */
    rtc_timer_t timers[RTC_TRANSPORT_MAX_TIMERS];
    int timer_count;

    /* Selected remote address (set by ICE after connectivity checks).
     *
     * One-shot publication: ICE writes remote_addr exactly once, then
     * publishes via a release-store to remote_addr_set. Senders do an
     * acquire-load on the flag; if true, they are guaranteed to see the
     * fully-written address. No lock needed on either side.
     *
     * If ICE restart / candidate switching is ever added (the remote may
     * change after publication), switch to a seqlock or take a mutex on
     * both write and read sides to prevent torn reads of remote_addr. */
    rtc_addr_t remote_addr;
    _Atomic bool remote_addr_set;
} rtc_transport_t;

/*
 * Create UDP socket, install the recv callback, and start the poller
 * thread. on_recv (may be NULL) fires on the transport thread.
 */
int rtc_transport_init(rtc_transport_t *t, rtc_transport_recv_fn on_recv, void *user);

/* Thread-safe: send data to a specific destination address */
int rtc_transport_send(rtc_transport_t *t, const uint8_t *data, size_t len, const rtc_addr_t *dest);

/* Thread-safe: send data to the selected remote (set by ICE) */
int rtc_transport_send_to_remote(rtc_transport_t *t, const uint8_t *data, size_t len);

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
