#include "video_payload.h"
#include "vp8_packetizer.h"

#include <stdlib.h>
#include <string.h>

static void *vp8_create_depacketizer(void) {
    rtc_vp8_depacketizer_t *state = calloc(1, sizeof(*state));
    if (!state) {
        RTC_LOG_ERR("VP8: failed to allocate depacketizer");
        return NULL;
    }
    if (rtc_vp8_depacketizer_init(state) != RTC_OK) {
        RTC_LOG_ERR("VP8: failed to initialize depacketizer");
        free(state);
        return NULL;
    }
    return state;
}

static int vp8_depacketize(void *state, const uint8_t *data, size_t len, uint16_t seq,
                           uint32_t timestamp, bool marker, video_payload_frame_t *frame) {
    rtc_vp8_depacketizer_t *depacketizer = state;
    memset(frame, 0, sizeof(*frame));
    uint32_t dropped = depacketizer->dropped_frames;
    int rc = rtc_vp8_depacketize(depacketizer, data, len, seq, timestamp, marker, &frame->data,
                                 &frame->len, &frame->is_keyframe);
    frame->dropped_frames = depacketizer->dropped_frames - dropped;
    return rc;
}

const video_payload_format_t *video_payload_format_find(const char *codec) {
    static const video_payload_format_t vp8 = {
        .clock_rate = 90000,
        .max_frame_size = VP8_MAX_FRAME_SIZE,
        .ivf_fourcc = "VP80",
        .packetize = rtc_vp8_packetize,
        .create_depacketizer = vp8_create_depacketizer,
        .destroy_depacketizer = free,
        .depacketize = vp8_depacketize,
    };
    if (codec && strcmp(codec, "VP8") == 0)
        return &vp8;
    RTC_LOG_ERR("Unsupported video payload format: %s", codec ? codec : "(null)");
    return NULL;
}
