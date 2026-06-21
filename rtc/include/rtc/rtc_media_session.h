/*
 * Per-peer RTP/RTCP media session.
 *
 * Owns the send and receive streams for one transport and drives the whole
 * per-stream media plane on top of it:
 *   - installs the transport's RTP router: a per-SSRC sink that delivers to the
 *     bound receive stream, plus a payload-type resolver for packets whose SSRC
 *     was not signalled (peers that omit a=ssrc);
 *   - handles inbound RTCP (via the transport's on_rtcp) and routes each report
 *     to the right stream -- SR to the receive stream, RR / NACK / PLI / FIR to
 *     the send stream -- and feeds RR loss into the transport bandwidth
 *     estimator;
 *   - runs the periodic SR / RR emission timer.
 *
 * The transport stays a generic encrypted endpoint; the session is the
 * reusable per-peer media layer on top of it. Not thread safe: streams are
 * registered before connect, and dispatch / emission run on the worker loop.
 */
#ifndef RTC_MEDIA_SESSION_H
#define RTC_MEDIA_SESSION_H

#include "rtc_common.h"
#include "rtc_interceptor.h"
#include "rtc_rtp_stream.h"
#include "rtc_transport.h"
#include "rtc_u32_map.h"
#include "rtc_vec.h"
#include "rtc_worker.h"

#include <stdatomic.h>

typedef struct {
    rtc_transport_t *transport;
    rtc_worker_t *worker;
    rtc_u32_map_t send_streams; /* ssrc -> rtc_rtp_send_stream_t* (RTCP feedback + SR) */
    rtc_vec_t recv_streams;     /* rtc_rtp_recv_stream_t* (RTP resolve + RR emission) */
    rtc_interceptor_chain_t chain; /* RTCP handling: report, NACK, PLI + custom */
    rtc_worker_timer_t report_timer;
    _Atomic bool running;
    bool ready;
} rtc_media_session_t;

/* Initialize a session over `transport` (whose timers run on `worker`). Installs
 * the transport's RTP router and on_rtcp handler. `transport`/`worker` may be
 * NULL for unit-testing the RTCP routing in isolation (then SR routing, the RR
 * loss feed, and the emission timer are inert). */
rtc_err_t rtc_media_session_init(rtc_media_session_t *s, rtc_transport_t *transport,
                                 rtc_worker_t *worker);

/* Register a send stream so RTCP RR / NACK / PLI naming its SSRC route to it and
 * the SR timer emits for it. */
rtc_err_t rtc_media_session_add_sender(rtc_media_session_t *s, rtc_rtp_send_stream_t *stream);

/* Register a receive stream for payload-type resolution and RR emission. */
rtc_err_t rtc_media_session_add_receiver(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream);

/* Bind a receive stream to a signalled SSRC (from a=ssrc) so inbound RTP and SR
 * dispatch in O(1) without the resolver. */
void rtc_media_session_bind_receiver(rtc_media_session_t *s, rtc_rtp_recv_stream_t *stream,
                                     uint32_t ssrc);

/* Append a custom RTCP interceptor (e.g. REMB, RFC 8888, stats). The session
 * takes ownership and destroys it in rtc_media_session_close(); on failure
 * (chain full) ownership stays with the caller. Interceptors run after the
 * built-in report / NACK / PLI handlers. */
rtc_err_t rtc_media_session_add_interceptor(rtc_media_session_t *s, rtc_interceptor_t *it);

/* Route one compound RTCP packet (post SRTCP-unprotect). Normally invoked
 * internally from the transport's on_rtcp; exposed for testing. */
void rtc_media_session_handle_rtcp(rtc_media_session_t *s, const uint8_t *buf, size_t len);

/* Start the periodic SR / RR emission timer (call once connected). */
void rtc_media_session_start(rtc_media_session_t *s);

/* Stop the emission timer. Call before the transport / worker are torn down. */
void rtc_media_session_stop(rtc_media_session_t *s);

/* Free the stream registries. Call after the transport has been destroyed (so
 * no dispatch races the free). Idempotent. */
void rtc_media_session_close(rtc_media_session_t *s);

#endif /* RTC_MEDIA_SESSION_H */
