/*
 * test_audio_codec.c - Opus encode/decode round-trip tests.
 */
#include "test_harness.h"
#include "media/audio_codec.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Generate a sine wave test signal */
static void fill_sine(int16_t *buf, int samples, int channels, int freq, int rate) {
    for (int i = 0; i < samples; i++) {
        int16_t val = (int16_t)(16000.0 * sin(2.0 * 3.14159265 * freq * i / rate));
        for (int c = 0; c < channels; c++)
            buf[i * channels + c] = val;
    }
}

/* ------------------------------------------------------------------ */
TEST(opus_encode_decode) {
    audio_encoder_t enc;
    audio_decoder_t dec;
    int sr = 48000, ch = 1;

    ASSERT_EQ(audio_encoder_create(&enc, "opus", sr, ch, 32000), RTC_OK);
    ASSERT_EQ(audio_decoder_create(&dec, "opus", sr, ch), RTC_OK);

    /* 20ms frame = 960 samples at 48kHz */
    int frame_samples = 960;
    int16_t *pcm_in = (int16_t *)malloc((size_t)(frame_samples * ch) * sizeof(int16_t));
    fill_sine(pcm_in, frame_samples, ch, 440, sr);

    audio_frame_t in_frame = {
        .samples = pcm_in,
        .sample_count = frame_samples,
        .sample_rate = sr,
        .channels = ch,
    };

    /* Encode */
    uint8_t opus_pkt[4000];
    int opus_len = (int)sizeof(opus_pkt);
    ASSERT_EQ(audio_encode(&enc, &in_frame, opus_pkt, &opus_len), RTC_OK);
    ASSERT(opus_len > 0);
    printf("    encoded: %d bytes from %d samples\n", opus_len, frame_samples);

    /* Decode */
    audio_frame_t out_frame;
    ASSERT_EQ(audio_decode(&dec, opus_pkt, opus_len, &out_frame), RTC_OK);
    ASSERT_EQ(out_frame.sample_count, frame_samples);
    ASSERT_EQ(out_frame.channels, ch);
    printf("    decoded: %d samples\n", out_frame.sample_count);

    free(pcm_in);
    audio_encoder_destroy(&enc);
    audio_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(opus_stereo) {
    audio_encoder_t enc;
    audio_decoder_t dec;
    int sr = 48000, ch = 2;

    ASSERT_EQ(audio_encoder_create(&enc, "opus", sr, ch, 64000), RTC_OK);
    ASSERT_EQ(audio_decoder_create(&dec, "opus", sr, ch), RTC_OK);

    int frame_samples = 960;
    int16_t *pcm = (int16_t *)malloc((size_t)(frame_samples * ch) * sizeof(int16_t));
    fill_sine(pcm, frame_samples, ch, 1000, sr);

    audio_frame_t in_frame = {
        .samples = pcm, .sample_count = frame_samples, .sample_rate = sr, .channels = ch};
    uint8_t pkt[4000];
    int pkt_len = (int)sizeof(pkt);
    ASSERT_EQ(audio_encode(&enc, &in_frame, pkt, &pkt_len), RTC_OK);

    audio_frame_t out;
    ASSERT_EQ(audio_decode(&dec, pkt, pkt_len, &out), RTC_OK);
    ASSERT_EQ(out.sample_count, frame_samples);
    ASSERT_EQ(out.channels, ch);
    printf("    stereo round-trip: %d samples, %d bytes\n", out.sample_count, pkt_len);

    free(pcm);
    audio_encoder_destroy(&enc);
    audio_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(opus_packet_loss) {
    audio_encoder_t enc;
    audio_decoder_t dec;
    int sr = 48000, ch = 1;

    ASSERT_EQ(audio_encoder_create(&enc, "opus", sr, ch, 32000), RTC_OK);
    ASSERT_EQ(audio_decoder_create(&dec, "opus", sr, ch), RTC_OK);

    int frame_samples = 960;
    int16_t *pcm = (int16_t *)malloc((size_t)(frame_samples * ch) * sizeof(int16_t));
    fill_sine(pcm, frame_samples, ch, 440, sr);

    /* Encode and decode one good frame */
    audio_frame_t in_frame = {
        .samples = pcm, .sample_count = frame_samples, .sample_rate = sr, .channels = ch};
    uint8_t pkt[4000];
    int pkt_len = (int)sizeof(pkt);
    ASSERT_EQ(audio_encode(&enc, &in_frame, pkt, &pkt_len), RTC_OK);

    audio_frame_t out;
    ASSERT_EQ(audio_decode(&dec, pkt, pkt_len, &out), RTC_OK);

    /* PLC: decode with NULL data (simulates lost packet) */
    audio_frame_t plc_out;
    ASSERT_EQ(audio_decode(&dec, NULL, 0, &plc_out), RTC_OK);
    ASSERT(plc_out.sample_count > 0);
    printf("    PLC output: %d samples (expected ~%d)\n", plc_out.sample_count, frame_samples);

    free(pcm);
    audio_encoder_destroy(&enc);
    audio_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(opus_multiple_frames) {
    audio_encoder_t enc;
    audio_decoder_t dec;
    int sr = 48000, ch = 1;

    ASSERT_EQ(audio_encoder_create(&enc, "opus", sr, ch, 32000), RTC_OK);
    ASSERT_EQ(audio_decoder_create(&dec, "opus", sr, ch), RTC_OK);

    int frame_samples = 960;
    int16_t *pcm = (int16_t *)malloc((size_t)(frame_samples * ch) * sizeof(int16_t));

    for (int i = 0; i < 50; i++) {
        fill_sine(pcm, frame_samples, ch, 440 + i * 10, sr);
        audio_frame_t in = {
            .samples = pcm, .sample_count = frame_samples, .sample_rate = sr, .channels = ch};
        uint8_t pkt[4000];
        int pkt_len = (int)sizeof(pkt);
        ASSERT_EQ(audio_encode(&enc, &in, pkt, &pkt_len), RTC_OK);

        audio_frame_t out;
        ASSERT_EQ(audio_decode(&dec, pkt, pkt_len, &out), RTC_OK);
    }
    printf("    50 frames encoded/decoded OK\n");

    free(pcm);
    audio_encoder_destroy(&enc);
    audio_decoder_destroy(&dec);
}

/* ------------------------------------------------------------------ */
TEST(opus_invalid_codec) {
    audio_encoder_t enc;
    ASSERT(audio_encoder_create(&enc, "BOGUS", 48000, 1, 32000) != RTC_OK);
    printf("    invalid codec correctly rejected\n");
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Audio Codec Tests (Opus)\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(opus_encode_decode);
    RUN_TEST(opus_stereo);
    RUN_TEST(opus_packet_loss);
    RUN_TEST(opus_multiple_frames);
    RUN_TEST(opus_invalid_codec);

    TEST_SUMMARY();
    return _test_fail_count > 0 ? 1 : 0;
}
