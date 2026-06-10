/*
 * Audio sine tone generator for testing.
 */
#ifndef MEDIA_TEST_TONE_H
#define MEDIA_TEST_TONE_H

#include "audio_codec.h"
#include <stdint.h>

typedef struct {
    int sample_rate;
    int channels;
    int frequency;   /* Hz, default 440 */
    float amplitude; /* 0.0-1.0, default 0.1 */
    int phase_sample;
    int16_t *buffer;
    int frame_size; /* samples per channel per frame */
} test_tone_t;

int test_tone_init(test_tone_t *tt, int sample_rate, int channels, int freq);
void test_tone_next_frame(test_tone_t *tt, audio_frame_t *out);
void test_tone_close(test_tone_t *tt);

#endif /* MEDIA_TEST_TONE_H */
