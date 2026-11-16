#include "rtc_turn_runtime.h"
#include "rtc_worker_internal.h"

#include <stdlib.h>
#include <string.h>

struct rtc_turn_runtime {
    rtc_worker_t *worker;
    rtc_packet_io_t io;
    rtc_turn_client_t client;
    char host[256], username[128], credential[256];
    rtc_packet_io_recv_fn receive;
    void *user;
    rtc_worker_timer_t timer;
    rtc_stun_msg_t request;
    uint16_t method;
    unsigned attempts;
    unsigned challenges;
    uint64_t refresh_ms;
    _Atomic uint64_t allocation_expires_ms;
    rtc_addr_t peer;
    bool peer_set;
    bool bound;
    bool failed;
    rtc_mutex_t mutex;
};

static bool turn_same_addr(const rtc_addr_t *a, const rtc_addr_t *b) {
    char ai[64], bi[64];
    uint16_t ap, bp;
    return rtc_addr_to_string(a, ai, sizeof(ai), &ap) == RTC_OK &&
           rtc_addr_to_string(b, bi, sizeof(bi), &bp) == RTC_OK && ap == bp && strcmp(ai, bi) == 0;
}

static int turn_build(rtc_turn_runtime_t *turn, uint16_t method, uint32_t lifetime) {
    rtc_turn_client_t *c = &turn->client;
    rtc_stun_msg_t *r = &turn->request;
    int rc = rtc_stun_build_request(r, method, NULL, NULL);
    if (rc != RTC_OK)
        return rc;
    if (c->has_credentials) {
        if (rtc_stun_add_username(r, turn->username) != RTC_OK ||
            rtc_stun_add_realm(r, c->realm) != RTC_OK || rtc_stun_add_nonce(r, c->nonce) != RTC_OK)
            return RTC_ERR_INVALID;
    }
    if (method == STUN_METHOD_ALLOCATE)
        rtc_stun_add_requested_transport(r, 17);
    if (method == STUN_METHOD_REFRESH)
        rtc_stun_add_lifetime(r, lifetime);
    if (method == STUN_METHOD_CREATE_PERM || method == STUN_METHOD_CHANNEL_BIND)
        rtc_stun_add_xor_peer_address(r, &turn->peer);
    if (method == STUN_METHOD_CHANNEL_BIND)
        rtc_stun_add_channel_number(r, TURN_CHANNEL_MIN);
    rc = rtc_stun_finalize_key(r, c->has_credentials ? c->lt_key : NULL,
                               c->has_credentials ? sizeof(c->lt_key) : 0);
    if (rc != RTC_OK)
        return rc;
    turn->method = method;
    turn->attempts = 0;
    return RTC_OK;
}

static void turn_tick(void *user);

static void turn_arm(rtc_turn_runtime_t *turn, uint64_t delay) {
    if (turn->timer != RTC_WORKER_TIMER_INVALID)
        rtc_worker_cancel_timer(turn->worker, turn->timer);
    turn->timer = rtc_worker_add_timer(turn->worker, rtc_time_ms() + delay, turn_tick, turn);
}

static int turn_send_request(rtc_turn_runtime_t *turn) {
    int rc = rtc_packet_io_send(&turn->io, turn->request.buf, turn->request.buf_len,
                                &turn->client.server_addr);
    turn->attempts++;
    turn_arm(turn, 500);
    return rc;
}

static void turn_tick(void *user) {
    rtc_turn_runtime_t *turn = user;
    turn->timer = RTC_WORKER_TIMER_INVALID;
    if (turn->failed)
        return;
    if (turn->client.allocated && rtc_time_ms() >= atomic_load(&turn->allocation_expires_ms)) {
        rtc_mutex_lock(&turn->mutex);
        turn->bound = false;
        turn->failed = true;
        rtc_mutex_unlock(&turn->mutex);
        return;
    }
    if (turn->method) {
        if (turn->attempts >= 7) {
            rtc_mutex_lock(&turn->mutex);
            turn->bound = false;
            turn->failed = true;
            rtc_mutex_unlock(&turn->mutex);
            return;
        }
        (void)turn_send_request(turn);
        return;
    }
    if (rtc_time_ms() >= turn->refresh_ms) {
        if (turn_build(turn, STUN_METHOD_REFRESH, 600) == RTC_OK)
            (void)turn_send_request(turn);
    } else {
        turn_arm(turn, turn->refresh_ms - rtc_time_ms());
    }
}

static rtc_pkt_type_t turn_packet_type(const uint8_t *data, size_t len) {
    if (!len)
        return RTC_PKT_UNKNOWN;
    if (data[0] <= 3)
        return RTC_PKT_STUN;
    if (data[0] >= 20 && data[0] <= 63)
        return RTC_PKT_DTLS;
    if (data[0] >= 128 && data[0] <= 191 && len >= 2)
        return data[1] >= 192 && data[1] <= 223 ? RTC_PKT_RTCP : RTC_PKT_RTP;
    return RTC_PKT_UNKNOWN;
}

static void turn_receive(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                         const rtc_addr_t *from, void *user) {
    rtc_turn_runtime_t *turn = user;
    rtc_turn_client_t *c = &turn->client;
    if (!turn_same_addr(from, &c->server_addr) || turn->failed)
        return;
    if (type == RTC_PKT_CHANNEL_DATA) {
        uint16_t channel;
        const uint8_t *payload;
        size_t size;
        if (turn->bound &&
            rtc_turn_parse_channel_data(data, len, &channel, &payload, &size) == RTC_OK &&
            channel == TURN_CHANNEL_MIN)
            turn->receive(turn_packet_type(payload, size), payload, size, &turn->peer, turn->user);
        return;
    }
    rtc_stun_msg_t response;
    if (type != RTC_PKT_STUN || rtc_stun_parse(&response, data, len) != RTC_OK || !turn->method ||
        memcmp(response.txn_id, turn->request.txn_id, STUN_TXN_ID_SIZE) != 0)
        return;
    int code = 0;
    if (rtc_stun_get_error_code(&response, &code) == RTC_OK) {
        if ((code != 401 && code != 438) || ++turn->challenges > 3 ||
            (code == 401 && c->has_credentials) ||
            rtc_stun_get_realm(&response, c->realm, sizeof(c->realm)) != RTC_OK ||
            rtc_stun_get_nonce(&response, c->nonce, sizeof(c->nonce)) != RTC_OK ||
            rtc_stun_long_term_key(turn->username, c->realm, turn->credential, c->lt_key) !=
                RTC_OK) {
            rtc_mutex_lock(&turn->mutex);
            turn->failed = true;
            turn->bound = false;
            rtc_mutex_unlock(&turn->mutex);
            return;
        }
        c->has_credentials = true;
        if (turn_build(turn, turn->method, 600) == RTC_OK)
            (void)turn_send_request(turn);
        return;
    }
    if (response.type != (uint16_t)(turn->method | 0x0100) || !c->has_credentials ||
        rtc_stun_verify_integrity_key(data, len, c->lt_key, sizeof(c->lt_key)) != RTC_OK)
        return;
    uint16_t method = turn->method;
    turn->method = 0;
    turn->challenges = 0;
    if (method == STUN_METHOD_ALLOCATE || method == STUN_METHOD_REFRESH) {
        if (rtc_stun_get_lifetime(&response, &c->lifetime) != RTC_OK || !c->lifetime ||
            (method == STUN_METHOD_ALLOCATE &&
             rtc_stun_get_relayed_address(&response, &c->relay_addr) != RTC_OK)) {
            turn->failed = true;
            return;
        }
        c->allocated = true;
        atomic_store(&turn->allocation_expires_ms, rtc_time_ms() + (uint64_t)c->lifetime * 1000);
        uint64_t refresh = (uint64_t)c->lifetime * 500;
        if (refresh > 120000)
            refresh = 120000;
        turn->refresh_ms = rtc_time_ms() + refresh;
        if (turn->peer_set) {
            if (turn_build(turn, STUN_METHOD_CREATE_PERM, 0) == RTC_OK)
                (void)turn_send_request(turn);
            return;
        }
    } else if (method == STUN_METHOD_CREATE_PERM) {
        if (turn_build(turn, STUN_METHOD_CHANNEL_BIND, 0) == RTC_OK)
            (void)turn_send_request(turn);
        return;
    } else if (method == STUN_METHOD_CHANNEL_BIND) {
        rtc_mutex_lock(&turn->mutex);
        turn->bound = true;
        rtc_mutex_unlock(&turn->mutex);
    }
    turn_arm(turn, turn->refresh_ms > rtc_time_ms() ? turn->refresh_ms - rtc_time_ms() : 1);
}

static void turn_io_ready(rtc_socket_t fd, void *user) {
    (void)fd;
    rtc_turn_runtime_t *turn = user;
    rtc_packet_io_drain(&turn->io);
}

rtc_turn_runtime_t *rtc_turn_runtime_create(rtc_worker_t *worker, const rtc_turn_config_t *cfg,
                                            rtc_packet_io_recv_fn receive, void *user) {
    if (!worker || !cfg || !cfg->server_host || !cfg->username || !cfg->credential || !receive)
        return NULL;
    rtc_turn_runtime_t *turn = calloc(1, sizeof(*turn));
    if (!turn)
        return NULL;
    turn->worker = worker;
    turn->receive = receive;
    turn->user = user;
    turn->timer = RTC_WORKER_TIMER_INVALID;
    if (strlen(cfg->server_host) >= sizeof(turn->host) ||
        strlen(cfg->username) >= sizeof(turn->username) ||
        strlen(cfg->credential) >= sizeof(turn->credential)) {
        free(turn);
        return NULL;
    }
    strcpy(turn->host, cfg->server_host);
    strcpy(turn->username, cfg->username);
    strcpy(turn->credential, cfg->credential);
    rtc_turn_config_t owned = {turn->host, cfg->server_port ? cfg->server_port : 3478,
                               turn->username, turn->credential};
    if (rtc_mutex_init(&turn->mutex) != RTC_OK) {
        free(turn);
        return NULL;
    }
    if (rtc_packet_io_init(&turn->io, turn_receive, turn) != RTC_OK) {
        rtc_mutex_destroy(&turn->mutex);
        free(turn);
        return NULL;
    }
    if (rtc_turn_init(&turn->client, &turn->io, &owned) != RTC_OK ||
        rtc_worker_add_io(worker, rtc_packet_io_get_socket(&turn->io), turn_io_ready, turn) !=
            RTC_OK ||
        turn_build(turn, STUN_METHOD_ALLOCATE, 0) != RTC_OK) {
        rtc_turn_runtime_destroy(turn);
        return NULL;
    }
    (void)turn_send_request(turn);
    return turn;
}

int rtc_turn_runtime_candidate(rtc_turn_runtime_t *turn, rtc_addr_t *relay) {
    if (!turn || !relay || !turn->client.allocated || turn->failed ||
        rtc_time_ms() >= atomic_load(&turn->allocation_expires_ms))
        return RTC_ERR_INVALID;
    *relay = turn->client.relay_addr;
    return RTC_OK;
}

int rtc_turn_runtime_set_peer(rtc_turn_runtime_t *turn, const rtc_addr_t *peer) {
    if (!turn || !peer || turn->failed)
        return RTC_ERR_INVALID;
    if (turn->peer_set && turn_same_addr(peer, &turn->peer))
        return RTC_OK;
    if (turn->method && turn->method != STUN_METHOD_ALLOCATE)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&turn->mutex);
    turn->peer = *peer;
    turn->peer_set = true;
    turn->bound = false;
    rtc_mutex_unlock(&turn->mutex);
    if (turn->client.allocated) {
        if (turn_build(turn, STUN_METHOD_CREATE_PERM, 0) != RTC_OK)
            return RTC_ERR_GENERIC;
        return turn_send_request(turn);
    }
    return RTC_OK;
}

int rtc_turn_runtime_send(rtc_turn_runtime_t *turn, const uint8_t *data, size_t len,
                          const rtc_addr_t *peer) {
    if (!turn || !data || !peer)
        return RTC_ERR_INVALID;
    uint8_t frame[RTC_PACKET_IO_BUF_SIZE];
    size_t size;
    rtc_mutex_lock(&turn->mutex);
    int rc = RTC_ERR_INVALID;
    if (turn->bound && !turn->failed && turn_same_addr(peer, &turn->peer) &&
        rtc_time_ms() < atomic_load(&turn->allocation_expires_ms)) {
        rc = rtc_turn_build_channel_data(frame, sizeof(frame), TURN_CHANNEL_MIN, data, len, &size);
        if (rc == RTC_OK)
            rc = rtc_packet_io_send(&turn->io, frame, size, &turn->client.server_addr);
    }
    rtc_mutex_unlock(&turn->mutex);
    return rc;
}

void rtc_turn_runtime_destroy(rtc_turn_runtime_t *turn) {
    if (!turn)
        return;
    if (turn->timer != RTC_WORKER_TIMER_INVALID)
        rtc_worker_cancel_timer(turn->worker, turn->timer);
    if (turn->client.allocated && turn_build(turn, STUN_METHOD_REFRESH, 0) == RTC_OK)
        (void)rtc_packet_io_send(&turn->io, turn->request.buf, turn->request.buf_len,
                                 &turn->client.server_addr);
    rtc_worker_remove_io(turn->worker, rtc_packet_io_get_socket(&turn->io));
    rtc_packet_io_close(&turn->io);
    rtc_mutex_destroy(&turn->mutex);
    free(turn);
}
