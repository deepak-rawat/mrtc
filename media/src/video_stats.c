/*
 * video_stats.c — Per-stream video statistics and PSNR.
 */
#include "media/video_stats.h"
#include "media/video_codec.h"
#include <math.h>
#include <string.h>

void video_send_stats_init(video_send_stats_t *s) {
    memset(s, 0, sizeof(*s));
}

void video_recv_stats_init(video_recv_stats_t *s) {
    memset(s, 0, sizeof(*s));
}

double video_frame_psnr(const video_frame_t *ref, const video_frame_t *test) {
    if (!ref || !test)
        return 0.0;
    if (ref->width != test->width || ref->height != test->height)
        return 0.0;

    int w = ref->width, h = ref->height;
    uint64_t sse = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *r = ref->planes[0] + y * ref->stride[0];
        const uint8_t *t = test->planes[0] + y * test->stride[0];
        for (int x = 0; x < w; x++) {
            int d = (int)r[x] - (int)t[x];
            sse += (uint64_t)(d * d);
        }
    }
    if (sse == 0)
        return INFINITY;
    return 10.0 * log10(255.0 * 255.0 / ((double)sse / (double)(w * h)));
}
