/*
 * Factory for pluggable video codecs.
 */
#include "media/video_codec.h"
#include <stdlib.h>
#include <string.h>

/* VP8 backend (codec_vp8.c) */
extern const video_encoder_ops_t *vp8_get_encoder_ops(void);
extern const video_decoder_ops_t *vp8_get_decoder_ops(void);
extern size_t vp8_encoder_ctx_size(void);
extern size_t vp8_decoder_ctx_size(void);

int video_encoder_create(video_encoder_t *enc, const char *codec_name, int width, int height,
                         int bitrate_kbps, int fps) {
    memset(enc, 0, sizeof(*enc));

    if (strcmp(codec_name, "VP8") == 0) {
        enc->ops = vp8_get_encoder_ops();
        enc->ctx = calloc(1, vp8_encoder_ctx_size());
    }
    /* Future: else if (strcmp(codec_name, "H264") == 0) { ... } */
    else {
        return RTC_ERR_INVALID;
    }

    if (!enc->ctx)
        return RTC_ERR_NOMEM;
    int rc = enc->ops->init(enc->ctx, width, height, bitrate_kbps, fps);
    if (rc != RTC_OK) {
        free(enc->ctx);
        enc->ctx = NULL;
    }
    return rc;
}

int video_decoder_create(video_decoder_t *dec, const char *codec_name) {
    memset(dec, 0, sizeof(*dec));

    if (strcmp(codec_name, "VP8") == 0) {
        dec->ops = vp8_get_decoder_ops();
        dec->ctx = calloc(1, vp8_decoder_ctx_size());
    } else {
        return RTC_ERR_INVALID;
    }

    if (!dec->ctx)
        return RTC_ERR_NOMEM;
    int rc = dec->ops->init(dec->ctx);
    if (rc != RTC_OK) {
        free(dec->ctx);
        dec->ctx = NULL;
    }
    return rc;
}

void video_encoder_destroy(video_encoder_t *enc) {
    if (enc && enc->ctx && enc->ops) {
        enc->ops->close(enc->ctx);
        free(enc->ctx);
        enc->ctx = NULL;
    }
}

void video_decoder_destroy(video_decoder_t *dec) {
    if (dec && dec->ctx && dec->ops) {
        dec->ops->close(dec->ctx);
        free(dec->ctx);
        dec->ctx = NULL;
    }
}

/* Helper: allocate I420 frame */
int video_frame_alloc(video_frame_t *f, int width, int height) {
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)(y_size + 2 * uv_size));
    if (!buf)
        return RTC_ERR_NOMEM;

    f->planes[0] = buf;
    f->planes[1] = buf + y_size;
    f->planes[2] = buf + y_size + uv_size;
    f->stride[0] = width;
    f->stride[1] = width / 2;
    f->stride[2] = width / 2;
    f->width = width;
    f->height = height;
    return RTC_OK;
}

void video_frame_free(video_frame_t *f) {
    if (f && f->planes[0]) {
        free(f->planes[0]);
        memset(f, 0, sizeof(*f));
    }
}
