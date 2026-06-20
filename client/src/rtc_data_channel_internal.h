/*
 * Internal data channel manager interface.
 * NOT part of the public API.
 *
 * The data channel manager is embedded inside rtc_peer_connection and
 * drives the OPEN/ACK/DATA/CLOSE handshake over the DTLS application-data
 * path. Only rtc_peer.c, rtc_data_channel.c, and the data channel unit
 * test should include this header.
 */
#ifndef RTC_DATA_CHANNEL_INTERNAL_H
#define RTC_DATA_CHANNEL_INTERNAL_H

#include "rtc/rtc_data_channel.h"
#include "rtc/rtc_u32_map.h"

#define RTC_DC_MAX_CHANNELS 16
#define RTC_DC_HEADER_SIZE  6

/* Wire-level message types (sent in the msg_type byte of the header). */
#define RTC_DC_MSG_OPEN  0x01
#define RTC_DC_MSG_ACK   0x02
#define RTC_DC_MSG_DATA  0x03
#define RTC_DC_MSG_CLOSE 0x04

typedef struct rtc_dc_manager rtc_dc_manager_t;

/* Callback for sending data over DTLS transport. */
typedef int (*rtc_dc_send_fn)(const uint8_t *data, size_t len, void *user);

/* Callback when a remote peer opens a data channel. */
typedef void (*rtc_dc_on_channel_fn)(rtc_data_channel_t *dc, void *user);

/* Initialize the data channel manager. */
int rtc_dc_manager_init(rtc_dc_manager_t *mgr, rtc_dc_send_fn send_fn, void *send_user);

/* Create a local data channel (returns it in CONNECTING state). */
rtc_data_channel_t *rtc_dc_manager_create_channel(rtc_dc_manager_t *mgr, const char *label,
                                                  const rtc_data_channel_init_t *opts);

/* Set callback for remotely-created channels. */
void rtc_dc_manager_on_channel(rtc_dc_manager_t *mgr, rtc_dc_on_channel_fn fn, void *user);

/* Process incoming DTLS application data (called when DTLS decrypts data). */
int rtc_dc_manager_recv(rtc_dc_manager_t *mgr, const uint8_t *data, size_t len);

/* Notify manager that DTLS is connected (triggers OPEN for pending channels). */
int rtc_dc_manager_on_dtls_connected(rtc_dc_manager_t *mgr);

/* Close all channels and free resources. */
void rtc_dc_manager_close(rtc_dc_manager_t *mgr);

/* Manager struct (not opaque, embedded in peer connection). */
struct rtc_dc_manager {
    rtc_u32_map_t channels; /* channel id (uint32) -> rtc_data_channel_t * */
    int next_id;            /* next auto-assigned channel ID */
    rtc_dc_send_fn send_fn;
    void *send_user;
    rtc_dc_on_channel_fn on_channel;
    void *on_channel_user;
    bool dtls_connected;
};

#endif /* RTC_DATA_CHANNEL_INTERNAL_H */
