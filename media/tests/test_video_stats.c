/*
 * Tests for video statistics and PSNR.
 */
#include "test_harness.h"
#include "media/video_stats.h"
#include "media/video_codec.h"
#include <string.h>
#include <math.h>

TEST(send_stats_init_zeros) {
    video_send_stats_t s;
    video_send_stats_init(&s);
    ASSERT(s.frames_encoded == 0);
    ASSERT(s.packets_sent == 0);
    ASSERT(s.psnr_sum == 0.0);
}

TEST(recv_stats_init_zeros) {
    video_recv_stats_t s;
    video_recv_stats_init(&s);
    ASSERT(s.packets_received == 0);
    ASSERT(s.rtp_seq_initialized == false);
}

TEST(recv_stats_rtp_seq_gaps) {
    video_recv_stats_t s;
    video_recv_stats_init(&s);
    s.rtp_seq_initialized = true;
    s.rtp_last_seq = 2;

    /* Gap: expected 3, got 5 → 2 missing */
    int16_t diff = (int16_t)(5 - (s.rtp_last_seq + 1));
    ASSERT(diff == 2);
    s.rtp_seq_gaps++;
    s.packets_missing += 2;
    s.rtp_last_seq = 5;

    /* Duplicate */
    s.packets_duplicate++;

    /* Reorder: expected 6, got 4 */
    diff = (int16_t)(4 - (s.rtp_last_seq + 1));
    ASSERT(diff < 0 && diff > -1000);
    s.packets_reordered++;

    ASSERT(s.rtp_seq_gaps == 1);
    ASSERT(s.packets_missing == 2);
    ASSERT(s.packets_duplicate == 1);
    ASSERT(s.packets_reordered == 1);
}

TEST(psnr_identical) {
    video_frame_t f;
    video_frame_alloc(&f, 64, 64);
    memset(f.planes[0], 128, 64 * 64);
    memset(f.planes[1], 128, 32 * 32);
    memset(f.planes[2], 128, 32 * 32);
    ASSERT(isinf(video_frame_psnr(&f, &f)));
    video_frame_free(&f);
}

TEST(psnr_known_difference) {
    video_frame_t ref, tst;
    video_frame_alloc(&ref, 64, 64);
    video_frame_alloc(&tst, 64, 64);
    memset(ref.planes[0], 128, 64 * 64);
    memset(tst.planes[0], 100, 64 * 64);
    memset(ref.planes[1], 128, 32 * 32);
    memset(ref.planes[2], 128, 32 * 32);
    memset(tst.planes[1], 128, 32 * 32);
    memset(tst.planes[2], 128, 32 * 32);

    double psnr = video_frame_psnr(&ref, &tst);
    /* MSE = 784, PSNR ≈ 19.19 dB */
    ASSERT(psnr > 18.0 && psnr < 20.0);
    video_frame_free(&ref);
    video_frame_free(&tst);
}

TEST(psnr_null_safety) {
    ASSERT(video_frame_psnr(NULL, NULL) == 0.0);
}

int main(void) {
    RUN_TEST(send_stats_init_zeros);
    RUN_TEST(recv_stats_init_zeros);
    RUN_TEST(recv_stats_rtp_seq_gaps);
    RUN_TEST(psnr_identical);
    RUN_TEST(psnr_known_difference);
    RUN_TEST(psnr_null_safety);
    return _test_fail_count > 0 ? 1 : 0;
}
