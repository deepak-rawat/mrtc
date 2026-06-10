/*
 * 440Hz sine tone generator.
 */
#include "media/test_tone.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

int test_tone_init(test_tone_t *tt, int sample_rate, int channels, int freq) {
    memset(tt, 0, sizeof(*tt));
    tt->sample_rate = sample_rate;
    tt->channels = channels;
    tt->frequency = freq > 0 ? freq : 440;
    tt->amplitude = 0.1f;              /* Quiet — just enough to see level bars */
    tt->frame_size = sample_rate / 50; /* 20ms frames */

    tt->buffer = (int16_t *)malloc((size_t)(tt->frame_size * channels) * sizeof(int16_t));
    if (!tt->buffer)
        return -1;

    return 0;
}

void test_tone_next_frame(test_tone_t *tt, audio_frame_t *out) {
    for (int i = 0; i < tt->frame_size; i++) {
        double t = (double)(tt->phase_sample + i) / (double)tt->sample_rate;
        int16_t val = (int16_t)(tt->amplitude * 32767.0 * sin(2.0 * M_PI * tt->frequency * t));
        for (int c = 0; c < tt->channels; c++)
            tt->buffer[i * tt->channels + c] = val;
    }
    tt->phase_sample += tt->frame_size;

    out->samples = tt->buffer;
    out->sample_count = tt->frame_size;
    out->sample_rate = tt->sample_rate;
    out->channels = tt->channels;
}

void test_tone_close(test_tone_t *tt) {
    free(tt->buffer);
    memset(tt, 0, sizeof(*tt));
}
