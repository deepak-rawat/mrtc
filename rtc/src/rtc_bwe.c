/*
 * Google Congestion Control — simplified C implementation.
 * See rtc_bwe.h for design notes.
 */
#include "rtc_bwe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BURST_WINDOW_US      5000   /* group packets within 5ms */
#define TRENDLINE_WINDOW     20     /* samples in linear regression */
#define TREND_THRESHOLD_INIT 12.5   /* initial overuse threshold (ms accumulated) */
#define TREND_K_UP           0.0087 /* threshold adapt up gain */
#define TREND_K_DOWN         0.039  /* threshold adapt down gain */
#define BACKOFF_FACTOR       0.85   /* multiplicative decrease */
#define ADDITIVE_INC_BPS     50000  /* +50 kbps per RTT in additive mode */
#define MULT_INC_FACTOR      1.08   /* +8% per RTT in multiplicative mode */
#define CHANGE_THRESHOLD     0.03   /* 3% change fires callback */
#define CHANGE_KEEPALIVE_MS  1000

struct rtc_bwe {
    rtc_bwe_config_t cfg;

    /* Delay-based estimator */
    rtc_bwe_state_t state;
    uint64_t last_burst_send_us;
    uint64_t last_burst_recv_us;
    uint32_t burst_size_bytes;
    int has_prev_burst;

    /* Accumulated inter-group delay; the trendline filter regresses
     * accumulated_delay (ms) vs. group_index. */
    double accumulated_delay_ms;
    int group_index;
    double tl_x[TRENDLINE_WINDOW];
    double tl_y[TRENDLINE_WINDOW];
    int tl_count;
    int tl_pos; /* circular */

    double trend_threshold_ms;
    double last_slope_per_group;

    /* Rate controller */
    uint32_t delay_estimate_bps;
    uint32_t loss_estimate_bps;
    uint32_t current_target_bps;
    uint64_t last_increase_us;
    uint64_t last_decrease_us;
    bool in_additive_mode;

    /* Recent received throughput estimate (bytes per ms, EWMA over bursts) */
    double recent_throughput_bytes_per_ms;

    /* Callback */
    rtc_bwe_on_bitrate_fn on_bitrate;
    void *on_bitrate_user;
    uint32_t last_reported_bps;
    uint64_t last_callback_us;
};

rtc_bwe_t *rtc_bwe_create(const rtc_bwe_config_t *cfg) {
    if (!cfg || cfg->min_bps == 0 || cfg->max_bps < cfg->min_bps)
        return NULL;
    rtc_bwe_t *b = (rtc_bwe_t *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->cfg = *cfg;
    b->state = RTC_BWE_NORMAL;
    b->trend_threshold_ms = TREND_THRESHOLD_INIT;
    b->delay_estimate_bps = cfg->initial_bps;
    /* Loss estimator starts unbounded (max_bps) so it does not cap the
     * delay-based estimate until actual loss is observed. */
    b->loss_estimate_bps = cfg->max_bps;
    b->current_target_bps = cfg->initial_bps;
    b->last_reported_bps = cfg->initial_bps;
    b->in_additive_mode = false;
    return b;
}

void rtc_bwe_destroy(rtc_bwe_t *b) {
    if (b)
        free(b);
}

void rtc_bwe_on_bitrate_change(rtc_bwe_t *b, rtc_bwe_on_bitrate_fn fn, void *user) {
    if (!b)
        return;
    b->on_bitrate = fn;
    b->on_bitrate_user = user;
}

rtc_bwe_state_t rtc_bwe_state(const rtc_bwe_t *b) {
    return b ? b->state : RTC_BWE_NORMAL;
}

uint32_t rtc_bwe_target_bitrate(const rtc_bwe_t *b) {
    return b ? b->current_target_bps : 0;
}

static uint32_t clamp_bps(const rtc_bwe_t *b, uint32_t v) {
    if (v < b->cfg.min_bps)
        v = b->cfg.min_bps;
    if (v > b->cfg.max_bps)
        v = b->cfg.max_bps;
    return v;
}

/* Push (x, y) into the trendline window and return the slope by simple
 * linear regression (least squares). slope is in y-units per x-unit
 * (here ms-per-group). */
static double trendline_push(rtc_bwe_t *b, double x, double y) {
    int pos = b->tl_pos;
    b->tl_x[pos] = x;
    b->tl_y[pos] = y;
    b->tl_pos = (pos + 1) % TRENDLINE_WINDOW;
    if (b->tl_count < TRENDLINE_WINDOW)
        b->tl_count++;

    int n = b->tl_count;
    if (n < 3)
        return 0.0;

    double sum_x = 0, sum_y = 0;
    for (int i = 0; i < n; i++) {
        sum_x += b->tl_x[i];
        sum_y += b->tl_y[i];
    }
    double mean_x = sum_x / n, mean_y = sum_y / n;
    double num = 0, den = 0;
    for (int i = 0; i < n; i++) {
        double dx = b->tl_x[i] - mean_x;
        num += dx * (b->tl_y[i] - mean_y);
        den += dx * dx;
    }
    if (den == 0.0)
        return 0.0;
    return num / den;
}

/* Update overuse detector state from slope. Threshold adapts: shrinks when
 * slope is small (more sensitive), grows when slope is large (less reactive). */
static void detector_update(rtc_bwe_t *b, double slope_ms_per_group) {
    /* Modulated trend: slope × N (signal strength). */
    double modulated = fabs(slope_ms_per_group) * (double)b->tl_count;

    if (slope_ms_per_group > 0 && modulated > b->trend_threshold_ms) {
        b->state = RTC_BWE_OVERUSE;
    } else if (slope_ms_per_group < 0 && modulated > b->trend_threshold_ms) {
        b->state = RTC_BWE_UNDERUSE;
    } else {
        b->state = RTC_BWE_NORMAL;
    }

    /* Adapt threshold (RFC 8298-ish). */
    double k = (modulated < b->trend_threshold_ms) ? TREND_K_DOWN : TREND_K_UP;
    double delta = k * (modulated - b->trend_threshold_ms);
    b->trend_threshold_ms += delta;
    if (b->trend_threshold_ms < 6.0)
        b->trend_threshold_ms = 6.0;
    if (b->trend_threshold_ms > 600.0)
        b->trend_threshold_ms = 600.0;

    b->last_slope_per_group = slope_ms_per_group;
}

/* Rate controller — pick the new delay-based estimate from current state. */
static void rate_control_step(rtc_bwe_t *b, uint64_t now_us) {
    switch (b->state) {
        case RTC_BWE_OVERUSE: {
            double tput_bps = b->recent_throughput_bytes_per_ms * 8000.0;
            uint32_t new_bps = (uint32_t)(tput_bps * BACKOFF_FACTOR);
            if (new_bps < b->cfg.min_bps)
                new_bps = b->cfg.min_bps;
            /* Only decrease (no rapid up-down oscillation). */
            if (new_bps < b->delay_estimate_bps)
                b->delay_estimate_bps = new_bps;
            b->last_decrease_us = now_us;
            b->in_additive_mode = true;
            break;
        }
        case RTC_BWE_UNDERUSE:
            /* Hold during underuse — queue is draining. */
            break;
        case RTC_BWE_NORMAL: {
            /* Use additive after recent decrease, otherwise multiplicative. */
            uint64_t since_dec_ms =
                (now_us > b->last_decrease_us) ? (now_us - b->last_decrease_us) / 1000ULL : 0;
            if (since_dec_ms > 3000)
                b->in_additive_mode = false;

            uint32_t prev = b->delay_estimate_bps;
            if (b->in_additive_mode) {
                b->delay_estimate_bps = clamp_bps(b, prev + ADDITIVE_INC_BPS);
            } else {
                b->delay_estimate_bps = clamp_bps(b, (uint32_t)(prev * MULT_INC_FACTOR));
            }
            b->last_increase_us = now_us;
            break;
        }
    }
}

static void finalize_target(rtc_bwe_t *b, uint64_t now_us) {
    uint32_t t = b->delay_estimate_bps;
    if (b->loss_estimate_bps < t)
        t = b->loss_estimate_bps;
    t = clamp_bps(b, t);
    b->current_target_bps = t;

    /* Fire callback if changed > 3% or 1 second elapsed. */
    if (b->on_bitrate) {
        uint64_t since_cb_ms =
            (now_us > b->last_callback_us) ? (now_us - b->last_callback_us) / 1000ULL : 0;
        uint32_t diff =
            (t > b->last_reported_bps) ? t - b->last_reported_bps : b->last_reported_bps - t;
        bool big_change =
            (b->last_reported_bps > 0) && ((double)diff / b->last_reported_bps > CHANGE_THRESHOLD);
        if (big_change || since_cb_ms > CHANGE_KEEPALIVE_MS) {
            b->on_bitrate(t, b->on_bitrate_user);
            b->last_reported_bps = t;
            b->last_callback_us = now_us;
        }
    }
}

void rtc_bwe_on_packet_feedback(rtc_bwe_t *b, uint64_t send_time_us, uint64_t recv_time_us,
                                uint16_t pkt_size) {
    if (!b)
        return;
    if (recv_time_us == 0)
        return; /* lost packet — only loss estimator consumes */

    /* Group packets into bursts (≤5 ms by send time). */
    if (!b->has_prev_burst) {
        b->last_burst_send_us = send_time_us;
        b->last_burst_recv_us = recv_time_us;
        b->burst_size_bytes = pkt_size;
        b->has_prev_burst = 1;
        return;
    }

    int64_t send_dt = (int64_t)send_time_us - (int64_t)b->last_burst_send_us;
    if (send_dt < BURST_WINDOW_US) {
        /* Same burst — accumulate size, advance the last-recv to the latest
         * arrival in this burst (which is what GCC compares against). */
        b->burst_size_bytes += pkt_size;
        if (recv_time_us > b->last_burst_recv_us)
            b->last_burst_recv_us = recv_time_us;
        return;
    }

    /* New burst: compute inter-group delay. */
    int64_t recv_dt = (int64_t)recv_time_us - (int64_t)b->last_burst_recv_us;
    int64_t delay_us = recv_dt - send_dt;
    double delay_ms = (double)delay_us / 1000.0;

    b->accumulated_delay_ms += delay_ms;
    b->group_index++;

    /* Recent throughput (bytes / ms) — EWMA over previous burst's size. */
    double send_dt_ms = (double)send_dt / 1000.0;
    if (send_dt_ms > 0) {
        double tput = (double)b->burst_size_bytes / send_dt_ms;
        b->recent_throughput_bytes_per_ms =
            (b->recent_throughput_bytes_per_ms == 0.0)
                ? tput
                : 0.95 * b->recent_throughput_bytes_per_ms + 0.05 * tput;
    }

    /* Trendline + detector. */
    double slope = trendline_push(b, (double)b->group_index, b->accumulated_delay_ms);
    detector_update(b, slope);
    rate_control_step(b, send_time_us);
    finalize_target(b, send_time_us);

    /* Start the new burst. */
    b->last_burst_send_us = send_time_us;
    b->last_burst_recv_us = recv_time_us;
    b->burst_size_bytes = pkt_size;
}

void rtc_bwe_on_loss(rtc_bwe_t *b, uint8_t fraction_lost_q8) {
    if (!b)
        return;
    double loss = (double)fraction_lost_q8 / 256.0;
    uint32_t prev = b->loss_estimate_bps;
    if (loss < 0.02)
        b->loss_estimate_bps = clamp_bps(b, (uint32_t)(prev * 1.05));
    else if (loss > 0.10)
        b->loss_estimate_bps = clamp_bps(b, (uint32_t)(prev * (1.0 - 0.5 * loss)));
    /* 2..10%: hold */
    finalize_target(b, b->last_increase_us /* monotonic-ish */);
}
