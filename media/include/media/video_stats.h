/*
 * Per-stream video statistics (W3C WebRTC Stats aligned).
 *
 * Pure cumulative counters. App polls via get_stats(), diffs two reads,
 * computes rates. No snapshot/log functions — app decides presentation.
 */
#ifndef MEDIA_VIDEO_STATS_H
#define MEDIA_VIDEO_STATS_H

#include <rtc/rtc_common.h>
#include "video_codec.h"
#include <stdbool.h>
#include <stdint.h>

/* Send-side stats (one per send stream).
   Mirrors RTCOutboundRtpStreamStats + RTCSentRtpStreamStats. */
typedef struct {
    uint64_t frames_encoded;    /* successful encodes */
    uint64_t frames_dropped;    /* encode returned error */
    uint64_t keyframes_encoded; /* subset of frames_encoded */
    uint64_t bytes_encoded;     /* VP8 bitstream bytes (pre-packetization) */
    uint64_t packets_sent;      /* RTP packets after packetization */
    uint64_t bytes_sent;        /* RTP payload bytes */

    /* PSNR (W3C: psnrSum / psnrMeasurements).
       Only populated when psnr_sample_interval > 0 in debug config. */
    double psnr_sum;
    uint64_t psnr_measurements;
} video_send_stats_t;

/* Recv-side stats (one per recv stream).
   Mirrors RTCInboundRtpStreamStats + RTCReceivedRtpStreamStats. */
typedef struct {
    uint64_t packets_received;
    uint64_t bytes_received;   /* RTP payload bytes */
    uint64_t frames_assembled; /* depacketizer produced complete frame */
    uint64_t frames_decoded;
    uint64_t frames_decode_failed;
    uint64_t keyframes_decoded;
    uint64_t bytes_decoded;   /* VP8 bitstream bytes fed to decoder */
    uint64_t frames_rendered; /* passed to renderer callback */
    uint64_t frames_dropped;  /* dropped before or during decode */

    /* RTP sequence gap tracking (always on, zero cost) */
    uint64_t rtp_seq_gaps;      /* gap events */
    uint64_t packets_missing;   /* sum of gap sizes */
    uint64_t packets_reordered; /* arrived out of order */
    uint64_t packets_duplicate;
    uint16_t rtp_last_seq;
    bool rtp_seq_initialized;

    /* Depacketizer health */
    uint64_t depack_frames_dropped; /* mid-frame reset */
} video_recv_stats_t;

/* Zero all fields. */
void video_send_stats_init(video_send_stats_t *s);
void video_recv_stats_init(video_recv_stats_t *s);

/* Y-plane PSNR. Returns dB (>30 good), INFINITY if identical, 0.0 on error. */
double video_frame_psnr(const video_frame_t *ref, const video_frame_t *test);

#endif /* MEDIA_VIDEO_STATS_H */
