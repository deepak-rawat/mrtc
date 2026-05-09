/*
 * video_dump.h — IVF bitstream dump for VP8/VP9.
 *
 * Controlled by media_debug_config_t.send_dump_path / recv_dump_path.
 * Both NULL by default → zero overhead. Set path to enable.
 */
#ifndef MEDIA_VIDEO_DUMP_H
#define MEDIA_VIDEO_DUMP_H

#include <rtc/rtc_common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct video_dump video_dump_t;

/* Open IVF file for writing. codec_fourcc: "VP80".
   Returns NULL on failure (logged). */
video_dump_t *video_dump_open(const char *path, const char *codec_fourcc, int width, int height,
                              int fps);

/* Write one compressed frame. timestamp: 90kHz RTP ticks.
   Returns RTC_OK or error. */
int video_dump_write_frame(video_dump_t *d, const uint8_t *data, size_t len, uint64_t timestamp);

/* Close file: patch frame count in header, fclose, free.
   Safe to call with NULL. */
void video_dump_close(video_dump_t *d);

#endif /* MEDIA_VIDEO_DUMP_H */
