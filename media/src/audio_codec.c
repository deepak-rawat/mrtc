/*
 * audio_codec.c - Factory for pluggable audio codecs.
 */
#include "media/audio_codec.h"
#include <stdlib.h>
#include <string.h>

/* Opus backend (codec_opus.c) */
extern const audio_encoder_ops_t *opus_get_encoder_ops(void);
extern const audio_decoder_ops_t *opus_get_decoder_ops(void);
extern size_t opus_encoder_ctx_size(void);
extern size_t opus_decoder_ctx_size(void);

int audio_encoder_create(audio_encoder_t *enc, const char *codec_name, int sample_rate,
                         int channels, int bitrate_bps) {
    memset(enc, 0, sizeof(*enc));

    if (strcmp(codec_name, "opus") == 0) {
        enc->ops = opus_get_encoder_ops();
        enc->ctx = calloc(1, opus_encoder_ctx_size());
    } else {
        return RTC_ERR_INVALID;
    }

    if (!enc->ctx)
        return RTC_ERR_NOMEM;
    int rc = enc->ops->init(enc->ctx, sample_rate, channels, bitrate_bps);
    if (rc != RTC_OK) {
        free(enc->ctx);
        enc->ctx = NULL;
    }
    return rc;
}

int audio_decoder_create(audio_decoder_t *dec, const char *codec_name, int sample_rate,
                         int channels) {
    memset(dec, 0, sizeof(*dec));

    if (strcmp(codec_name, "opus") == 0) {
        dec->ops = opus_get_decoder_ops();
        dec->ctx = calloc(1, opus_decoder_ctx_size());
    } else {
        return RTC_ERR_INVALID;
    }

    if (!dec->ctx)
        return RTC_ERR_NOMEM;
    int rc = dec->ops->init(dec->ctx, sample_rate, channels);
    if (rc != RTC_OK) {
        free(dec->ctx);
        dec->ctx = NULL;
    }
    return rc;
}

void audio_encoder_destroy(audio_encoder_t *enc) {
    if (enc && enc->ctx && enc->ops) {
        enc->ops->close(enc->ctx);
        free(enc->ctx);
        enc->ctx = NULL;
    }
}

void audio_decoder_destroy(audio_decoder_t *dec) {
    if (dec && dec->ctx && dec->ops) {
        dec->ops->close(dec->ctx);
        free(dec->ctx);
        dec->ctx = NULL;
    }
}
