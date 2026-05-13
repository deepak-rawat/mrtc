/*
 * turn_handler.h - TURN message handler and allocation management.
 */
#ifndef TURN_HANDLER_H
#define TURN_HANDLER_H

#include <rtc/rtc_common.h>
#include <rtc/rtc_stun.h>
#include <rtc/rtc_vec.h>

#define TURN_MAX_ALLOCATIONS  64
#define TURN_DEFAULT_LIFETIME 600 /* 10 minutes */

/* Nonce lifetime (RFC 8489 §9.2 recommends rotation). Clients that send
 * an expired nonce receive 438 STALE_NONCE with a fresh REALM/NONCE pair. */
#define TURN_NONCE_LIFETIME_MS (3600u * 1000u) /* 1 hour */

typedef struct {
    uint16_t channel;
    rtc_addr_t peer_addr;
    bool active;
} turn_channel_t;

typedef struct {
    rtc_addr_t addr;
    bool active;
} turn_permission_t;

typedef struct {
    bool active;
    rtc_addr_t client_addr;  /* who allocated */
    rtc_socket_t relay_sock; /* relay UDP socket */
    rtc_addr_t relay_addr;   /* relay address */
    uint32_t lifetime;
    uint64_t expires_ms; /* rtc_time_ms() deadline */

    rtc_vec_t channels;    /* turn_channel_t */
    rtc_vec_t permissions; /* turn_permission_t */
} turn_allocation_t;

typedef struct {
    const char *username;
    const char *password;
    const char *realm;
    const char *public_ip;
    uint8_t lt_key[16]; /* MD5(username:realm:password) */
    char nonce[64];
    uint64_t nonce_generated_ms;

    rtc_socket_t listen_sock;
    turn_allocation_t allocs[TURN_MAX_ALLOCATIONS];
    int alloc_count;
} turn_server_t;

/* Initialize the TURN server state */
int turn_server_init(turn_server_t *ts, const char *public_ip, uint16_t port, const char *username,
                     const char *password, const char *realm);

/* Process one incoming UDP packet */
void turn_server_handle_packet(turn_server_t *ts, const uint8_t *data, size_t len,
                               const rtc_addr_t *from);

/* Relay data from a relay socket back to the client via ChannelData */
void turn_server_relay_from_peer(turn_server_t *ts, turn_allocation_t *alloc, const uint8_t *data,
                                 size_t len, const rtc_addr_t *peer_from);

/* Expire old allocations */
void turn_server_expire(turn_server_t *ts);

/* Cleanup */
void turn_server_close(turn_server_t *ts);

#endif /* TURN_HANDLER_H */
