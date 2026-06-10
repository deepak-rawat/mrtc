/*
 * Animated bouncing box test pattern.
 */
#include "media/test_pattern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int test_pattern_init(test_pattern_t *tp, int width, int height) {
    memset(tp, 0, sizeof(*tp));
    tp->width = width;
    tp->height = height;

    /* Allocate I420 frame buffer */
    if (video_frame_alloc(&tp->frame, width, height) != 0)
        return -1;

    /* Bouncing box: 1/4 of frame size */
    tp->box_w = width / 4;
    tp->box_h = height / 4;
    tp->box_x = width / 3;
    tp->box_y = height / 3;
    tp->dx = 3;
    tp->dy = 2;

    /* Random color (YUV space) — each instance gets a unique hue */
    uint8_t rnd[3];
    rtc_random_bytes(rnd, 3);
    tp->color_y = (uint8_t)(100 + (rnd[0] % 120)); /* 100-220: bright enough */
    tp->color_u = rnd[1];
    tp->color_v = rnd[2];

    return 0;
}

static void fill_rect_y(video_frame_t *f, int x0, int y0, int w, int h, uint8_t val) {
    for (int y = y0; y < y0 + h && y < f->height; y++)
        for (int x = x0; x < x0 + w && x < f->width; x++)
            f->planes[0][y * f->stride[0] + x] = val;
}

static void fill_rect_uv(video_frame_t *f, int x0, int y0, int w, int h, uint8_t u_val,
                         uint8_t v_val) {
    int cx = x0 / 2, cy = y0 / 2;
    int cw = w / 2, ch = h / 2;
    for (int y = cy; y < cy + ch && y < f->height / 2; y++) {
        for (int x = cx; x < cx + cw && x < f->width / 2; x++) {
            f->planes[1][y * f->stride[1] + x] = u_val;
            f->planes[2][y * f->stride[2] + x] = v_val;
        }
    }
}

/* Draw a simple digit (0-9) at position, 5x7 pixels in Y plane */
static const uint8_t digit_font[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, /* 2 */
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
};

static void draw_digit(video_frame_t *f, int x0, int y0, int digit, uint8_t val) {
    if (digit < 0 || digit > 9)
        return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = digit_font[digit][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                int px = x0 + col * 2;
                int py = y0 + row * 2;
                /* Draw 2x2 pixel for visibility */
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        if (py + dy < f->height && px + dx < f->width)
                            f->planes[0][(py + dy) * f->stride[0] + (px + dx)] = val;
            }
        }
    }
}

static void draw_number(video_frame_t *f, int x0, int y0, int number, uint8_t val) {
    /* Draw up to 6 digits */
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%d", number);
    for (int i = 0; i < n; i++)
        draw_digit(f, x0 + i * 12, y0, buf[i] - '0', val);
}

void test_pattern_next_frame(test_pattern_t *tp) {
    video_frame_t *f = &tp->frame;

    /* Clear to dark gray */
    memset(f->planes[0], 40, (size_t)(f->stride[0] * f->height));
    memset(f->planes[1], 128, (size_t)(f->stride[1] * (f->height / 2)));
    memset(f->planes[2], 128, (size_t)(f->stride[2] * (f->height / 2)));

    /* Move bouncing box */
    tp->box_x += tp->dx;
    tp->box_y += tp->dy;
    if (tp->box_x <= 0 || tp->box_x + tp->box_w >= tp->width)
        tp->dx = -tp->dx;
    if (tp->box_y <= 0 || tp->box_y + tp->box_h >= tp->height - 20)
        tp->dy = -tp->dy;
    /* Clamp */
    if (tp->box_x < 0)
        tp->box_x = 0;
    if (tp->box_y < 0)
        tp->box_y = 0;

    /* Draw colored box */
    fill_rect_y(f, tp->box_x, tp->box_y, tp->box_w, tp->box_h, tp->color_y);
    fill_rect_uv(f, tp->box_x, tp->box_y, tp->box_w, tp->box_h, tp->color_u, tp->color_v);

    /* Draw bottom bar (dark) */
    fill_rect_y(f, 0, tp->height - 18, tp->width, 18, 20);

    /* Draw frame counter in bottom bar */
    draw_number(f, 4, tp->height - 16, tp->frame_num, 220);

    tp->frame_num++;
}

const video_frame_t *test_pattern_get_frame(const test_pattern_t *tp) {
    return &tp->frame;
}

void test_pattern_close(test_pattern_t *tp) {
    video_frame_free(&tp->frame);
    memset(tp, 0, sizeof(*tp));
}
