/*
 * Data Channel API - WebRTC-style RTCDataChannel.
 *
 * Mirrors the W3C RTCDataChannel interface. A data channel is created
 * from a peer connection (see rtc_peer_connection_create_data_channel)
 * and delivers reliable, ordered messages over the DTLS transport.
 *
 * The wire format and the manager that owns channels are internal to
 * the library (see client/src/rtc_data_channel_internal.h).
 */
#ifndef RTC_DATA_CHANNEL_H
#define RTC_DATA_CHANNEL_H

#include "rtc_common.h"

#define RTC_DC_MAX_LABEL    128
#define RTC_DC_MAX_MSG_SIZE 65535

/* Opaque data channel handle */
typedef struct rtc_data_channel rtc_data_channel_t;

/* Data channel initialization options (mirrors RTCDataChannelInit) */
typedef struct {
    bool ordered;         /* Default: true */
    int max_retransmits;  /* -1 = unlimited (default) */
    int max_packet_life;  /* -1 = unlimited (default) */
    const char *protocol; /* Default: "" */
    bool negotiated;      /* Default: false */
    int id;               /* -1 = auto (default) */
} rtc_data_channel_init_t;

/* Data channel state (mirrors RTCDataChannelState) */
typedef enum {
    RTC_DC_CONNECTING,
    RTC_DC_OPEN,
    RTC_DC_CLOSING,
    RTC_DC_CLOSED,
} rtc_data_channel_state_t;

/* Callbacks */
typedef void (*rtc_on_dc_open_fn)(void *user);
typedef void (*rtc_on_dc_close_fn)(void *user);
typedef void (*rtc_on_dc_message_fn)(const uint8_t *data, size_t len, void *user);
typedef void (*rtc_on_dc_buffered_amount_low_fn)(void *user);

/* Send binary data over the channel */
int rtc_data_channel_send(rtc_data_channel_t *dc, const uint8_t *data, size_t len);

/* Send text data over the channel (convenience wrapper) */
int rtc_data_channel_send_text(rtc_data_channel_t *dc, const char *text);

/* Close the data channel */
void rtc_data_channel_close(rtc_data_channel_t *dc);

/* Event callbacks */
void rtc_data_channel_on_open(rtc_data_channel_t *dc, rtc_on_dc_open_fn fn, void *user);
void rtc_data_channel_on_close(rtc_data_channel_t *dc, rtc_on_dc_close_fn fn, void *user);
void rtc_data_channel_on_message(rtc_data_channel_t *dc, rtc_on_dc_message_fn fn, void *user);

/* Property getters */
const char *rtc_data_channel_label(const rtc_data_channel_t *dc);
uint16_t rtc_data_channel_id(const rtc_data_channel_t *dc);
rtc_data_channel_state_t rtc_data_channel_state(const rtc_data_channel_t *dc);

/* Bytes queued in user-space awaiting transmission.
 * mrtc sends synchronously today (no internal queue), so this is always
 * 0. The API is exposed for spec parity and forward compatibility. */
uint64_t rtc_data_channel_buffered_amount(const rtc_data_channel_t *dc);

/* bufferedAmountLowThreshold (spec): when bufferedAmount drops below
 * this value the on_buffered_amount_low callback fires. With the
 * current synchronous send path bufferedAmount never rises above 0,
 * so the callback only fires if the threshold is configured > 0 and
 * a send completes. */
uint64_t rtc_data_channel_buffered_amount_low_threshold(const rtc_data_channel_t *dc);
void rtc_data_channel_set_buffered_amount_low_threshold(rtc_data_channel_t *dc, uint64_t threshold);
void rtc_data_channel_on_buffered_amount_low(rtc_data_channel_t *dc,
                                             rtc_on_dc_buffered_amount_low_fn fn, void *user);

/* Cumulative byte counters (mrtc extension, not in W3C spec). Useful
 * for observability when there is no real send queue to inspect. */
uint64_t rtc_data_channel_bytes_sent(const rtc_data_channel_t *dc);
uint64_t rtc_data_channel_bytes_received(const rtc_data_channel_t *dc);

#endif /* RTC_DATA_CHANNEL_H */
