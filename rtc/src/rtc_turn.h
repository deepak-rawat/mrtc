/*
 * TURN client (RFC 5766) for relay candidate gathering.
 *
 * Manages a TURN allocation on a server: allocate, create permissions,
 * bind channels, and send/receive data via ChannelData framing.
 */
#ifndef RTC_TURN_H
#define RTC_TURN_H

#include "rtc_common.h"
#include "rtc_stun.h"

/* Forward declarations */
typedef struct rtc_packet_io rtc_packet_io_t;

#define TURN_MAX_CHANNELS    8
#define TURN_MAX_PERMISSIONS 16

typedef struct {
    const char *server_host;
    uint16_t server_port; /* default 3478 */
    const char *username;
    const char *credential;
} rtc_turn_config_t;

typedef struct {
    uint16_t channel;
    rtc_addr_t peer_addr;
    bool active;
} rtc_turn_channel_t;

typedef struct {
    rtc_addr_t addr;
    bool active;
} rtc_turn_permission_t;

typedef struct rtc_turn_client {
    rtc_turn_config_t cfg;
    rtc_packet_io_t *transport;
    rtc_addr_t server_addr;

    /* Allocation state */
    bool allocated;
    rtc_addr_t relay_addr; /* server-assigned relay address */
    uint32_t lifetime;     /* seconds */

    /* Long-term credential state (from 401 challenge) */
    char realm[128];
    char nonce[256];
    uint8_t lt_key[16]; /* MD5(user:realm:pass) */
    bool has_credentials;

    /* Channels and permissions */
    rtc_turn_channel_t channels[TURN_MAX_CHANNELS];
    int channel_count;
    rtc_turn_permission_t permissions[TURN_MAX_PERMISSIONS];
    int permission_count;
} rtc_turn_client_t;

/* Initialize TURN client (does not allocate yet) */
rtc_err_t rtc_turn_init(rtc_turn_client_t *tc, rtc_packet_io_t *transport,
                        const rtc_turn_config_t *cfg);

/* Send Allocate request to server. Returns relay address in tc->relay_addr. */
rtc_err_t rtc_turn_allocate(rtc_turn_client_t *tc);

/* Create a permission for a peer address */
rtc_err_t rtc_turn_create_permission(rtc_turn_client_t *tc, const rtc_addr_t *peer);

/* Bind a channel number to a peer address */
rtc_err_t rtc_turn_channel_bind(rtc_turn_client_t *tc, const rtc_addr_t *peer, uint16_t channel);

/* Send data via ChannelData to a bound channel */
rtc_err_t rtc_turn_send(rtc_turn_client_t *tc, uint16_t channel, const uint8_t *data, size_t len);

/* Send Refresh(lifetime=0) to deallocate, then cleanup */
void rtc_turn_close(rtc_turn_client_t *tc);

#endif /* RTC_TURN_H */
