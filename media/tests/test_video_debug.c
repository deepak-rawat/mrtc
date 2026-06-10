/*
 * Checksum tests.
 */
#include "test_harness.h"
#include "video_debug.h"
#include "media/video_codec.h"
#include <string.h>

TEST(checksum_deterministic) {
    video_frame_t f;
    video_frame_alloc(&f, 64, 64);
    memset(f.planes[0], 128, 64 * 64);
    ASSERT(video_frame_checksum(&f) == video_frame_checksum(&f));
    ASSERT(video_frame_checksum(&f) != 0);
    video_frame_free(&f);
}

TEST(checksum_sensitive) {
    video_frame_t f;
    video_frame_alloc(&f, 64, 64);
    memset(f.planes[0], 128, 64 * 64);
    uint32_t before = video_frame_checksum(&f);
    f.planes[0][100] = 0;
    ASSERT(video_frame_checksum(&f) != before);
    video_frame_free(&f);
}

TEST(checksum_null) {
    ASSERT(video_frame_checksum(NULL) == 0);
}

int main(void) {
    RUN_TEST(checksum_deterministic);
    RUN_TEST(checksum_sensitive);
    RUN_TEST(checksum_null);
    return _test_fail_count > 0 ? 1 : 0;
}
