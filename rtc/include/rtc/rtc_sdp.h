/*
 * SDP (RFC 4566 / WebRTC subset) - SDP generation and parsing for WebRTC.
 *
 * Supports multiple media lines (audio, video, data channel):
 *  - ICE credentials (ufrag, pwd)
 *  - ICE candidates
 *  - DTLS fingerprint and setup role
 *  - Audio (e.g. Opus), Video (e.g. VP8, H264), Application (data channel)
 */
#ifndef RTC_SDP_H
#define RTC_SDP_H

#include "rtc_common.h"

/* ICE candidate types (shared with ICE agent internals) */
#define ICE_MAX_CANDIDATES 16
#define ICE_UFRAG_LEN      8
#define ICE_PWD_LEN        24

typedef enum {
    ICE_CANDIDATE_HOST,
    ICE_CANDIDATE_SRFLX,
    ICE_CANDIDATE_RELAY,
} rtc_ice_candidate_type_t;

typedef struct {
    rtc_ice_candidate_type_t type;
    rtc_addr_t addr;
    uint32_t priority;
    int component;
    char foundation[8];
} rtc_ice_candidate_t;

#define SDP_MAX_SIZE       8192
#define SDP_MAX_CANDIDATES 16
#define SDP_MAX_MEDIA      4

typedef enum {
    RTC_SDP_OFFER,
    RTC_SDP_ANSWER,
    RTC_SDP_PRANSWER,
} rtc_sdp_type_t;

typedef enum {
    RTC_MEDIA_AUDIO,
    RTC_MEDIA_VIDEO,
    RTC_MEDIA_APPLICATION, /* data channel */
} rtc_media_type_t;

typedef enum {
    RTC_SETUP_ACTIVE,  /* DTLS client */
    RTC_SETUP_PASSIVE, /* DTLS server */
    RTC_SETUP_ACTPASS, /* can be either */
} rtc_setup_role_t;

/* Per-media-line description */
typedef struct {
    rtc_media_type_t media_type;
    int payload_type;    /* e.g. 111 for Opus, 96 for VP8 */
    int clockrate;       /* e.g. 48000 for Opus, 90000 for video */
    int channels;        /* e.g. 2 for Opus. 0 for video. */
    char codec_name[32]; /* e.g. "opus", "VP8", "H264" */
    int mid_index;       /* m= line index (0, 1, 2, ...) */
} rtc_sdp_media_t;

typedef struct {
    rtc_sdp_type_t type;

    /* Legacy single-media fields (for backward compatibility) */
    rtc_media_type_t media_type;
    int payload_type;
    int clockrate;
    int channels;
    char codec_name[32];

    /* Multi-media support */
    rtc_sdp_media_t media[SDP_MAX_MEDIA];
    int media_count;

    /* ICE */
    char ice_ufrag[ICE_UFRAG_LEN];
    char ice_pwd[ICE_PWD_LEN];

    /* DTLS */
    char fingerprint[96];
    rtc_setup_role_t setup;

    /* Candidates */
    rtc_ice_candidate_t candidates[SDP_MAX_CANDIDATES];
    int candidate_count;

    /* Session-level */
    char session_id[32];

    /* Raw SDP text */
    char raw[SDP_MAX_SIZE];
    size_t raw_len;
} rtc_sdp_t;

/* Generate an SDP offer or answer */
int rtc_sdp_generate(rtc_sdp_t *sdp);

/* Parse SDP text into the structure */
int rtc_sdp_parse(rtc_sdp_t *sdp, const char *text, size_t len);

/* Pretty-print SDP to stdout */
void rtc_sdp_print(const rtc_sdp_t *sdp);

#endif /* RTC_SDP_H */
