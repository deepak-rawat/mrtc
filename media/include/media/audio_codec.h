/*
 * Pluggable audio codec interface via vtable.
 */
#ifndef MEDIA_AUDIO_CODEC_H
#define MEDIA_AUDIO_CODEC_H

#include <rtc/rtc_common.h>
#include <stdint.h>

/* Raw audio frame */
typedef struct {
    int16_t *samples; /* Interleaved PCM */
    int sample_count; /* Per channel */
    int sample_rate;  /* e.g. 48000 */
    int channels;     /* 1 or 2 */
} audio_frame_t;

/* Audio encoder vtable */
typedef struct audio_encoder_ops {
    int (*init)(void *ctx, int sample_rate, int channels, int bitrate_bps);
    int (*encode)(void *ctx, const audio_frame_t *frame, uint8_t *out, int *out_len);
    void (*close)(void *ctx);
} audio_encoder_ops_t;

/* Audio decoder vtable */
typedef struct audio_decoder_ops {
    int (*init)(void *ctx, int sample_rate, int channels);
    int (*decode)(void *ctx, const uint8_t *data, int len, audio_frame_t *out);
    void (*close)(void *ctx);
} audio_decoder_ops_t;

typedef struct {
    void *ctx;
    const audio_encoder_ops_t *ops;
} audio_encoder_t;

typedef struct {
    void *ctx;
    const audio_decoder_ops_t *ops;
} audio_decoder_t;

int audio_encoder_create(audio_encoder_t *enc, const char *codec_name, int sample_rate,
                         int channels, int bitrate_bps);
int audio_decoder_create(audio_decoder_t *dec, const char *codec_name, int sample_rate,
                         int channels);
void audio_encoder_destroy(audio_encoder_t *enc);
void audio_decoder_destroy(audio_decoder_t *dec);

/* Convenience wrappers */
static inline int audio_encode(audio_encoder_t *enc, const audio_frame_t *frame, uint8_t *out,
                               int *out_len) {
    return enc->ops->encode(enc->ctx, frame, out, out_len);
}
static inline int audio_decode(audio_decoder_t *dec, const uint8_t *data, int len,
                               audio_frame_t *out) {
    return dec->ops->decode(dec->ctx, data, len, out);
}

#endif /* MEDIA_AUDIO_CODEC_H */
