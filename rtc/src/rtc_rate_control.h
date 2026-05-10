/*
 * rtc_rate_control.h - AIMD bitrate adaptation from RTCP feedback.
 */
#ifndef RTC_RATE_CONTROL_H
#define RTC_RATE_CONTROL_H

#include <stdbool.h>

typedef struct {
    int target_bitrate_kbps; /* Configured target */
    int min_bitrate_kbps;    /* Floor (e.g., 100) */
    int max_bitrate_kbps;    /* Ceiling (e.g., 2500) */
} rtc_rate_control_config_t;

typedef struct rtc_rate_controller {
    int current_bitrate_kbps;
    int min_bitrate_kbps;
    int max_bitrate_kbps;
    bool keyframe_requested;
} rtc_rate_controller_t;

rtc_rate_controller_t *rtc_rate_control_create(const rtc_rate_control_config_t *cfg);

/* Called when RTCP RR arrives */
void rtc_rate_control_on_rtcp_rr(rtc_rate_controller_t *rc, int fraction_lost, /* 0-255 (0=no loss,
                                                                                  255=100%) */
                                 int rtt_ms, int jitter);

/* Called before each encode — returns recommended bitrate in kbps */
int rtc_rate_control_get_bitrate(rtc_rate_controller_t *rc);

/* Returns true and clears the flag if a keyframe was requested */
bool rtc_rate_control_should_keyframe(rtc_rate_controller_t *rc);

void rtc_rate_control_destroy(rtc_rate_controller_t *rc);

#endif /* RTC_RATE_CONTROL_H */
