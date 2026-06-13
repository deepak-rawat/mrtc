/*
 * Packet I/O - Owns the UDP socket, sends/receives datagrams, and
 * demuxes incoming packets (STUN/DTLS/RTP) per RFC 7983.
 *
 * This object is PASSIVE: it does not own a thread or a timer scheduler.
 * The owning rtc_worker registers the socket with its poller and calls
 * rtc_packet_io_drain() when the socket becomes readable. All recv
 * callbacks therefore fire on the worker thread.
 *
 * Provides:
 *  - Socket lifecycle (create, bind, close)
 *  - A non-blocking drain pass that classifies packets and dispatches
 *    them to the registered callback
 *  - Thread-safe send helpers
 */
#ifndef RTC_PACKET_IO_H
#define RTC_PACKET_IO_H

#include "rtc_common.h"

#include <stdatomic.h>

/* Recv buffer sized for jumbo frames so DTLS records / RTP packets near
 * the jumbo MTU are not silently truncated. */
#define RTC_PACKET_IO_BUF_SIZE 9216

/* Max packets drained per poll wakeup before yielding to timers.
 * Bounds worst-case timer lateness under packet bursts. */
#define RTC_PACKET_IO_RECV_BATCH 16

/* Requested kernel socket buffer size (bytes). The kernel may clamp this
 * to a system maximum; the actual value is logged at init time. */
#define RTC_PACKET_IO_SOCKBUF_BYTES (1 * 1024 * 1024)

/* Packet classification per RFC 7983 */
typedef enum {
    RTC_PKT_STUN,         /* first byte [0, 3]     */
    RTC_PKT_CHANNEL_DATA, /* first byte [64, 79] — TURN ChannelData */
    RTC_PKT_DTLS,         /* first byte [20, 63]   */
    RTC_PKT_RTP,          /* first byte [128, 191], byte[1] PT < 200 */
    RTC_PKT_RTCP,         /* first byte [128, 191], byte[1] PT 200-206 */
    RTC_PKT_UNKNOWN,
} rtc_pkt_type_t;

/* Callback for received + classified packets (fires on the worker thread).
 *
 * CONTRACT: must complete quickly (target < 1 ms). The worker thread
 * that drains the socket also drives timer scheduling for protocol
 * retransmissions (DTLS, ICE, RTCP, TWCC), so any time spent inside
 * on_recv directly delays timer firing and back-pressures the receive
 * path.
 *
 * Offload heavy work (video decoding, file I/O, application logic) to a
 * separate thread by enqueuing in this callback and returning immediately.
 *
 * BUFFER LIFETIME: `data` points into a transport-owned buffer that is
 * reused for the NEXT packet. The callee MUST NOT retain `data` (or
 * `from`) after returning. Copy into the callee's own storage if the
 * payload needs to outlive the callback.
 */
typedef void (*rtc_packet_io_recv_fn)(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                      const rtc_addr_t *from, void *user);

typedef struct {
    const char *listen_ip;
    uint16_t port;
} rtc_packet_io_config_t;

typedef struct rtc_packet_io {
    /* Socket. Atomic so concurrent senders that race with close see
     * RTC_INVALID_SOCKET and return an error instead of using a freed fd. */
    _Atomic rtc_socket_t sock;
    rtc_addr_t local_addr;

    /* Packet callback. Installed by rtc_packet_io_init() before the
     * socket is registered with a worker; never changed afterwards. */
    rtc_packet_io_recv_fn on_recv;
    void *recv_user;

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

    /* Lightweight counters (atomic so reads from any thread are valid).
     * recv_drain_full counts how often the recv loop hit RECV_BATCH and
     * left packets in the kernel buffer — a signal of sustained bursts.
     *
     * Backpressure: a non-zero / growing recv_drain_full or
     * recv_kernel_drops indicates the network is delivering faster than
     * we can process. Higher layers (BWE, pacer) should monitor these
     * via rtc_packet_io_get_stats() and reduce send rate when they
     * grow. No callback hook yet — polling is sufficient at the cadence
     * BWE already runs (sub-second). */
    _Atomic uint64_t pkts_recv;
    _Atomic uint64_t bytes_recv;
    _Atomic uint64_t pkts_sent;
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t send_errors;
    _Atomic uint64_t send_would_block;
    _Atomic uint64_t recv_drain_full;
    _Atomic uint64_t recv_kernel_drops;

    /* Source-pinning hint, populated from IPV6_PKTINFO cmsg on each recv
     * (POSIX). The 128-bit address is intentionally NOT atomic: a torn
     * read just means a single send goes out via the kernel default
     * source (the prior behavior), which is no worse than skipping
     * source pinning. last_local_valid gates whether the address is
     * meaningful at all. */
    uint8_t last_local_v6[16];
    _Atomic bool last_local_valid;

    /* Linux recvmmsg batch arena. Allocated in init, freed in close.
     * Sized for RTC_PACKET_IO_RECV_BATCH packets at full BUF_SIZE so a
     * single syscall can pull a whole burst. NULL on non-Linux. */
    void *recv_batch_arena;
} rtc_packet_io_t;

/* Snapshot of transport counters. */
typedef struct {
    uint64_t pkts_recv;
    uint64_t bytes_recv;
    uint64_t pkts_sent;
    uint64_t bytes_sent;
    uint64_t send_errors;
    uint64_t send_would_block; /* EAGAIN/EWOULDBLOCK on sendto; transient */
    uint64_t recv_drain_full;
    uint64_t recv_kernel_drops; /* SO_RXQ_OVFL counter (Linux only; else 0) */
} rtc_packet_io_stats_t;

/*
 * Create and bind the UDP socket and install the recv callback. Does NOT
 * start any thread: the owning worker drives I/O by polling the socket
 * and calling rtc_packet_io_drain(). on_recv (may be NULL) fires on
 * whichever thread calls rtc_packet_io_drain().
 */
int rtc_packet_io_init(rtc_packet_io_t *t, rtc_packet_io_recv_fn on_recv, void *user);
int rtc_packet_io_init_ex(rtc_packet_io_t *t, const rtc_packet_io_config_t *cfg,
                          rtc_packet_io_recv_fn on_recv, void *user);

/*
 * Drain up to RTC_PACKET_IO_RECV_BATCH datagrams from the socket without
 * blocking, classifying each and dispatching to the recv callback.
 * Intended to be called by the owning worker when the socket is readable.
 * Returns the number of datagrams processed (0 when the socket is empty).
 */
int rtc_packet_io_drain(rtc_packet_io_t *t);

/* Thread-safe: send data to a specific destination address */
int rtc_packet_io_send(rtc_packet_io_t *t, const uint8_t *data, size_t len, const rtc_addr_t *dest);

/* Thread-safe: send data to the selected remote (set by ICE) */
int rtc_packet_io_send_to_remote(rtc_packet_io_t *t, const uint8_t *data, size_t len);

/* Set the selected remote address (called by ICE after connect) */
void rtc_packet_io_set_remote(rtc_packet_io_t *t, const rtc_addr_t *addr);

/* Get the underlying socket (for getsockname during ICE gathering) */
rtc_socket_t rtc_packet_io_get_socket(const rtc_packet_io_t *t);

/* Get the local bound address */
int rtc_packet_io_get_local_addr(rtc_packet_io_t *t, rtc_addr_t *out);

/* Read a snapshot of internal counters. Lock-free. */
void rtc_packet_io_get_stats(const rtc_packet_io_t *t, rtc_packet_io_stats_t *out);

/* Close socket, free resources */
void rtc_packet_io_close(rtc_packet_io_t *t);

#endif /* RTC_PACKET_IO_H */
