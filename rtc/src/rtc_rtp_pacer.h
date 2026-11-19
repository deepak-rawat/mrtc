#ifndef RTC_RTP_PACER_H
#define RTC_RTP_PACER_H

#include "rtc_common.h"

#define RTC_PACER_MAX_BYTES       (256 * 1024)
#define RTC_PACER_MAX_FRAME_BYTES (128 * 1024)
#define RTC_PACER_MAX_AGE_MS      200
#define RTC_PACER_STAGE_AGE_MS    200
#define RTC_PACER_BURST_BYTES     2400
#define RTC_PACER_MAX_SOURCES     32

typedef struct rtc_pacer_frame rtc_pacer_frame_t;
typedef struct rtc_pacer_source rtc_pacer_source_t;
typedef struct {
    uint64_t packets_sent, octets_sent, packets_dropped, packets_rejected;
    uint64_t packets_queued;
    uint32_t timestamp;
    uint64_t send_time_ms;
} rtc_pacer_sent_t;

typedef struct {
    rtc_pacer_frame_t *head, *tail;
    rtc_pacer_source_t *sources;
    size_t bytes, packets;
    unsigned source_count;
    uint32_t bitrate_bps;
    uint64_t last_ms, credit;
    uint64_t packets_sent, bytes_sent, packets_dropped, frames_dropped;
    bool clock_valid;
} rtc_rtp_pacer_t;

typedef int (*rtc_pacer_write_fn)(const uint8_t *wire, size_t len, void *user);

int rtc_rtp_pacer_enqueue(rtc_rtp_pacer_t *p, const uint8_t *wire, size_t len, size_t payload_len,
                          uint64_t now);
void rtc_rtp_pacer_run(rtc_rtp_pacer_t *p, uint64_t now, rtc_pacer_write_fn write, void *user);
bool rtc_rtp_pacer_pending(const rtc_rtp_pacer_t *p);
void rtc_rtp_pacer_discard(rtc_rtp_pacer_t *p, uint32_t ssrc);
void rtc_rtp_pacer_forget(rtc_rtp_pacer_t *p, uint32_t ssrc);
void rtc_rtp_pacer_close(rtc_rtp_pacer_t *p);
void rtc_rtp_pacer_clear(rtc_rtp_pacer_t *p);
bool rtc_rtp_pacer_sent(const rtc_rtp_pacer_t *p, uint32_t ssrc, rtc_pacer_sent_t *out);
bool rtc_rtp_pacer_repair(rtc_rtp_pacer_t *p, const uint8_t *wire, size_t len, uint64_t now);

#endif
