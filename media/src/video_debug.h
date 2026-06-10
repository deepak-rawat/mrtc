/*
 * Opt-in frame integrity check.
 *
 * FNV-1a checksum over Y plane. Useful for detecting stale/duplicate
 * decoder output (the "traces" artifact).
 */
#ifndef MEDIA_VIDEO_DEBUG_H
#define MEDIA_VIDEO_DEBUG_H

#include "media/video_codec.h"
#include <stdint.h>

/* Compute 32-bit FNV-1a hash of the Y plane.
   Returns 0 if frame is NULL. Deterministic. */
uint32_t video_frame_checksum(const video_frame_t *frame);

#endif /* MEDIA_VIDEO_DEBUG_H */
