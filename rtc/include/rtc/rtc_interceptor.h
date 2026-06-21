/*
 * RTCP interceptor chain.
 *
 * A composable, ordered set of handlers for the per-peer RTCP plane. Each
 * interceptor observes inbound RTCP sub-packets and an optional periodic tick;
 * the media session builds a chain of built-in interceptors (report emission,
 * NACK responder, PLI/FIR responder) and applications may append their own
 * (e.g. REMB, RFC 8888, stats, logging) via rtc_media_session_add_interceptor().
 *
 * This replaces a hardcoded RTCP dispatch switch: handling a new feedback type
 * is adding an interceptor rather than editing the session.
 */
#ifndef RTC_INTERCEPTOR_H
#define RTC_INTERCEPTOR_H

#include "rtc_common.h"

typedef struct rtc_interceptor rtc_interceptor_t;

typedef struct {
    const char *name; /* for logging / debugging; may be NULL */

    /* One inbound RTCP sub-packet, already split out of a compound packet and
     * SRTCP-unprotected. `pt` is the packet type (e.g. 200 SR, 201 RR, 205
     * RTPFB, 206 PSFB) and `fmt` the feedback-message type (FMT/RC field). */
    void (*on_rtcp)(rtc_interceptor_t *it, uint8_t pt, uint8_t fmt, const uint8_t *buf, size_t len);

    /* Periodic tick, fired on the worker loop (e.g. for SR / RR emission). */
    void (*on_tick)(rtc_interceptor_t *it, uint64_t now_ms);

    /* Release interceptor-owned resources. The chain calls this on close for
     * every interceptor it owns. May be NULL. */
    void (*destroy)(rtc_interceptor_t *it);
} rtc_interceptor_ops_t;

/* Embed as the first member of a concrete interceptor so the chain can upcast
 * `rtc_interceptor_t *` back to the implementation. */
struct rtc_interceptor {
    const rtc_interceptor_ops_t *ops;
};

#define RTC_INTERCEPTOR_CHAIN_MAX 8

typedef struct {
    rtc_interceptor_t *items[RTC_INTERCEPTOR_CHAIN_MAX];
    int count;
} rtc_interceptor_chain_t;

void rtc_interceptor_chain_init(rtc_interceptor_chain_t *chain);

/* Append `it` to the chain. On success the chain takes ownership and destroys
 * `it` in rtc_interceptor_chain_close(). On failure (chain full) ownership stays
 * with the caller. */
rtc_err_t rtc_interceptor_chain_add(rtc_interceptor_chain_t *chain, rtc_interceptor_t *it);

/* Dispatch one inbound RTCP sub-packet to every interceptor in order. */
void rtc_interceptor_chain_on_rtcp(rtc_interceptor_chain_t *chain, uint8_t pt, uint8_t fmt,
                                   const uint8_t *buf, size_t len);

/* Fire the periodic tick on every interceptor that defines on_tick. */
void rtc_interceptor_chain_tick(rtc_interceptor_chain_t *chain, uint64_t now_ms);

/* Destroy every owned interceptor and reset the chain. Idempotent. */
void rtc_interceptor_chain_close(rtc_interceptor_chain_t *chain);

#endif /* RTC_INTERCEPTOR_H */
