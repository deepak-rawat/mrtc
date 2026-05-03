/*
 * test_pattern.h - Animated video test pattern generator.
 *
 * Generates a bouncing colored box on a dark background with a frame counter.
 * Each instance gets a random color so different peers are visually distinct.
 * No camera required.
 */
#ifndef MEDIA_TEST_PATTERN_H
#define MEDIA_TEST_PATTERN_H

#include "video_codec.h"
#include <stdint.h>

typedef struct {
    int width, height;
    int frame_num;

    /* Bouncing box state */
    int box_x, box_y;
    int box_w, box_h;
    int dx, dy;
    uint8_t color_y, color_u, color_v; /* Random per-instance */

    /* Internal I420 buffer */
    video_frame_t frame;
} test_pattern_t;

int test_pattern_init(test_pattern_t *tp, int width, int height);
void test_pattern_next_frame(test_pattern_t *tp);
const video_frame_t *test_pattern_get_frame(const test_pattern_t *tp);
void test_pattern_close(test_pattern_t *tp);

#endif /* MEDIA_TEST_PATTERN_H */
