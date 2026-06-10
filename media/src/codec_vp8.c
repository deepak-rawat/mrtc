/*
 * VP8 codec backend using libvpx.
 */
#include "media/video_codec.h"
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    vpx_codec_ctx_t codec;
    vpx_image_t img;
    int width, height;
    int pts;
    int fps;
    bool force_keyframe;
    uint8_t *out_buf;
    int out_buf_size;
} vp8_encoder_ctx_t;

static int vp8_enc_init(void *ctx, int w, int h, int bitrate_kbps, int fps) {
    vp8_encoder_ctx_t *e = (vp8_encoder_ctx_t *)ctx;
    e->width = w;
    e->height = h;
    e->fps = fps;
    e->pts = 0;
    e->force_keyframe = false;

    vpx_codec_enc_cfg_t cfg;
    vpx_codec_err_t res = vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0);
    if (res)
        return RTC_ERR_GENERIC;

    cfg.g_w = (unsigned int)w;
    cfg.g_h = (unsigned int)h;
    cfg.rc_target_bitrate = (unsigned int)bitrate_kbps;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = fps;
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    cfg.g_lag_in_frames = 0;    /* Real-time: no look-ahead */
    cfg.rc_end_usage = VPX_CBR; /* Constant bitrate for WebRTC */
    cfg.kf_max_dist = fps * 5;  /* Keyframe every 5 seconds */
    cfg.g_threads = 1;

    res = vpx_codec_enc_init(&e->codec, vpx_codec_vp8_cx(), &cfg, 0);
    if (res)
        return RTC_ERR_GENERIC;

    /* Real-time tuning */
    vpx_codec_control(&e->codec, VP8E_SET_CPUUSED, 8);
    vpx_codec_control(&e->codec, VP8E_SET_STATIC_THRESHOLD, 800);

    if (!vpx_img_alloc(&e->img, VPX_IMG_FMT_I420, (unsigned int)w, (unsigned int)h, 1))
        return RTC_ERR_NOMEM;

    e->out_buf_size = w * h; /* Generous upper bound */
    e->out_buf = (uint8_t *)malloc((size_t)e->out_buf_size);
    if (!e->out_buf)
        return RTC_ERR_NOMEM;

    return RTC_OK;
}

static int vp8_enc_encode(void *ctx, const video_frame_t *frame, video_packet_t *out) {
    vp8_encoder_ctx_t *e = (vp8_encoder_ctx_t *)ctx;

    /* Copy planes into vpx_image_t */
    for (int p = 0; p < 3; p++) {
        int h = (p == 0) ? e->height : e->height / 2;
        for (int y = 0; y < h; y++) {
            memcpy(e->img.planes[p] + y * e->img.stride[p], frame->planes[p] + y * frame->stride[p],
                   (p == 0) ? (size_t)e->width : (size_t)(e->width / 2));
        }
    }

    vpx_enc_frame_flags_t flags = 0;
    if (e->force_keyframe) {
        flags |= VPX_EFLAG_FORCE_KF;
        e->force_keyframe = false;
    }

    vpx_codec_err_t res = vpx_codec_encode(&e->codec, &e->img, e->pts++, 1, flags, VPX_DL_REALTIME);
    if (res)
        return RTC_ERR_GENERIC;

    /* Collect output */
    const vpx_codec_cx_pkt_t *pkt;
    vpx_codec_iter_t iter = NULL;
    out->data = NULL;
    out->len = 0;

    while ((pkt = vpx_codec_get_cx_data(&e->codec, &iter)) != NULL) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
            if ((int)pkt->data.frame.sz > e->out_buf_size) {
                e->out_buf_size = (int)pkt->data.frame.sz * 2;
                e->out_buf = (uint8_t *)realloc(e->out_buf, (size_t)e->out_buf_size);
            }
            memcpy(e->out_buf, pkt->data.frame.buf, pkt->data.frame.sz);
            out->data = e->out_buf;
            out->len = (int)pkt->data.frame.sz;
            out->is_keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            out->timestamp = (uint32_t)(e->pts * (90000 / e->fps));
            break;
        }
    }

    return (out->data) ? RTC_OK : RTC_ERR_GENERIC;
}

static int vp8_enc_set_bitrate(void *ctx, int bitrate_kbps) {
    vp8_encoder_ctx_t *e = (vp8_encoder_ctx_t *)ctx;
    vpx_codec_enc_cfg_t cfg;
    vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0);

    /* Re-read current config and update bitrate */
    cfg.g_w = (unsigned int)e->width;
    cfg.g_h = (unsigned int)e->height;
    cfg.rc_target_bitrate = (unsigned int)bitrate_kbps;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = e->fps;
    cfg.rc_end_usage = VPX_CBR;
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    cfg.g_lag_in_frames = 0;

    vpx_codec_enc_config_set(&e->codec, &cfg);
    return RTC_OK;
}

static int vp8_enc_request_keyframe(void *ctx) {
    vp8_encoder_ctx_t *e = (vp8_encoder_ctx_t *)ctx;
    e->force_keyframe = true;
    return RTC_OK;
}

static void vp8_enc_close(void *ctx) {
    vp8_encoder_ctx_t *e = (vp8_encoder_ctx_t *)ctx;
    vpx_codec_destroy(&e->codec);
    vpx_img_free(&e->img);
    free(e->out_buf);
}

static const video_encoder_ops_t vp8_encoder_ops = {
    .init = vp8_enc_init,
    .encode = vp8_enc_encode,
    .set_bitrate = vp8_enc_set_bitrate,
    .request_keyframe = vp8_enc_request_keyframe,
    .close = vp8_enc_close,
};

typedef struct {
    vpx_codec_ctx_t codec;
} vp8_decoder_ctx_t;

static int vp8_dec_init(void *ctx) {
    vp8_decoder_ctx_t *d = (vp8_decoder_ctx_t *)ctx;
    vpx_codec_err_t res = vpx_codec_dec_init(&d->codec, vpx_codec_vp8_dx(), NULL, 0);
    return res ? RTC_ERR_GENERIC : RTC_OK;
}

static int vp8_dec_decode(void *ctx, const uint8_t *data, int len, video_frame_t *out) {
    vp8_decoder_ctx_t *d = (vp8_decoder_ctx_t *)ctx;

    vpx_codec_err_t res = vpx_codec_decode(&d->codec, data, (unsigned int)len, NULL, 0);
    if (res)
        return RTC_ERR_GENERIC;

    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img = vpx_codec_get_frame(&d->codec, &iter);
    if (!img)
        return RTC_ERR_GENERIC;

    /* Point output planes at decoder's internal buffers (zero copy) */
    out->planes[0] = img->planes[0];
    out->planes[1] = img->planes[1];
    out->planes[2] = img->planes[2];
    out->stride[0] = img->stride[0];
    out->stride[1] = img->stride[1];
    out->stride[2] = img->stride[2];
    out->width = (int)img->d_w;
    out->height = (int)img->d_h;

    return RTC_OK;
}

static void vp8_dec_close(void *ctx) {
    vp8_decoder_ctx_t *d = (vp8_decoder_ctx_t *)ctx;
    vpx_codec_destroy(&d->codec);
}

static const video_decoder_ops_t vp8_decoder_ops = {
    .init = vp8_dec_init,
    .decode = vp8_dec_decode,
    .close = vp8_dec_close,
};

/* Registration entry points, called from the video_codec.c factory. */
const video_encoder_ops_t *vp8_get_encoder_ops(void) {
    return &vp8_encoder_ops;
}
const video_decoder_ops_t *vp8_get_decoder_ops(void) {
    return &vp8_decoder_ops;
}
size_t vp8_encoder_ctx_size(void) {
    return sizeof(vp8_encoder_ctx_t);
}
size_t vp8_decoder_ctx_size(void) {
    return sizeof(vp8_decoder_ctx_t);
}
