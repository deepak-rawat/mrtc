/*
 * rate_control.c - AIMD (Additive Increase Multiplicative Decrease) bitrate adaptation.
 *
 * loss < 2%  → increase 5%
 * loss > 5%  → decrease 20%
 * RTT > 300ms → decrease 10%
 * loss > 10% → request keyframe
 */
#include "rate_control.h"
#include <stdlib.h>

rate_controller_t *rate_control_create(const rate_control_config_t *cfg) {
    rate_controller_t *rc = (rate_controller_t *)calloc(1, sizeof(*rc));
    if (!rc)
        return NULL;

    rc->current_bitrate_kbps = cfg->target_bitrate_kbps;
    rc->min_bitrate_kbps = cfg->min_bitrate_kbps > 0 ? cfg->min_bitrate_kbps : 100;
    rc->max_bitrate_kbps = cfg->max_bitrate_kbps > 0 ? cfg->max_bitrate_kbps : 2500;

    if (rc->current_bitrate_kbps < rc->min_bitrate_kbps)
        rc->current_bitrate_kbps = rc->min_bitrate_kbps;
    if (rc->current_bitrate_kbps > rc->max_bitrate_kbps)
        rc->current_bitrate_kbps = rc->max_bitrate_kbps;

    return rc;
}

void rate_control_on_rtcp_rr(rate_controller_t *rc, int fraction_lost, int rtt_ms, int jitter) {
    (void)jitter;

    /* Convert fraction_lost (0-255) to percentage */
    int loss_pct = (fraction_lost * 100) / 256;

    if (loss_pct < 2) {
        /* Low loss: additive increase (+5%) */
        rc->current_bitrate_kbps += rc->current_bitrate_kbps / 20;
    } else if (loss_pct > 5) {
        /* High loss: multiplicative decrease (-20%) */
        rc->current_bitrate_kbps -= rc->current_bitrate_kbps / 5;
    }

    /* High RTT penalty */
    if (rtt_ms > 300) {
        rc->current_bitrate_kbps -= rc->current_bitrate_kbps / 10;
    }

    /* Loss spike: request keyframe */
    if (loss_pct > 10) {
        rc->keyframe_requested = true;
    }

    /* Clamp */
    if (rc->current_bitrate_kbps < rc->min_bitrate_kbps)
        rc->current_bitrate_kbps = rc->min_bitrate_kbps;
    if (rc->current_bitrate_kbps > rc->max_bitrate_kbps)
        rc->current_bitrate_kbps = rc->max_bitrate_kbps;
}

int rate_control_get_bitrate(rate_controller_t *rc) {
    return rc->current_bitrate_kbps;
}

bool rate_control_should_keyframe(rate_controller_t *rc) {
    if (rc->keyframe_requested) {
        rc->keyframe_requested = false;
        return true;
    }
    return false;
}

void rate_control_destroy(rate_controller_t *rc) {
    free(rc);
}
