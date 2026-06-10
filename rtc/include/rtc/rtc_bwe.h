/*
 * Google Congestion Control (GCC) bandwidth estimator.
 *
 * Simplified C implementation of draft-ietf-rmcat-gcc:
 *   - Group packets by send time into ≤5 ms bursts.
 *   - For each pair of consecutive bursts compute the inter-group delay
 *     delta = (recv_delta − send_delta).
 *   - Feed accumulated delay into a trendline filter (linear regression
 *     over a sliding window of N samples). The slope (ms/group) is the
 *     overuse signal.
 *   - Adaptive threshold drives a {NORMAL, OVERUSE, UNDERUSE} signal.
 *   - Rate controller state machine:
 *        NORMAL   → multiplicative or additive increase
 *        OVERUSE  → decrease to β · max_recent_received_throughput
 *        UNDERUSE → hold
 *   - Loss-based estimator (Loss controller, §5.5):
 *        loss < 2%   → +5%
 *        2% ≤ loss ≤ 10%  → hold
 *        loss > 10%  → × (1 − 0.5·loss)
 *   - Final target = min(delay_estimate, loss_estimate), clamped to
 *     [min_bps, max_bps]. The user-supplied callback fires when the
 *     target changes by more than 3% or after 1s elapsed.
 */
#ifndef RTC_BWE_H
#define RTC_BWE_H

#include "rtc_common.h"

typedef struct rtc_bwe rtc_bwe_t;

typedef void (*rtc_bwe_on_bitrate_fn)(uint32_t bitrate_bps, void *user);

typedef struct {
    uint32_t initial_bps;
    uint32_t min_bps;
    uint32_t max_bps;
} rtc_bwe_config_t;

rtc_bwe_t *rtc_bwe_create(const rtc_bwe_config_t *cfg);
void rtc_bwe_destroy(rtc_bwe_t *b);

/* Feed one TWCC feedback item: per-packet (send_time_us, recv_time_us,
 * wire_size). recv_time_us == 0 means the packet was reported as lost; the
 * BWE does not consume it for delay (use rtc_bwe_on_loss for loss signal).
 * Call in ascending twcc_seq order. */
void rtc_bwe_on_packet_feedback(rtc_bwe_t *b, uint64_t send_time_us, uint64_t recv_time_us,
                                uint16_t pkt_size);

/* Loss signal (typically from RR fraction_lost, value 0..255 = 0..100%). */
void rtc_bwe_on_loss(rtc_bwe_t *b, uint8_t fraction_lost_q8);

/* Current target bitrate (after both delay + loss estimators). */
uint32_t rtc_bwe_target_bitrate(const rtc_bwe_t *b);

/* Register a callback that fires when the target bitrate changes
 * significantly (>3%) or after 1 second of no change. */
void rtc_bwe_on_bitrate_change(rtc_bwe_t *b, rtc_bwe_on_bitrate_fn fn, void *user);

/* For tests: directly inspect internal state. */
typedef enum { RTC_BWE_NORMAL = 0, RTC_BWE_OVERUSE, RTC_BWE_UNDERUSE } rtc_bwe_state_t;
rtc_bwe_state_t rtc_bwe_state(const rtc_bwe_t *b);

#endif /* RTC_BWE_H */
