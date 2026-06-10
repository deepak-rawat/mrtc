/*
 * Frame integrity checksum.
 */
#include "video_debug.h"

uint32_t video_frame_checksum(const video_frame_t *frame) {
    if (!frame || !frame->planes[0])
        return 0;

    uint32_t hash = 2166136261u; /* FNV offset basis */
    int w = frame->width, h = frame->height;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = frame->planes[0] + y * frame->stride[0];
        for (int x = 0; x < w; x++) {
            hash ^= row[x];
            hash *= 16777619u; /* FNV prime */
        }
    }
    return hash;
}
