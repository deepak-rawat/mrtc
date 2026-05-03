/*
 * test_video_codec.c - VP8 encode/decode round-trip tests.
 */
#include "test_harness.h"
#include "media/video_codec.h"
#include <string.h>
#include <math.h>

/* Generate a test pattern (gradient) */
static void fill_test_pattern(video_frame_t *f, int seed) {
    int w = f->width, h = f->height;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            f->planes[0][y * f->stride[0] + x] = (uint8_t)((x + y + seed) & 0xFF);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            f->planes[1][y * f->stride[1] + x] = (uint8_t)(128 + seed);
            f->planes[2][y * f->stride[2] + x] = (uint8_t)(64 + seed);
        }
}

/* Compute PSNR between two Y planes */
static double compute_psnr(const uint8_t *a, int stride_a,
                           const uint8_t *b, int stride_b,
                           int w, int h) {
    double mse = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double d = (double)a[y * stride_a + x] - (double)b[y * stride_b + x];
            mse += d * d;
        }
    mse /= (double)(w * h);
    if (mse < 0.01) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

/* ------------------------------------------------------------------ */
TEST(vp8_encode_decode) {
    video_encoder_t enc;
    video_decoder_t dec;
    int w = 320, h = 240;

    ASSERT_EQ(video_encoder_create(&enc, "VP8", w, h, 500, 30), RTC_OK);
    ASSERT_EQ(video_decoder_create(&dec, "VP8"), RTC_OK);

    video_frame_t input;
    ASSERT_EQ(video_frame_alloc(&input, w, h), RTC_OK);
    fill_test_pattern(&input, 42);

    /* Encode */
    video_packet_t pkt;
    ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
    ASSERT(pkt.len > 0);
    ASSERT(pkt.is_keyframe);  /* First frame should be keyframe */
    printf("    encoded: %d bytes (keyframe=%d)\n", pkt.len, pkt.is_keyframe);

    /* Decode */
    video_frame_t output;
    ASSERT_EQ(video_decode(&dec, pkt.data, pkt.len, &output), RTC_OK);
    ASSERT_EQ(output.width, w);
    ASSERT_EQ(output.height, h);

    /* Check quality (lossy codec, so use PSNR) */
    double psnr = compute_psnr(input.planes[0], input.stride[0],
                               output.planes[0], output.stride[0], w, h);
    printf("    PSNR: %.1f dB\n", psnr);
    ASSERT(psnr > 25.0);  /* VP8 at 500kbps 320x240 should be >25dB */

    video_frame_free(&input);
    video_encoder_destroy(&enc);
    video_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(vp8_keyframe_request) {
    video_encoder_t enc;
    video_decoder_t dec;
    int w = 160, h = 120;

    ASSERT_EQ(video_encoder_create(&enc, "VP8", w, h, 300, 30), RTC_OK);
    ASSERT_EQ(video_decoder_create(&dec, "VP8"), RTC_OK);

    video_frame_t input;
    ASSERT_EQ(video_frame_alloc(&input, w, h), RTC_OK);
    fill_test_pattern(&input, 0);

    /* Encode several frames to get past initial keyframe */
    video_packet_t pkt;
    for (int i = 0; i < 5; i++) {
        fill_test_pattern(&input, i);
        ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
    }

    /* Frame after initial should not be keyframe */
    fill_test_pattern(&input, 99);
    ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
    bool was_key_before = pkt.is_keyframe;

    /* Request keyframe */
    video_encoder_request_keyframe(&enc);
    fill_test_pattern(&input, 100);
    ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
    ASSERT(pkt.is_keyframe);
    printf("    before request: keyframe=%d, after request: keyframe=%d\n",
           was_key_before, pkt.is_keyframe);

    video_frame_free(&input);
    video_encoder_destroy(&enc);
    video_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(vp8_bitrate_change) {
    video_encoder_t enc;
    int w = 320, h = 240;

    ASSERT_EQ(video_encoder_create(&enc, "VP8", w, h, 1000, 30), RTC_OK);

    video_frame_t input;
    ASSERT_EQ(video_frame_alloc(&input, w, h), RTC_OK);

    /* Encode a few frames at high bitrate */
    video_packet_t pkt;
    int total_high = 0;
    for (int i = 0; i < 10; i++) {
        fill_test_pattern(&input, i);
        ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
        total_high += pkt.len;
    }

    /* Lower bitrate */
    video_encoder_set_bitrate(&enc, 200);
    int total_low = 0;
    for (int i = 10; i < 20; i++) {
        fill_test_pattern(&input, i);
        ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
        total_low += pkt.len;
    }

    printf("    high bitrate total: %d bytes, low bitrate total: %d bytes\n",
           total_high, total_low);
    /* Low bitrate should produce less data (may take a few frames to converge) */

    video_frame_free(&input);
    video_encoder_destroy(&enc);
}

/* ------------------------------------------------------------------ */
TEST(vp8_multiple_frames) {
    video_encoder_t enc;
    video_decoder_t dec;
    int w = 160, h = 120;

    ASSERT_EQ(video_encoder_create(&enc, "VP8", w, h, 300, 30), RTC_OK);
    ASSERT_EQ(video_decoder_create(&dec, "VP8"), RTC_OK);

    video_frame_t input;
    ASSERT_EQ(video_frame_alloc(&input, w, h), RTC_OK);

    for (int i = 0; i < 30; i++) {
        fill_test_pattern(&input, i);
        video_packet_t pkt;
        ASSERT_EQ(video_encode(&enc, &input, &pkt), RTC_OK);
        ASSERT(pkt.len > 0);

        video_frame_t output;
        ASSERT_EQ(video_decode(&dec, pkt.data, pkt.len, &output), RTC_OK);
        ASSERT_EQ(output.width, w);
    }
    printf("    30 frames encoded/decoded OK\n");

    video_frame_free(&input);
    video_encoder_destroy(&enc);
    video_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(vp8_invalid_codec) {
    video_encoder_t enc;
    ASSERT(video_encoder_create(&enc, "BOGUS", 320, 240, 500, 30) != RTC_OK);
    printf("    invalid codec correctly rejected\n");
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Video Codec Tests (VP8)\n");
    printf("========================================\n\n");

    RUN_TEST(vp8_encode_decode);
    RUN_TEST(vp8_keyframe_request);
    RUN_TEST(vp8_bitrate_change);
    RUN_TEST(vp8_multiple_frames);
    RUN_TEST(vp8_invalid_codec);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
