/*
 * Public RTP capabilities and parameters shared by SFU signaling APIs.
 */
#ifndef RTC_RTP_PARAMS_H
#define RTC_RTP_PARAMS_H

#if !defined(MRTC_ENABLE_SFU_API) && !defined(MRTC_ENABLE_RUNTIME_TRANSPORT)
#  error "rtc_rtp_params.h requires MRTC_ENABLE_SFU_API or MRTC_ENABLE_RUNTIME_TRANSPORT"
#endif

#include "rtc_common.h"

typedef enum {
    RTC_MEDIA_KIND_AUDIO,
    RTC_MEDIA_KIND_VIDEO,
} rtc_media_kind_t;

typedef struct {
    rtc_media_kind_t kind;
    uint8_t payload_type;
    char mime_type[64];
    uint32_t clock_rate;
    int channels;
} rtc_rtp_codec_parameters_t;

#define RTC_RTP_MAX_CODECS 4

typedef struct {
    rtc_rtp_codec_parameters_t codecs[RTC_RTP_MAX_CODECS];
    int codec_count;
    uint32_t ssrc;
    char mid[32];
} rtc_rtp_parameters_t;

#endif /* RTC_RTP_PARAMS_H */