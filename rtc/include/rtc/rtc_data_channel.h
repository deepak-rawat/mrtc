/*
 * Data Channel API - Lightweight data channel over DTLS.
 *
 * Implements a simplified data channel protocol using DTLS for transport.
 * Messages are framed with a simple header (type + length) over the
 * DTLS application data path.
 *
 * Wire format (each message):
 *   [1 byte: channel_id] [1 byte: msg_type] [2 bytes: length (BE)] [payload]
 *
 * Channel negotiation uses a simple open/ack handshake.
 */
#ifndef RTC_DATA_CHANNEL_H
#define RTC_DATA_CHANNEL_H

#include "rtc_common.h"

#define RTC_DC_MAX_CHANNELS  16
#define RTC_DC_MAX_LABEL     128
#define RTC_DC_MAX_MSG_SIZE  65535
#define RTC_DC_HEADER_SIZE   4

/* Internal message types for channel protocol */
#define RTC_DC_MSG_OPEN      0x01
#define RTC_DC_MSG_ACK       0x02
#define RTC_DC_MSG_DATA      0x03
#define RTC_DC_MSG_CLOSE     0x04

/* Opaque data channel handle */
typedef struct rtc_data_channel rtc_data_channel_t;

/* Data channel initialization options (mirrors RTCDataChannelInit) */
typedef struct {
    bool        ordered;           /* Default: true */
    int         max_retransmits;   /* -1 = unlimited (default) */
    int         max_packet_life;   /* -1 = unlimited (default) */
    const char *protocol;          /* Default: "" */
    bool        negotiated;        /* Default: false */
    int         id;                /* -1 = auto (default) */
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

/* ---- API ---- */

/* Send binary data over the channel */
int rtc_data_channel_send(rtc_data_channel_t *dc,
                          const uint8_t *data, size_t len);

/* Send text data over the channel (convenience wrapper) */
int rtc_data_channel_send_text(rtc_data_channel_t *dc,
                               const char *text);

/* Close the data channel */
void rtc_data_channel_close(rtc_data_channel_t *dc);

/* Event callbacks */
void rtc_data_channel_on_open(rtc_data_channel_t *dc,
                              rtc_on_dc_open_fn fn, void *user);
void rtc_data_channel_on_close(rtc_data_channel_t *dc,
                               rtc_on_dc_close_fn fn, void *user);
void rtc_data_channel_on_message(rtc_data_channel_t *dc,
                                 rtc_on_dc_message_fn fn, void *user);

/* Property getters */
const char *rtc_data_channel_label(const rtc_data_channel_t *dc);
uint16_t rtc_data_channel_id(const rtc_data_channel_t *dc);
rtc_data_channel_state_t rtc_data_channel_state(const rtc_data_channel_t *dc);

/* ---- Data channel manager (internal, used by peer connection) ---- */

/* Forward declaration */
typedef struct rtc_dc_manager rtc_dc_manager_t;

/* Callback for sending data over DTLS transport */
typedef int (*rtc_dc_send_fn)(const uint8_t *data, size_t len, void *user);

/* Callback when a remote peer opens a data channel */
typedef void (*rtc_dc_on_channel_fn)(rtc_data_channel_t *dc, void *user);

/* Initialize the data channel manager */
int rtc_dc_manager_init(rtc_dc_manager_t *mgr, rtc_dc_send_fn send_fn, void *send_user);

/* Create a local data channel (returns it in CONNECTING state) */
rtc_data_channel_t *rtc_dc_manager_create_channel(rtc_dc_manager_t *mgr,
                                                   const char *label,
                                                   const rtc_data_channel_init_t *opts);

/* Set callback for remotely-created channels */
void rtc_dc_manager_on_channel(rtc_dc_manager_t *mgr,
                               rtc_dc_on_channel_fn fn, void *user);

/* Process incoming DTLS application data (called when DTLS decrypts data) */
int rtc_dc_manager_recv(rtc_dc_manager_t *mgr, const uint8_t *data, size_t len);

/* Notify manager that DTLS is connected (triggers OPEN handshake for pending channels) */
int rtc_dc_manager_on_dtls_connected(rtc_dc_manager_t *mgr);

/* Close all channels and free resources */
void rtc_dc_manager_close(rtc_dc_manager_t *mgr);

/* Data channel manager struct (not opaque, embedded in peer connection) */
struct rtc_dc_manager {
    struct rtc_data_channel *channels[RTC_DC_MAX_CHANNELS];
    int                      channel_count;
    int                      next_id;          /* next auto-assigned channel ID */
    rtc_dc_send_fn           send_fn;
    void                    *send_user;
    rtc_dc_on_channel_fn     on_channel;
    void                    *on_channel_user;
    bool                     dtls_connected;
};

#endif /* RTC_DATA_CHANNEL_H */
