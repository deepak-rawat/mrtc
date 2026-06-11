/*
 * AIMD (Additive Increase Multiplicative Decrease) bitrate adaptation.
 *
 * loss < 2%  → increase 5%
 * loss > 5%  → decrease 20%
 * RTT > 300ms → decrease 10%
 * loss > 10% → request keyframe
 */
#include "rtc/rtc_rate_control.h"
#include <stdatomic.h>
#include <stdlib.h>

rtc_rate_controller_t *rtc_rate_control_create(const rtc_rate_control_config_t *cfg) {
    rtc_rate_controller_t *rc = (rtc_rate_controller_t *)calloc(1, sizeof(*rc));
    if (!rc)
        return NULL;

    int initial = cfg->target_bitrate_kbps;
    rc->min_bitrate_kbps = cfg->min_bitrate_kbps > 0 ? cfg->min_bitrate_kbps : 100;
    rc->max_bitrate_kbps = cfg->max_bitrate_kbps > 0 ? cfg->max_bitrate_kbps : 2500;

    if (initial < rc->min_bitrate_kbps)
        initial = rc->min_bitrate_kbps;
    if (initial > rc->max_bitrate_kbps)
        initial = rc->max_bitrate_kbps;
    atomic_store_explicit(&rc->current_bitrate_kbps, initial, memory_order_relaxed);
    atomic_store_explicit(&rc->keyframe_requested, false, memory_order_relaxed);

    return rc;
}

void rtc_rate_control_on_rtcp_rr(rtc_rate_controller_t *rc, int fraction_lost, int rtt_ms,
                                 int jitter) {
    (void)jitter;

    /* Convert fraction_lost (0-255) to percentage */
    int loss_pct = (fraction_lost * 100) / 256;

    /* Only the transport thread runs this function, so we can safely
     * load-modify-store without CAS. Use relaxed for the load (we own it)
     * and release for the store so the encoder's acquire load observes it. */
    int bitrate = atomic_load_explicit(&rc->current_bitrate_kbps, memory_order_relaxed);

    if (loss_pct < 2) {
        /* Low loss: additive increase (+5%) */
        bitrate += bitrate / 20;
    } else if (loss_pct > 5) {
        /* High loss: multiplicative decrease (-20%) */
        bitrate -= bitrate / 5;
    }

    /* High RTT penalty */
    if (rtt_ms > 300) {
        bitrate -= bitrate / 10;
    }

    /* Loss spike: request keyframe */
    if (loss_pct > 10) {
        atomic_store_explicit(&rc->keyframe_requested, true, memory_order_release);
    }

    /* Clamp */
    if (bitrate < rc->min_bitrate_kbps)
        bitrate = rc->min_bitrate_kbps;
    if (bitrate > rc->max_bitrate_kbps)
        bitrate = rc->max_bitrate_kbps;

    atomic_store_explicit(&rc->current_bitrate_kbps, bitrate, memory_order_release);
}

int rtc_rate_control_get_bitrate(rtc_rate_controller_t *rc) {
    return atomic_load_explicit(&rc->current_bitrate_kbps, memory_order_acquire);
}

bool rtc_rate_control_should_keyframe(rtc_rate_controller_t *rc) {
    return atomic_exchange_explicit(&rc->keyframe_requested, false, memory_order_acq_rel);
}

void rtc_rate_control_destroy(rtc_rate_controller_t *rc) {
    free(rc);
}
