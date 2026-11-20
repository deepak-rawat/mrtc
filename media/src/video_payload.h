#ifndef MEDIA_VIDEO_PAYLOAD_H
#define MEDIA_VIDEO_PAYLOAD_H

#include <rtc/rtc_common.h>

#define VIDEO_PAYLOAD_MAX_SIZE          1400
#define VIDEO_PAYLOAD_MAX_FRAME_PACKETS 256

typedef struct {
    uint8_t data[VIDEO_PAYLOAD_MAX_SIZE];
    size_t len;
    bool marker;
} video_payload_t;

typedef struct {
    const uint8_t *data; /* Borrowed until the next depacketizer call. */
    size_t len;
    bool is_keyframe;
    uint32_t dropped_frames; /* Frames lost during this call, including incomplete frames. */
} video_payload_frame_t;

typedef struct {
    uint32_t clock_rate;
    size_t max_frame_size;
    const char *ivf_fourcc; /* NULL when IVF output is unsupported. */
    int (*packetize)(const uint8_t *frame, size_t len, bool is_keyframe, video_payload_t *payloads,
                     int capacity, int *count);
    void *(*create_depacketizer)(void);
    void (*destroy_depacketizer)(void *state);
    /* RTC_ERR_GENERIC means that no complete frame is available yet. */
    int (*depacketize)(void *state, const uint8_t *data, size_t len, uint16_t seq,
                       uint32_t timestamp, bool marker, video_payload_frame_t *frame);
} video_payload_format_t;

const video_payload_format_t *video_payload_format_find(const char *codec);

#endif
