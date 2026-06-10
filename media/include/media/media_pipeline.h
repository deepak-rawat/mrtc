/*
 * Multi-stream media pipeline.
 *
 * Supports multiple send streams (camera, screen share) and multiple
 * recv streams (one per remote peer per track), each with its own
 * encoder/decoder, packetizer, and jitter buffer.
 *
 * Streams are identified by label ("camera", "screen") on the send side
 * and by SSRC on the recv side. The renderer callback includes both
 * peer_id and label so the UI knows which tile to update.
 */
#ifndef MEDIA_PIPELINE_H
#define MEDIA_PIPELINE_H

#include "video_codec.h"
#include "audio_codec.h"
#include "video_stats.h"
#include <rtc/rtc_common.h>
#include <rtc/rtc_track.h>

#define MP_MAX_SEND_STREAMS 4
#define MP_MAX_RECV_STREAMS 16
#define MP_LABEL_SIZE       64

/* Renderer interface (application provides). */
typedef struct {
    void (*on_video_frame)(const char *peer_id, const char *label, const video_frame_t *frame,
                           void *user);
    void (*on_audio_samples)(const char *peer_id, const char *label, const audio_frame_t *audio,
                             void *user);
    void *user;
} media_renderer_t;

#define MP_MAX_SEND_PEERS 8

/* Pipeline config (initial defaults). */
typedef struct {
    const char *default_video_codec; /* "VP8" */
    const char *default_audio_codec; /* "opus" */
    const media_renderer_t *renderer;
} media_pipeline_config_t;

typedef struct media_pipeline media_pipeline_t;

/* Opaque handle to a send stream (returned by add_send_stream) */
typedef struct media_send_stream media_send_stream_t;

/* Opaque handle to a recv stream (returned by add_recv_stream) */
typedef struct media_recv_stream media_recv_stream_t;

/* Create/destroy */
media_pipeline_t *media_pipeline_create(const media_pipeline_config_t *cfg);
void media_pipeline_destroy(media_pipeline_t *p);

/* Send peers — fan-out: encode once → send to N peers. */

/*
 * Register a send peer. When push_video/push_audio is called, the pipeline
 * encodes once and calls rtc_rtp_sender_send() on each registered peer's
 * video_sender or audio_sender directly.
 */
int media_pipeline_add_send_peer(media_pipeline_t *p, const char *peer_id,
                                 rtc_rtp_sender_t *video_sender, rtc_rtp_sender_t *audio_sender);
void media_pipeline_remove_send_peer(media_pipeline_t *p, const char *peer_id);

/* Send streams (local tracks). */

/*
 * Add a local send stream. Returns an opaque handle, or NULL on error.
 * label: "camera", "screen", "mic", etc.
 * codec: "VP8", "opus"
 * For video: width, height, fps, bitrate_kbps are used.
 * For audio: width/height/fps ignored, bitrate = bitrate_bps.
 */
media_send_stream_t *media_pipeline_add_send_stream(media_pipeline_t *p, const char *label,
                                                    bool is_video, const char *codec, int width,
                                                    int height, int fps, int bitrate);

/* Get the SSRC assigned to a send stream */
uint32_t media_send_stream_get_ssrc(const media_send_stream_t *s);

/* Push a video frame to a send stream (encode → fragment → fan-out) */
int media_pipeline_push_video(media_pipeline_t *p, media_send_stream_t *stream,
                              const video_frame_t *frame);

/* Push audio to a send stream (encode → fan-out) */
int media_pipeline_push_audio(media_pipeline_t *p, media_send_stream_t *stream,
                              const audio_frame_t *audio);

/* Recv streams (remote tracks). */

/*
 * Register a remote recv stream. Returns an opaque handle, or NULL on error.
 * Duplicate SSRC returns the existing handle.
 * peer_id: remote peer identifier
 * label: "camera", "screen", "mic"
 * ssrc: RTP SSRC for this stream
 * codec: "VP8", "opus"
 */
media_recv_stream_t *media_pipeline_add_recv_stream(media_pipeline_t *p, const char *peer_id,
                                                    const char *label, uint32_t ssrc, bool is_video,
                                                    const char *codec);

/* Remove a recv stream (peer left or track removed) */
void media_pipeline_remove_recv_stream(media_pipeline_t *p, uint32_t ssrc);

/* Remove all recv streams for a peer */
void media_pipeline_remove_peer(media_pipeline_t *p, const char *peer_id);

/*
 * Feed an incoming RTP payload to a recv stream (direct, no SSRC lookup).
 * For video: depacketize → decode → render
 * For audio: jitter buffer → decode → render
 */
int media_pipeline_recv_rtp(media_pipeline_t *p, media_recv_stream_t *stream, const uint8_t *data,
                            int len, uint16_t seq, uint32_t timestamp, bool marker);

/* Debug configuration (all features off by default). */

typedef struct {
    const char *send_dump_path;            /* IVF dump path, NULL = disabled */
    const char *recv_dump_path;            /* IVF dump path, NULL = disabled */
    int dump_width, dump_height, dump_fps; /* IVF header params */
    bool enable_frame_checksum;            /* log Y-plane checksum at DEBUG level */
    int psnr_sample_interval;              /* 0=off, N=every Nth encoded frame */
} media_debug_config_t;

/* Configure all debug features at once. Pass NULL to disable everything. */
int media_pipeline_set_debug(media_pipeline_t *p, const media_debug_config_t *cfg);

/* Stats access — app polls these, computes rates by diffing. */

const video_send_stats_t *media_pipeline_get_send_stats(media_pipeline_t *p, int index);
const video_recv_stats_t *media_pipeline_get_recv_stats(media_pipeline_t *p, int index);

#endif /* MEDIA_PIPELINE_H */
