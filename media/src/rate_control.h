/*
 * rate_control.h - AIMD bitrate adaptation from RTCP feedback.
 */
#ifndef MEDIA_RATE_CONTROL_H
#define MEDIA_RATE_CONTROL_H

#include <stdbool.h>

typedef struct {
    int target_bitrate_kbps; /* Configured target */
    int min_bitrate_kbps;    /* Floor (e.g., 100) */
    int max_bitrate_kbps;    /* Ceiling (e.g., 2500) */
} rate_control_config_t;

typedef struct rate_controller {
    int current_bitrate_kbps;
    int min_bitrate_kbps;
    int max_bitrate_kbps;
    bool keyframe_requested;
} rate_controller_t;

rate_controller_t *rate_control_create(const rate_control_config_t *cfg);

/* Called when RTCP RR arrives */
void rate_control_on_rtcp_rr(rate_controller_t *rc, int fraction_lost, /* 0-255 (0=no loss,
                                                                          255=100%) */
                             int rtt_ms, int jitter);

/* Called before each encode — returns recommended bitrate in kbps */
int rate_control_get_bitrate(rate_controller_t *rc);

/* Returns true and clears the flag if a keyframe was requested */
bool rate_control_should_keyframe(rate_controller_t *rc);

void rate_control_destroy(rate_controller_t *rc);

#endif /* MEDIA_RATE_CONTROL_H */
