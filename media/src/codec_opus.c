/*
 * Opus audio codec backend using libopus.
 */
#include "media/audio_codec.h"
#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    OpusEncoder *enc;
    int sample_rate;
    int channels;
    int frame_size; /* samples per channel per encode call */
} opus_encoder_ctx_t;

static int opus_enc_init(void *ctx, int sample_rate, int channels, int bitrate_bps) {
    opus_encoder_ctx_t *e = (opus_encoder_ctx_t *)ctx;
    int err;
    e->enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !e->enc)
        return RTC_ERR_GENERIC;

    e->sample_rate = sample_rate;
    e->channels = channels;
    e->frame_size = sample_rate / 50; /* 20ms frames: 48000/50 = 960 */

    opus_encoder_ctl(e->enc, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(e->enc, OPUS_SET_COMPLEXITY(5)); /* Balanced */
    opus_encoder_ctl(e->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e->enc, OPUS_SET_DTX(1)); /* Discontinuous transmission */

    return RTC_OK;
}

static int opus_enc_encode(void *ctx, const audio_frame_t *frame, uint8_t *out, int *out_len) {
    opus_encoder_ctx_t *e = (opus_encoder_ctx_t *)ctx;
    int max_out = 4000; /* Opus max packet is ~4000 bytes */
    if (*out_len < max_out)
        max_out = *out_len;

    int n = opus_encode(e->enc, frame->samples, frame->sample_count, out, max_out);
    if (n < 0)
        return RTC_ERR_GENERIC;

    *out_len = n;
    return RTC_OK;
}

static void opus_enc_close(void *ctx) {
    opus_encoder_ctx_t *e = (opus_encoder_ctx_t *)ctx;
    if (e->enc)
        opus_encoder_destroy(e->enc);
}

static const audio_encoder_ops_t opus_encoder_ops = {
    .init = opus_enc_init,
    .encode = opus_enc_encode,
    .close = opus_enc_close,
};

typedef struct {
    OpusDecoder *dec;
    int sample_rate;
    int channels;
    int16_t *pcm_buf;
    int pcm_buf_size;
} opus_decoder_ctx_t;

static int opus_dec_init(void *ctx, int sample_rate, int channels) {
    opus_decoder_ctx_t *d = (opus_decoder_ctx_t *)ctx;
    int err;
    d->dec = opus_decoder_create(sample_rate, channels, &err);
    if (err != OPUS_OK || !d->dec)
        return RTC_ERR_GENERIC;

    d->sample_rate = sample_rate;
    d->channels = channels;
    /* Max 120ms Opus frame = 5760 samples at 48kHz */
    d->pcm_buf_size = 5760 * channels;
    d->pcm_buf = (int16_t *)malloc((size_t)d->pcm_buf_size * sizeof(int16_t));
    if (!d->pcm_buf)
        return RTC_ERR_NOMEM;

    return RTC_OK;
}

static int opus_dec_decode(void *ctx, const uint8_t *data, int len, audio_frame_t *out) {
    opus_decoder_ctx_t *d = (opus_decoder_ctx_t *)ctx;

    /* data=NULL triggers PLC (packet loss concealment) */
    int samples = opus_decode(d->dec, data, len, d->pcm_buf, d->pcm_buf_size / d->channels, 0);
    if (samples < 0)
        return RTC_ERR_GENERIC;

    out->samples = d->pcm_buf;
    out->sample_count = samples;
    out->sample_rate = d->sample_rate;
    out->channels = d->channels;
    return RTC_OK;
}

static void opus_dec_close(void *ctx) {
    opus_decoder_ctx_t *d = (opus_decoder_ctx_t *)ctx;
    if (d->dec)
        opus_decoder_destroy(d->dec);
    free(d->pcm_buf);
}

static const audio_decoder_ops_t opus_decoder_ops = {
    .init = opus_dec_init,
    .decode = opus_dec_decode,
    .close = opus_dec_close,
};

const audio_encoder_ops_t *opus_get_encoder_ops(void) {
    return &opus_encoder_ops;
}
const audio_decoder_ops_t *opus_get_decoder_ops(void) {
    return &opus_decoder_ops;
}
size_t opus_encoder_ctx_size(void) {
    return sizeof(opus_encoder_ctx_t);
}
size_t opus_decoder_ctx_size(void) {
    return sizeof(opus_decoder_ctx_t);
}
