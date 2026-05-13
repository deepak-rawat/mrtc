/*
 * turn_handler.c - TURN server message handling and allocation management.
 */
#include "turn_handler.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Helpers - reuse STUN read/write from rtc_stun (they're static there, so redefine) */
static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}
static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}
static uint16_t read_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static int addr_match(const rtc_addr_t *a, const rtc_addr_t *b) {
    if (a->len != b->len)
        return 0;
    return memcmp(&a->addr, &b->addr, a->len) == 0;
}

static int addr_ip_match(const rtc_addr_t *a, const rtc_addr_t *b) {
    const struct sockaddr_in *sa = (const struct sockaddr_in *)&a->addr;
    const struct sockaddr_in *sb = (const struct sockaddr_in *)&b->addr;
    if (sa->sin_family != sb->sin_family)
        return 0;
    return sa->sin_addr.s_addr == sb->sin_addr.s_addr;
}

static turn_allocation_t *find_alloc(turn_server_t *ts, const rtc_addr_t *client) {
    for (int i = 0; i < TURN_MAX_ALLOCATIONS; i++) {
        if (ts->allocs[i].active && addr_match(&ts->allocs[i].client_addr, client))
            return &ts->allocs[i];
    }
    return NULL;
}

static void send_response(turn_server_t *ts, const uint8_t *data, size_t len,
                          const rtc_addr_t *to) {
    sendto(ts->listen_sock, (const char *)data, (int)len, 0, (struct sockaddr *)&to->addr, to->len);
}

/* Build a simple STUN success response */
static int build_success_response(rtc_stun_msg_t *resp, uint16_t resp_type,
                                  const uint8_t txn_id[STUN_TXN_ID_SIZE]) {
    memset(resp, 0, sizeof(*resp));
    resp->type = resp_type;
    memcpy(resp->txn_id, txn_id, STUN_TXN_ID_SIZE);
    write_u16(resp->buf, resp_type);
    write_u16(resp->buf + 2, 0);
    write_u32(resp->buf + 4, STUN_MAGIC_COOKIE);
    memcpy(resp->buf + 8, txn_id, STUN_TXN_ID_SIZE);
    resp->buf_len = STUN_HEADER_SIZE;
    return RTC_OK;
}

/* Build a STUN error response */
static void send_error(turn_server_t *ts, uint16_t method, const uint8_t txn_id[12], int error_code,
                       const rtc_addr_t *to) {
    rtc_stun_msg_t resp;
    uint16_t resp_type = method | 0x0110; /* error response class */
    build_success_response(&resp, resp_type, txn_id);

    /* ERROR-CODE attribute */
    uint8_t err[4];
    err[0] = 0;
    err[1] = 0;
    err[2] = (uint8_t)(error_code / 100);
    err[3] = (uint8_t)(error_code % 100);
    rtc_stun_msg_t tmp = resp;
    /* Manually add attr */
    size_t pos = resp.buf_len;
    write_u16(resp.buf + pos, STUN_ATTR_ERROR_CODE);
    pos += 2;
    write_u16(resp.buf + pos, 4);
    pos += 2;
    memcpy(resp.buf + pos, err, 4);
    pos += 4;

    if (error_code == 401) {
        /* Add REALM */
        size_t rlen = strlen(ts->realm);
        write_u16(resp.buf + pos, STUN_ATTR_REALM);
        pos += 2;
        write_u16(resp.buf + pos, (uint16_t)rlen);
        pos += 2;
        memcpy(resp.buf + pos, ts->realm, rlen);
        pos += (rlen + 3) & ~(size_t)3;

        /* Add NONCE */
        size_t nlen = strlen(ts->nonce);
        write_u16(resp.buf + pos, STUN_ATTR_NONCE);
        pos += 2;
        write_u16(resp.buf + pos, (uint16_t)nlen);
        pos += 2;
        memcpy(resp.buf + pos, ts->nonce, nlen);
        pos += (nlen + 3) & ~(size_t)3;
    }

    resp.buf_len = pos;
    resp.length = (uint16_t)(pos - STUN_HEADER_SIZE);
    write_u16(resp.buf + 2, resp.length);
    (void)tmp;

    send_response(ts, resp.buf, resp.buf_len, to);
}

/* Verify long-term credentials on a request */
static int verify_credentials(turn_server_t *ts, const rtc_stun_msg_t *msg, const uint8_t *raw,
                              size_t raw_len) {
    uint16_t ulen;
    const uint8_t *uval = rtc_stun_find_attr(msg, STUN_ATTR_USERNAME, &ulen);
    if (!uval)
        return -1;

    /* Check username matches */
    if (ulen != strlen(ts->username) || memcmp(uval, ts->username, ulen) != 0)
        return -1;

    /* Verify MESSAGE-INTEGRITY with long-term key */
    return rtc_stun_verify_integrity(raw, raw_len, (const char *)ts->lt_key);
}

static void handle_allocate(turn_server_t *ts, const rtc_stun_msg_t *msg, const uint8_t *raw,
                            size_t raw_len, const rtc_addr_t *from) {
    /* Check credentials */
    if (verify_credentials(ts, msg, raw, raw_len) != RTC_OK) {
        send_error(ts, STUN_METHOD_ALLOCATE, msg->txn_id, 401, from);
        return;
    }

    /* Check if already allocated */
    if (find_alloc(ts, from)) {
        send_error(ts, STUN_METHOD_ALLOCATE, msg->txn_id, 437, from); /* Allocation mismatch */
        return;
    }

    /* Find free slot */
    turn_allocation_t *alloc = NULL;
    for (int i = 0; i < TURN_MAX_ALLOCATIONS; i++) {
        if (!ts->allocs[i].active) {
            alloc = &ts->allocs[i];
            break;
        }
    }
    if (!alloc) {
        send_error(ts, STUN_METHOD_ALLOCATE, msg->txn_id, 508, from); /* Insufficient capacity */
        return;
    }

    /* Create relay socket */
    memset(alloc, 0, sizeof(*alloc));
    alloc->relay_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (alloc->relay_sock == RTC_INVALID_SOCKET) {
        send_error(ts, STUN_METHOD_ALLOCATE, msg->txn_id, 500, from);
        return;
    }

    /* Bind relay to ephemeral port on public IP */
    struct sockaddr_in relay_sin;
    memset(&relay_sin, 0, sizeof(relay_sin));
    relay_sin.sin_family = AF_INET;
    relay_sin.sin_port = 0; /* ephemeral */
    inet_pton(AF_INET, ts->public_ip, &relay_sin.sin_addr);

    if (bind(alloc->relay_sock, (struct sockaddr *)&relay_sin, sizeof(relay_sin)) < 0) {
        rtc_close_socket(alloc->relay_sock);
        send_error(ts, STUN_METHOD_ALLOCATE, msg->txn_id, 500, from);
        return;
    }

    /* Get assigned port */
    socklen_t slen = sizeof(relay_sin);
    getsockname(alloc->relay_sock, (struct sockaddr *)&relay_sin, &slen);

    rtc_set_nonblocking(alloc->relay_sock);

    /* Fill allocation */
    alloc->active = true;
    alloc->client_addr = *from;
    memcpy(&alloc->relay_addr.addr, &relay_sin, sizeof(relay_sin));
    alloc->relay_addr.len = sizeof(relay_sin);
    alloc->lifetime = TURN_DEFAULT_LIFETIME;
    alloc->expires_ms = rtc_time_ms() + (uint64_t)alloc->lifetime * 1000;
    rtc_vec_init(&alloc->channels, sizeof(turn_channel_t));
    rtc_vec_init(&alloc->permissions, sizeof(turn_permission_t));
    ts->alloc_count++;

    /* Build success response */
    rtc_stun_msg_t resp;
    build_success_response(&resp, STUN_ALLOCATE_RESPONSE, msg->txn_id);

    /* Add XOR-RELAYED-ADDRESS */
    uint8_t xra[8];
    xra[0] = 0;
    xra[1] = 0x01;
    uint16_t rport = ntohs(relay_sin.sin_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    write_u16(xra + 2, rport);
    uint32_t rip = ntohl(relay_sin.sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;
    write_u32(xra + 4, rip);
    size_t pos = resp.buf_len;
    write_u16(resp.buf + pos, STUN_ATTR_XOR_RELAYED_ADDR);
    pos += 2;
    write_u16(resp.buf + pos, 8);
    pos += 2;
    memcpy(resp.buf + pos, xra, 8);
    pos += 8;

    /* Add LIFETIME */
    write_u16(resp.buf + pos, STUN_ATTR_LIFETIME);
    pos += 2;
    write_u16(resp.buf + pos, 4);
    pos += 2;
    write_u32(resp.buf + pos, alloc->lifetime);
    pos += 4;

    /* Add XOR-MAPPED-ADDRESS (client's address as seen by server) */
    const struct sockaddr_in *csin = (const struct sockaddr_in *)&from->addr;
    uint8_t xma[8];
    xma[0] = 0;
    xma[1] = 0x01;
    write_u16(xma + 2, ntohs(csin->sin_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));
    write_u32(xma + 4, ntohl(csin->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE);
    write_u16(resp.buf + pos, STUN_ATTR_XOR_MAPPED_ADDRESS);
    pos += 2;
    write_u16(resp.buf + pos, 8);
    pos += 2;
    memcpy(resp.buf + pos, xma, 8);
    pos += 8;

    resp.buf_len = pos;
    resp.length = (uint16_t)(pos - STUN_HEADER_SIZE);
    write_u16(resp.buf + 2, resp.length);

    send_response(ts, resp.buf, resp.buf_len, from);

    char ip[64];
    uint16_t p;
    rtc_addr_to_string(&alloc->relay_addr, ip, sizeof(ip), &p);
    RTC_LOG_INFO("TURN: allocated relay %s:%d for client", ip, p);
}

static void handle_refresh(turn_server_t *ts, const rtc_stun_msg_t *msg, const uint8_t *raw,
                           size_t raw_len, const rtc_addr_t *from) {
    if (verify_credentials(ts, msg, raw, raw_len) != RTC_OK) {
        send_error(ts, STUN_METHOD_REFRESH, msg->txn_id, 401, from);
        return;
    }

    turn_allocation_t *alloc = find_alloc(ts, from);
    if (!alloc) {
        send_error(ts, STUN_METHOD_REFRESH, msg->txn_id, 437, from);
        return;
    }

    uint32_t lifetime = TURN_DEFAULT_LIFETIME;
    rtc_stun_get_lifetime(msg, &lifetime);

    if (lifetime == 0) {
        /* Deallocate */
        rtc_close_socket(alloc->relay_sock);
        rtc_vec_free(&alloc->channels);
        rtc_vec_free(&alloc->permissions);
        alloc->active = false;
        ts->alloc_count--;
        RTC_LOG_INFO("TURN: allocation released by refresh(0)");
    } else {
        alloc->lifetime = lifetime;
        alloc->expires_ms = rtc_time_ms() + (uint64_t)lifetime * 1000;
    }

    /* Success response */
    rtc_stun_msg_t resp;
    build_success_response(&resp, STUN_REFRESH_RESPONSE, msg->txn_id);
    size_t pos = resp.buf_len;
    write_u16(resp.buf + pos, STUN_ATTR_LIFETIME);
    pos += 2;
    write_u16(resp.buf + pos, 4);
    pos += 2;
    write_u32(resp.buf + pos, lifetime);
    pos += 4;
    resp.buf_len = pos;
    write_u16(resp.buf + 2, (uint16_t)(pos - STUN_HEADER_SIZE));
    send_response(ts, resp.buf, resp.buf_len, from);
}

static void handle_create_permission(turn_server_t *ts, const rtc_stun_msg_t *msg,
                                     const uint8_t *raw, size_t raw_len, const rtc_addr_t *from) {
    if (verify_credentials(ts, msg, raw, raw_len) != RTC_OK) {
        send_error(ts, STUN_METHOD_CREATE_PERM, msg->txn_id, 401, from);
        return;
    }

    turn_allocation_t *alloc = find_alloc(ts, from);
    if (!alloc) {
        send_error(ts, STUN_METHOD_CREATE_PERM, msg->txn_id, 437, from);
        return;
    }

    /* Extract XOR-PEER-ADDRESS */
    uint16_t alen;
    const uint8_t *val = rtc_stun_find_attr(msg, STUN_ATTR_XOR_PEER_ADDRESS, &alen);
    if (!val || alen < 8) {
        send_error(ts, STUN_METHOD_CREATE_PERM, msg->txn_id, 400, from);
        return;
    }

    rtc_addr_t peer;
    memset(&peer, 0, sizeof(peer));
    struct sockaddr_in *sin = (struct sockaddr_in *)&peer.addr;
    sin->sin_family = AF_INET;
    uint16_t port = read_u16(val + 2) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    sin->sin_port = htons(port);
    uint32_t ip =
        (((uint32_t)val[4] << 24) | ((uint32_t)val[5] << 16) | ((uint32_t)val[6] << 8) | val[7]) ^
        STUN_MAGIC_COOKIE;
    sin->sin_addr.s_addr = htonl(ip);
    peer.len = sizeof(struct sockaddr_in);

    /* Add permission */
    turn_permission_t perm = {.addr = peer, .active = true};
    rtc_vec_push(&alloc->permissions, &perm);

    /* Success */
    rtc_stun_msg_t resp;
    build_success_response(&resp, STUN_CREATE_PERM_RESPONSE, msg->txn_id);
    send_response(ts, resp.buf, resp.buf_len, from);
    RTC_LOG_DBG("TURN: permission created");
}

static void handle_channel_bind(turn_server_t *ts, const rtc_stun_msg_t *msg, const uint8_t *raw,
                                size_t raw_len, const rtc_addr_t *from) {
    if (verify_credentials(ts, msg, raw, raw_len) != RTC_OK) {
        send_error(ts, STUN_METHOD_CHANNEL_BIND, msg->txn_id, 401, from);
        return;
    }

    turn_allocation_t *alloc = find_alloc(ts, from);
    if (!alloc) {
        send_error(ts, STUN_METHOD_CHANNEL_BIND, msg->txn_id, 437, from);
        return;
    }

    /* CHANNEL-NUMBER */
    uint16_t clen;
    const uint8_t *cval = rtc_stun_find_attr(msg, STUN_ATTR_CHANNEL_NUMBER, &clen);
    if (!cval || clen < 4) {
        send_error(ts, STUN_METHOD_CHANNEL_BIND, msg->txn_id, 400, from);
        return;
    }
    uint16_t channel = read_u16(cval);

    /* XOR-PEER-ADDRESS */
    uint16_t plen;
    const uint8_t *pval = rtc_stun_find_attr(msg, STUN_ATTR_XOR_PEER_ADDRESS, &plen);
    if (!pval || plen < 8) {
        send_error(ts, STUN_METHOD_CHANNEL_BIND, msg->txn_id, 400, from);
        return;
    }

    rtc_addr_t peer;
    memset(&peer, 0, sizeof(peer));
    struct sockaddr_in *sin = (struct sockaddr_in *)&peer.addr;
    sin->sin_family = AF_INET;
    uint16_t port = read_u16(pval + 2) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    sin->sin_port = htons(port);
    uint32_t ip = (((uint32_t)pval[4] << 24) | ((uint32_t)pval[5] << 16) |
                   ((uint32_t)pval[6] << 8) | pval[7]) ^
                  STUN_MAGIC_COOKIE;
    sin->sin_addr.s_addr = htonl(ip);
    peer.len = sizeof(struct sockaddr_in);

    /* Store channel binding */
    turn_channel_t cb = {.channel = channel, .peer_addr = peer, .active = true};
    rtc_vec_push(&alloc->channels, &cb);

    /* Success */
    rtc_stun_msg_t resp;
    build_success_response(&resp, STUN_CHANNEL_BIND_RESPONSE, msg->txn_id);
    send_response(ts, resp.buf, resp.buf_len, from);

    RTC_LOG_DBG("TURN: channel 0x%04x bound", channel);
}

static void handle_channel_data(turn_server_t *ts, const uint8_t *data, size_t len,
                                const rtc_addr_t *from) {
    uint16_t channel;
    const uint8_t *payload;
    size_t payload_len;
    if (rtc_turn_parse_channel_data(data, len, &channel, &payload, &payload_len) != RTC_OK)
        return;

    turn_allocation_t *alloc = find_alloc(ts, from);
    if (!alloc)
        return;

    /* Find channel binding → peer address */
    size_t n_ch = rtc_vec_len(&alloc->channels);
    for (size_t i = 0; i < n_ch; i++) {
        turn_channel_t *ch = (turn_channel_t *)rtc_vec_at(&alloc->channels, i);
        if (ch->active && ch->channel == channel) {
            /* Check permission */
            bool permitted = false;
            size_t n_perm = rtc_vec_len(&alloc->permissions);
            for (size_t j = 0; j < n_perm; j++) {
                turn_permission_t *perm = (turn_permission_t *)rtc_vec_at(&alloc->permissions, j);
                if (perm->active && addr_ip_match(&perm->addr, &ch->peer_addr)) {
                    permitted = true;
                    break;
                }
            }
            if (!permitted)
                return;

            /* Forward via relay socket to peer */
            sendto(alloc->relay_sock, (const char *)payload, (int)payload_len, 0,
                   (struct sockaddr *)&ch->peer_addr.addr, ch->peer_addr.len);
            return;
        }
    }
}

/* ---- Public API ---- */

int turn_server_init(turn_server_t *ts, const char *public_ip, uint16_t port, const char *username,
                     const char *password, const char *realm) {
    memset(ts, 0, sizeof(*ts));
    ts->username = username;
    ts->password = password;
    ts->realm = realm;
    ts->public_ip = public_ip;

    /* Compute long-term key */
    rtc_stun_long_term_key(username, realm, password, ts->lt_key);

    /* Generate nonce */
    rtc_random_string(ts->nonce, sizeof(ts->nonce));

    /* Create listen socket */
    ts->listen_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ts->listen_sock == RTC_INVALID_SOCKET)
        return RTC_ERR_SOCKET;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(ts->listen_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        rtc_close_socket(ts->listen_sock);
        return RTC_ERR_SOCKET;
    }

    rtc_set_nonblocking(ts->listen_sock);

    RTC_LOG_INFO("TURN server listening on %s:%d", public_ip, port);
    return RTC_OK;
}

void turn_server_handle_packet(turn_server_t *ts, const uint8_t *data, size_t len,
                               const rtc_addr_t *from) {
    if (len < 4)
        return;

    /* Demux: ChannelData (first byte 0x40-0x7F) vs STUN (first byte 0x00-0x01) */
    if (data[0] >= 0x40 && data[0] <= 0x7F) {
        handle_channel_data(ts, data, len, from);
        return;
    }

    /* STUN message */
    if (len < STUN_HEADER_SIZE)
        return;

    rtc_stun_msg_t msg;
    if (rtc_stun_parse(&msg, data, len) != RTC_OK)
        return;

    /* Extract method (mask out class bits) */
    uint16_t method = msg.type & 0x3EEF;

    switch (method) {
        case STUN_METHOD_ALLOCATE:
            handle_allocate(ts, &msg, data, len, from);
            break;
        case STUN_METHOD_REFRESH:
            handle_refresh(ts, &msg, data, len, from);
            break;
        case STUN_METHOD_CREATE_PERM:
            handle_create_permission(ts, &msg, data, len, from);
            break;
        case STUN_METHOD_CHANNEL_BIND:
            handle_channel_bind(ts, &msg, data, len, from);
            break;
        default:
            RTC_LOG_WARN("TURN: unknown method 0x%04x", method);
            break;
    }
}

void turn_server_relay_from_peer(turn_server_t *ts, turn_allocation_t *alloc, const uint8_t *data,
                                 size_t len, const rtc_addr_t *peer_from) {
    /* Find channel bound to this peer */
    size_t n_ch = rtc_vec_len(&alloc->channels);
    for (size_t i = 0; i < n_ch; i++) {
        turn_channel_t *ch = (turn_channel_t *)rtc_vec_at(&alloc->channels, i);
        if (ch->active && addr_match(&ch->peer_addr, peer_from)) {
            /* Wrap in ChannelData and send to client */
            uint8_t buf[2048];
            size_t frame_len;
            if (rtc_turn_build_channel_data(buf, sizeof(buf), ch->channel, data, len, &frame_len) ==
                RTC_OK) {
                sendto(ts->listen_sock, (const char *)buf, (int)frame_len, 0,
                       (struct sockaddr *)&alloc->client_addr.addr, alloc->client_addr.len);
            }
            return;
        }
    }
}

void turn_server_expire(turn_server_t *ts) {
    uint64_t now = rtc_time_ms();
    for (int i = 0; i < TURN_MAX_ALLOCATIONS; i++) {
        if (ts->allocs[i].active && ts->allocs[i].expires_ms <= now) {
            rtc_close_socket(ts->allocs[i].relay_sock);
            rtc_vec_free(&ts->allocs[i].channels);
            rtc_vec_free(&ts->allocs[i].permissions);
            ts->allocs[i].active = false;
            ts->alloc_count--;
            RTC_LOG_INFO("TURN: allocation expired");
        }
    }
}

void turn_server_close(turn_server_t *ts) {
    for (int i = 0; i < TURN_MAX_ALLOCATIONS; i++) {
        if (ts->allocs[i].active) {
            rtc_close_socket(ts->allocs[i].relay_sock);
            rtc_vec_free(&ts->allocs[i].channels);
            rtc_vec_free(&ts->allocs[i].permissions);
            ts->allocs[i].active = false;
        }
    }
    rtc_close_socket(ts->listen_sock);
    ts->alloc_count = 0;
}
