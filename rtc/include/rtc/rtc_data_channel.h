/*
 * Data Channel API - WebRTC-style RTCDataChannel.
 *
 * Mirrors the W3C RTCDataChannel interface. A data channel is created
 * from a peer connection (see rtc_peer_connection_create_data_channel)
 * and delivers SCTP messages over DTLS (RFC 8831), with DCEP (RFC 8832).
 *
 * The wire format and the manager that owns channels are internal to
 * the library (see rtc/src/rtc_data_channel_internal.h).
 */
#ifndef RTC_DATA_CHANNEL_H
#define RTC_DATA_CHANNEL_H

#include "rtc_common.h"

#define RTC_DC_MAX_LABEL    128
#define RTC_DC_MAX_MSG_SIZE 65535

/* Opaque data channel handle */
typedef struct rtc_data_channel rtc_data_channel_t;

/* NULL options select reliable ordered delivery, automatic stream id.
 * With explicit options initialize both reliability fields and id to -1
 * unless used. Reliability limits are mutually exclusive (0..65535);
 * negotiated channels require an explicit id on both endpoints. */
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

/* Automatic stream ids are provisional until the DTLS role is negotiated. */
const char *rtc_data_channel_label(const rtc_data_channel_t *dc);
uint16_t rtc_data_channel_id(const rtc_data_channel_t *dc);
rtc_data_channel_state_t rtc_data_channel_state(const rtc_data_channel_t *dc);

/* Bytes queued in the bounded user-space queue while SCTP is backpressured.
 * Bytes already accepted into SCTP's retransmission buffer are excluded. */
uint64_t rtc_data_channel_buffered_amount(const rtc_data_channel_t *dc);

/* Fires only on an actual downward crossing of the configured threshold. */
uint64_t rtc_data_channel_buffered_amount_low_threshold(const rtc_data_channel_t *dc);
void rtc_data_channel_set_buffered_amount_low_threshold(rtc_data_channel_t *dc, uint64_t threshold);
void rtc_data_channel_on_buffered_amount_low(rtc_data_channel_t *dc,
                                             rtc_on_dc_buffered_amount_low_fn fn, void *user);

/* Cumulative accepted/delivered application bytes (mrtc extension). */
uint64_t rtc_data_channel_bytes_sent(const rtc_data_channel_t *dc);
uint64_t rtc_data_channel_bytes_received(const rtc_data_channel_t *dc);

#endif /* RTC_DATA_CHANNEL_H */
