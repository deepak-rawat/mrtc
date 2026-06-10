/*
 * Pluggable video codec interface via vtable.
 *
 * Supports VP8 (libvpx), with H.264 (OpenH264) and FFmpeg backends
 * addable by implementing the ops vtable.
 */
#ifndef MEDIA_VIDEO_CODEC_H
#define MEDIA_VIDEO_CODEC_H

#include <rtc/rtc_common.h>
#include <stdint.h>
#include <stdbool.h>

/* Raw video frame (YUV420 / I420) */
typedef struct {
    uint8_t *planes[3]; /* Y, U, V plane pointers */
    int stride[3];      /* Row stride per plane */
    int width;
    int height;
} video_frame_t;

/* Compressed video packet */
typedef struct {
    uint8_t *data;
    int len;
    bool is_keyframe;
    uint32_t timestamp; /* 90kHz RTP clock */
} video_packet_t;

/* Encoder vtable */
typedef struct video_encoder_ops {
    int (*init)(void *ctx, int width, int height, int bitrate_kbps, int fps);
    int (*encode)(void *ctx, const video_frame_t *frame, video_packet_t *out);
    int (*set_bitrate)(void *ctx, int bitrate_kbps);
    int (*request_keyframe)(void *ctx);
    void (*close)(void *ctx);
} video_encoder_ops_t;

/* Decoder vtable */
typedef struct video_decoder_ops {
    int (*init)(void *ctx);
    int (*decode)(void *ctx, const uint8_t *data, int len, video_frame_t *out);
    void (*close)(void *ctx);
} video_decoder_ops_t;

/* Encoder instance */
typedef struct {
    void *ctx;
    const video_encoder_ops_t *ops;
} video_encoder_t;

/* Decoder instance */
typedef struct {
    void *ctx;
    const video_decoder_ops_t *ops;
} video_decoder_t;

/* Factory: create encoder/decoder by codec name ("VP8", "H264") */
int video_encoder_create(video_encoder_t *enc, const char *codec_name, int width, int height,
                         int bitrate_kbps, int fps);
int video_decoder_create(video_decoder_t *dec, const char *codec_name);
void video_encoder_destroy(video_encoder_t *enc);
void video_decoder_destroy(video_decoder_t *dec);

/* Convenience wrappers */
static inline int video_encode(video_encoder_t *enc, const video_frame_t *frame,
                               video_packet_t *out) {
    return enc->ops->encode(enc->ctx, frame, out);
}
static inline int video_decode(video_decoder_t *dec, const uint8_t *data, int len,
                               video_frame_t *out) {
    return dec->ops->decode(dec->ctx, data, len, out);
}
static inline int video_encoder_set_bitrate(video_encoder_t *enc, int kbps) {
    return enc->ops->set_bitrate(enc->ctx, kbps);
}
static inline int video_encoder_request_keyframe(video_encoder_t *enc) {
    return enc->ops->request_keyframe(enc->ctx);
}

/* Helper: allocate a video_frame_t with I420 layout */
int video_frame_alloc(video_frame_t *f, int width, int height);
void video_frame_free(video_frame_t *f);

#endif /* MEDIA_VIDEO_CODEC_H */
