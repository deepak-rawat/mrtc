/*
 * rtc_turn.c - TURN client implementation.
 *
 * Handles Allocate (with 401 challenge retry), CreatePermission,
 * ChannelBind, and ChannelData send. Uses blocking sendto/recvfrom
 * on the transport socket for simplicity during gathering.
 */
#include "rtc/rtc_turn.h"
#include "rtc_transport.h"
#include <string.h>
#include <stdio.h>

/* Send a STUN message to the TURN server and wait for response */
static int turn_transaction(rtc_turn_client_t *tc, rtc_stun_msg_t *req, rtc_stun_msg_t *resp) {
    ssize_t sent = sendto(tc->transport->sock, (const char *)req->buf, (int)req->buf_len, 0,
                          (struct sockaddr *)&tc->server_addr.addr, tc->server_addr.len);
    if (sent < 0)
        return RTC_ERR_SOCKET;

    /* Wait for response */
    fd_set fds;
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    FD_ZERO(&fds);
    FD_SET(tc->transport->sock, &fds);

    int sel = select((int)tc->transport->sock + 1, &fds, NULL, NULL, &tv);
    if (sel <= 0)
        return RTC_ERR_TIMEOUT;

    uint8_t buf[STUN_MAX_MSG_SIZE];
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(tc->transport->sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from,
                         &fromlen);
    if (n < STUN_HEADER_SIZE)
        return RTC_ERR_SOCKET;

    int rc = rtc_stun_parse(resp, buf, (size_t)n);
    if (rc != RTC_OK)
        return rc;

    /* Verify transaction ID */
    if (memcmp(resp->txn_id, req->txn_id, STUN_TXN_ID_SIZE) != 0)
        return RTC_ERR_INVALID;

    return RTC_OK;
}

/* Build and finalize a TURN request with credentials */
static int build_authenticated_request(rtc_turn_client_t *tc, rtc_stun_msg_t *msg,
                                       uint16_t method) {
    int rc = rtc_stun_build_request(msg, method, tc->cfg.username, tc->cfg.credential);
    if (rc != RTC_OK)
        return rc;

    if (tc->has_credentials) {
        rtc_stun_add_username(msg, tc->cfg.username);
        rtc_stun_add_realm(msg, tc->realm);
        rtc_stun_add_nonce(msg, tc->nonce);
    }
    return RTC_OK;
}

static int finalize_with_key(rtc_turn_client_t *tc, rtc_stun_msg_t *msg) {
    if (tc->has_credentials) {
        /* Use long-term key as the HMAC password */
        return rtc_stun_finalize(msg, (const char *)tc->lt_key);
    }
    return rtc_stun_finalize(msg, NULL);
}

rtc_err_t rtc_turn_init(rtc_turn_client_t *tc, rtc_transport_t *transport,
                        const rtc_turn_config_t *cfg) {
    memset(tc, 0, sizeof(*tc));
    tc->cfg = *cfg;
    tc->transport = transport;

    /* Resolve server address */
    int rc = rtc_addr_from_string(&tc->server_addr, cfg->server_host, cfg->server_port);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("TURN: failed to resolve server %s:%d", cfg->server_host, cfg->server_port);
        return rc;
    }

    RTC_LOG_INFO("TURN: client initialized for %s:%d", cfg->server_host, cfg->server_port);
    return RTC_OK;
}

rtc_err_t rtc_turn_allocate(rtc_turn_client_t *tc) {
    rtc_stun_msg_t req, resp;

    /* First attempt: unauthenticated Allocate */
    int rc = rtc_stun_build_request(&req, STUN_METHOD_ALLOCATE, NULL, NULL);
    if (rc != RTC_OK)
        return rc;
    rtc_stun_add_requested_transport(&req, 17); /* UDP */
    rtc_stun_finalize(&req, NULL);

    rc = turn_transaction(tc, &req, &resp);
    if (rc != RTC_OK)
        return rc;

    /* Expect 401 Unauthorized with REALM + NONCE */
    if (resp.type == STUN_ALLOCATE_ERROR_RESPONSE) {
        int err_code = 0;
        rtc_stun_get_error_code(&resp, &err_code);

        if (err_code == 401) {
            /* Extract realm and nonce */
            if (rtc_stun_get_realm(&resp, tc->realm, sizeof(tc->realm)) != RTC_OK ||
                rtc_stun_get_nonce(&resp, tc->nonce, sizeof(tc->nonce)) != RTC_OK) {
                RTC_LOG_ERR("TURN: 401 missing realm/nonce");
                return RTC_ERR_INVALID;
            }

            /* Compute long-term key */
            rc =
                rtc_stun_long_term_key(tc->cfg.username, tc->realm, tc->cfg.credential, tc->lt_key);
            if (rc != RTC_OK)
                return rc;
            tc->has_credentials = true;

            /* Retry with credentials */
            build_authenticated_request(tc, &req, STUN_METHOD_ALLOCATE);
            rtc_stun_add_requested_transport(&req, 17);
            /* Use lt_key (16 bytes) as HMAC key — need to pass as string */
            finalize_with_key(tc, &req);

            rc = turn_transaction(tc, &req, &resp);
            if (rc != RTC_OK)
                return rc;
        } else {
            RTC_LOG_ERR("TURN: Allocate error %d", err_code);
            return RTC_ERR_GENERIC;
        }
    }

    if (resp.type != STUN_ALLOCATE_RESPONSE) {
        RTC_LOG_ERR("TURN: unexpected response 0x%04x", resp.type);
        return RTC_ERR_INVALID;
    }

    /* Extract relay address and lifetime */
    rc = rtc_stun_get_relayed_address(&resp, &tc->relay_addr);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("TURN: no relayed address in response");
        return rc;
    }

    rtc_stun_get_lifetime(&resp, &tc->lifetime);
    tc->allocated = true;

    char ip[64];
    uint16_t port;
    rtc_addr_to_string(&tc->relay_addr, ip, sizeof(ip), &port);
    RTC_LOG_INFO("TURN: allocated relay %s:%d (lifetime=%ds)", ip, port, tc->lifetime);

    return RTC_OK;
}

rtc_err_t rtc_turn_create_permission(rtc_turn_client_t *tc, const rtc_addr_t *peer) {
    if (!tc->allocated)
        return RTC_ERR_INVALID;

    rtc_stun_msg_t req, resp;
    build_authenticated_request(tc, &req, STUN_METHOD_CREATE_PERM);
    rtc_stun_add_xor_peer_address(&req, peer);
    finalize_with_key(tc, &req);

    int rc = turn_transaction(tc, &req, &resp);
    if (rc != RTC_OK)
        return rc;

    if (resp.type != STUN_CREATE_PERM_RESPONSE) {
        RTC_LOG_ERR("TURN: CreatePermission failed (0x%04x)", resp.type);
        return RTC_ERR_GENERIC;
    }

    /* Store permission */
    if (tc->permission_count < TURN_MAX_PERMISSIONS) {
        tc->permissions[tc->permission_count].addr = *peer;
        tc->permissions[tc->permission_count].active = true;
        tc->permission_count++;
    }

    RTC_LOG_INFO("TURN: permission created");
    return RTC_OK;
}

rtc_err_t rtc_turn_channel_bind(rtc_turn_client_t *tc, const rtc_addr_t *peer, uint16_t channel) {
    if (!tc->allocated)
        return RTC_ERR_INVALID;
    if (channel < TURN_CHANNEL_MIN || channel > TURN_CHANNEL_MAX)
        return RTC_ERR_INVALID;

    rtc_stun_msg_t req, resp;
    build_authenticated_request(tc, &req, STUN_METHOD_CHANNEL_BIND);
    rtc_stun_add_channel_number(&req, channel);
    rtc_stun_add_xor_peer_address(&req, peer);
    finalize_with_key(tc, &req);

    int rc = turn_transaction(tc, &req, &resp);
    if (rc != RTC_OK)
        return rc;

    if (resp.type != STUN_CHANNEL_BIND_RESPONSE) {
        RTC_LOG_ERR("TURN: ChannelBind failed (0x%04x)", resp.type);
        return RTC_ERR_GENERIC;
    }

    /* Store binding */
    if (tc->channel_count < TURN_MAX_CHANNELS) {
        tc->channels[tc->channel_count].channel = channel;
        tc->channels[tc->channel_count].peer_addr = *peer;
        tc->channels[tc->channel_count].active = true;
        tc->channel_count++;
    }

    RTC_LOG_INFO("TURN: channel 0x%04x bound", channel);
    return RTC_OK;
}

rtc_err_t rtc_turn_send(rtc_turn_client_t *tc, uint16_t channel, const uint8_t *data, size_t len) {
    if (!tc->allocated)
        return RTC_ERR_INVALID;

    uint8_t buf[2048];
    size_t frame_len;
    int rc = rtc_turn_build_channel_data(buf, sizeof(buf), channel, data, len, &frame_len);
    if (rc != RTC_OK)
        return rc;

    ssize_t sent = sendto(tc->transport->sock, (const char *)buf, (int)frame_len, 0,
                          (struct sockaddr *)&tc->server_addr.addr, tc->server_addr.len);
    return sent >= 0 ? RTC_OK : RTC_ERR_SOCKET;
}

void rtc_turn_close(rtc_turn_client_t *tc) {
    if (!tc->allocated)
        return;

    /* Send Refresh with lifetime=0 to deallocate */
    rtc_stun_msg_t req, resp;
    build_authenticated_request(tc, &req, STUN_METHOD_REFRESH);
    rtc_stun_add_lifetime(&req, 0);
    finalize_with_key(tc, &req);

    /* Best-effort, don't check result */
    turn_transaction(tc, &req, &resp);

    tc->allocated = false;
    RTC_LOG_INFO("TURN: allocation released");
}
