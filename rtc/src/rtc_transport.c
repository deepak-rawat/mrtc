/*
 * rtc_transport.c - Logical endpoint transport skeleton.
 */
#include "rtc_transport_internal.h"

#include "rtc_listener_internal.h"
#include "rtc/rtc_u32_map.h"
#include "rtc_dtls.h"
#include "rtc_producer_internal.h"
#include "rtc_router_internal.h"
#include "rtc_rtp.h"
#include "rtc_sdp.h"
#include "rtc_srtp.h"
#include "rtc_stun.h"
#include "rtc_worker_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rtc_transport {
    rtc_router_t *router;
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    char ice_ufrag[ICE_UFRAG_LEN];
    char ice_pwd[ICE_PWD_LEN];
    char remote_ufrag[ICE_UFRAG_LEN];
    char remote_pwd[ICE_PWD_LEN];
    rtc_ice_mode_t ice_mode;
    bool enable_sctp;
    bool enable_twcc;
    uint32_t initial_outgoing_bitrate_bps;
    rtc_dtls_transport_t dtls;
    rtc_srtp_ctx_t srtp_send;
    rtc_srtp_ctx_t srtp_recv;
    rtc_u32_map_t producers_by_ssrc;
    rtc_mutex_t producer_mutex;
    bool producer_mutex_ready;
    rtc_transport_rtp_fn on_rtp;
    void *on_rtp_user;
    rtc_transport_rtcp_fn on_rtcp;
    void *on_rtcp_user;
    rtc_transport_data_fn on_data;
    void *on_data_user;
    uint8_t *app_buf;
    size_t app_buf_cap;
    rtc_addr_t selected_remote;
    bool selected_remote_valid;
    rtc_addr_t remote_candidate;
    bool remote_ice_valid;
    bool remote_candidate_valid;
    uint8_t ice_txn_id[STUN_TXN_ID_SIZE];
    bool ice_txn_registered;
    rtc_worker_timer_t ice_timer;
    int ice_check_count;
    bool srtp_ready;
    rtc_worker_timer_t dtls_timer;
    _Atomic bool closed;
    _Atomic uint64_t packets_received;
    _Atomic uint64_t bytes_received;
    _Atomic uint64_t dtls_packets_received;
};

static void transport_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                const rtc_addr_t *from, void *user);

static rtc_transport_dtls_state_t transport_dtls_state(rtc_dtls_state_t state) {
    switch (state) {
        case RTC_DTLS_STATE_NEW:
            return RTC_TRANSPORT_DTLS_NEW;
        case RTC_DTLS_STATE_CONNECTING:
            return RTC_TRANSPORT_DTLS_CONNECTING;
        case RTC_DTLS_STATE_CONNECTED:
            return RTC_TRANSPORT_DTLS_CONNECTED;
        case RTC_DTLS_STATE_FAILED:
            return RTC_TRANSPORT_DTLS_FAILED;
        case RTC_DTLS_STATE_CLOSED:
            return RTC_TRANSPORT_DTLS_CLOSED;
        default:
            return RTC_TRANSPORT_DTLS_FAILED;
    }
}

static rtc_transport_dtls_role_t transport_public_dtls_role(rtc_dtls_role_t role) {
    return role == RTC_DTLS_ROLE_CLIENT ? RTC_TRANSPORT_DTLS_ROLE_CLIENT
                                        : RTC_TRANSPORT_DTLS_ROLE_SERVER;
}

static int transport_dtls_send(const uint8_t *data, size_t len, void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    return rtc_transport_send_raw(transport, data, len);
}

static void transport_ice_timer(void *user);

static void transport_cancel_ice_timer(rtc_transport_t *transport) {
    if (transport->worker && transport->ice_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(transport->worker, transport->ice_timer);
        transport->ice_timer = RTC_WORKER_TIMER_INVALID;
    }
}

static void transport_unregister_ice_txn(rtc_transport_t *transport) {
    if (transport->ice_txn_registered) {
        rtc_listener_unregister_stun_txn(transport->listener, transport->ice_txn_id);
        transport->ice_txn_registered = false;
    }
}

static int transport_send_ice_check(rtc_transport_t *transport) {
    if (!transport->remote_ice_valid || !transport->remote_candidate_valid)
        return RTC_ERR_INVALID;

    char username[RTC_ICE_UFRAG_MAX * 2 + 2];
    snprintf(username, sizeof(username), "%s:%s", transport->remote_ufrag, transport->ice_ufrag);

    rtc_stun_msg_t req;
    int rc = rtc_stun_build_binding_request(&req, username, transport->remote_pwd, 0x7E0000FF, true,
                                            0x123456789ABCDEF0ULL, true);
    if (rc != RTC_OK)
        return rc;

    transport_unregister_ice_txn(transport);
    memcpy(transport->ice_txn_id, req.txn_id, STUN_TXN_ID_SIZE);
    rc = rtc_listener_register_stun_txn(transport->listener, transport->ice_txn_id,
                                        transport_on_packet, transport);
    if (rc != RTC_OK)
        return rc;
    transport->ice_txn_registered = true;

    rc = rtc_listener_send_to(transport->listener, req.buf, req.buf_len,
                              &transport->remote_candidate);
    if (rc == RTC_OK)
        transport->ice_check_count++;
    return rc;
}

static void transport_arm_ice_timer(rtc_transport_t *transport) {
    if (!transport->worker || transport->selected_remote_valid ||
        atomic_load_explicit(&transport->closed, memory_order_acquire)) {
        return;
    }
    if (transport->ice_timer != RTC_WORKER_TIMER_INVALID)
        return;
    transport->ice_timer = rtc_worker_add_timer(transport->worker, rtc_time_ms() + 250,
                                                transport_ice_timer, transport);
}

static void transport_ice_timer(void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    transport->ice_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire) ||
        transport->selected_remote_valid) {
        return;
    }
    if (transport->ice_check_count >= 120)
        return;
    if (transport_send_ice_check(transport) == RTC_OK)
        transport_arm_ice_timer(transport);
}

static void transport_dtls_timer(void *user);

static void transport_cancel_dtls_timer(rtc_transport_t *transport) {
    if (transport->worker && transport->dtls_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(transport->worker, transport->dtls_timer);
        transport->dtls_timer = RTC_WORKER_TIMER_INVALID;
    }
}

static void transport_arm_dtls_timer(rtc_transport_t *transport, uint64_t delay_ms) {
    if (!transport->worker || transport->dtls.state != RTC_DTLS_STATE_CONNECTING ||
        atomic_load_explicit(&transport->closed, memory_order_acquire)) {
        return;
    }
    if (transport->dtls_timer != RTC_WORKER_TIMER_INVALID)
        return;
    transport->dtls_timer = rtc_worker_add_timer(transport->worker, rtc_time_ms() + delay_ms,
                                                 transport_dtls_timer, transport);
}

static void transport_dtls_timer(void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    transport->dtls_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire))
        return;
    if (transport->dtls.state != RTC_DTLS_STATE_CONNECTING)
        return;
    (void)rtc_dtls_retransmit(&transport->dtls);
    transport_arm_dtls_timer(transport, 1000);
}

static void transport_try_export_srtp(rtc_transport_t *transport) {
    if (transport->srtp_ready || transport->dtls.state != RTC_DTLS_STATE_CONNECTED)
        return;
    transport_cancel_dtls_timer(transport);
    if (rtc_dtls_export_srtp_keys(&transport->dtls) != RTC_OK) {
        transport->dtls.state = RTC_DTLS_STATE_FAILED;
        return;
    }

    const uint8_t *send_key = transport->dtls.role == RTC_DTLS_ROLE_CLIENT
                                  ? transport->dtls.srtp_client_key
                                  : transport->dtls.srtp_server_key;
    const uint8_t *send_salt = transport->dtls.role == RTC_DTLS_ROLE_CLIENT
                                   ? transport->dtls.srtp_client_salt
                                   : transport->dtls.srtp_server_salt;
    const uint8_t *recv_key = transport->dtls.role == RTC_DTLS_ROLE_CLIENT
                                  ? transport->dtls.srtp_server_key
                                  : transport->dtls.srtp_client_key;
    const uint8_t *recv_salt = transport->dtls.role == RTC_DTLS_ROLE_CLIENT
                                   ? transport->dtls.srtp_server_salt
                                   : transport->dtls.srtp_client_salt;

    int rc = rtc_srtp_init(&transport->srtp_send, send_key, RTC_SRTP_MASTER_KEY_LEN, send_salt,
                           RTC_SRTP_MASTER_SALT_LEN);
    if (rc != RTC_OK) {
        transport->dtls.state = RTC_DTLS_STATE_FAILED;
        return;
    }
    rc = rtc_srtp_init(&transport->srtp_recv, recv_key, RTC_SRTP_MASTER_KEY_LEN, recv_salt,
                       RTC_SRTP_MASTER_SALT_LEN);
    if (rc != RTC_OK) {
        rtc_srtp_close(&transport->srtp_send);
        transport->dtls.state = RTC_DTLS_STATE_FAILED;
        return;
    }
    transport->srtp_ready = true;
}

static void transport_handle_rtp(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport->srtp_ready || len > SRTP_MAX_PACKET)
        return;

    uint8_t buf[SRTP_MAX_PACKET];
    memcpy(buf, data, len);
    size_t pkt_len = len;
    if (rtc_srtp_unprotect(&transport->srtp_recv, buf, &pkt_len) != RTC_OK)
        return;

    rtc_rtp_packet_t pkt;
    if (rtc_rtp_parse(&pkt, buf, pkt_len) != RTC_OK)
        return;

    rtc_mutex_lock(&transport->producer_mutex);
    rtc_producer_t *producer =
        (rtc_producer_t *)rtc_u32_map_get(&transport->producers_by_ssrc, pkt.header.ssrc);
    rtc_mutex_unlock(&transport->producer_mutex);

    if (producer)
        rtc_producer_on_rtp(producer, &pkt);
    if (transport->on_rtp)
        transport->on_rtp(&pkt, transport->on_rtp_user);
}

static void transport_handle_rtcp(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport->srtp_ready || !transport->on_rtcp || len > SRTP_MAX_PACKET)
        return;
    uint8_t buf[SRTP_MAX_PACKET];
    memcpy(buf, data, len);
    size_t pkt_len = len;
    if (rtc_srtp_unprotect_rtcp(&transport->srtp_recv, buf, &pkt_len) != RTC_OK)
        return;
    transport->on_rtcp(buf, pkt_len, transport->on_rtcp_user);
}

static void transport_read_app_data(rtc_transport_t *transport) {
    if (!transport->on_data || transport->dtls.state != RTC_DTLS_STATE_CONNECTED ||
        !transport->app_buf)
        return;
    int app_len;
    while ((app_len = SSL_read(transport->dtls.ssl, transport->app_buf,
                               (int)transport->app_buf_cap)) > 0) {
        transport->on_data(transport->app_buf, (size_t)app_len, transport->on_data_user);
    }
}

static void transport_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                const rtc_addr_t *from, void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire))
        return;
    atomic_fetch_add_explicit(&transport->packets_received, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&transport->bytes_received, (uint64_t)len, memory_order_relaxed);

    if (type != RTC_PKT_STUN)
        goto maybe_dtls;

    rtc_stun_msg_t msg;
    if (rtc_stun_parse(&msg, data, len) != RTC_OK)
        return;

    if (msg.type == STUN_BINDING_RESPONSE) {
        if (!transport->ice_txn_registered ||
            memcmp(msg.txn_id, transport->ice_txn_id, STUN_TXN_ID_SIZE) != 0) {
            return;
        }
        transport_unregister_ice_txn(transport);
        transport_cancel_ice_timer(transport);
        if (!transport->selected_remote_valid) {
            transport->selected_remote = *from;
            transport->selected_remote_valid = true;
            (void)rtc_listener_register_tuple(transport->listener, &transport->selected_remote,
                                              transport_on_packet, transport);
        }
        return;
    }

    if (msg.type != STUN_BINDING_REQUEST)
        return;

    if (!transport->selected_remote_valid) {
        transport->selected_remote = *from;
        transport->selected_remote_valid = true;
        (void)rtc_listener_register_tuple(transport->listener, &transport->selected_remote,
                                          transport_on_packet, transport);
    }

    uint8_t resp[128];
    size_t resp_len = 0;
    if (rtc_stun_build_binding_response(data, len, from, resp, sizeof(resp), &resp_len) == RTC_OK)
        (void)rtc_listener_send_to(transport->listener, resp, resp_len, from);
    return;

maybe_dtls:
    if (type == RTC_PKT_DTLS) {
        atomic_fetch_add_explicit(&transport->dtls_packets_received, 1, memory_order_relaxed);
        if (rtc_dtls_recv(&transport->dtls, data, len) == RTC_OK) {
            transport_arm_dtls_timer(transport, 1000);
            transport_try_export_srtp(transport);
            transport_read_app_data(transport);
        }
    } else if (type == RTC_PKT_RTP) {
        transport_handle_rtp(transport, data, len);
    } else if (type == RTC_PKT_RTCP) {
        transport_handle_rtcp(transport, data, len);
    }
}

static void transport_copy_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->username_fragment, transport->ice_ufrag, sizeof(out->username_fragment));
    memcpy(out->password, transport->ice_pwd, sizeof(out->password));
    out->mode = transport->ice_mode;
}

rtc_transport_t *rtc_transport_create_internal(rtc_router_t *router,
                                               const rtc_transport_config_t *cfg) {
    if (!router || !cfg || !cfg->listener)
        return NULL;

    rtc_transport_t *transport = (rtc_transport_t *)calloc(1, sizeof(*transport));
    if (!transport)
        return NULL;

    transport->router = router;
    transport->worker = rtc_router_worker(router);
    transport->listener = cfg->listener;
    transport->dtls_timer = RTC_WORKER_TIMER_INVALID;
    transport->ice_timer = RTC_WORKER_TIMER_INVALID;
    transport->ice_mode = cfg->ice_mode;
    transport->enable_sctp = cfg->enable_sctp;
    transport->enable_twcc = cfg->enable_twcc;
    transport->initial_outgoing_bitrate_bps = cfg->initial_outgoing_bitrate_bps;
    if (transport->ice_mode != RTC_ICE_MODE_FULL && transport->ice_mode != RTC_ICE_MODE_LITE)
        transport->ice_mode = RTC_ICE_MODE_LITE;

    if (rtc_random_string(transport->ice_ufrag, sizeof(transport->ice_ufrag)) != RTC_OK ||
        rtc_random_string(transport->ice_pwd, sizeof(transport->ice_pwd)) != RTC_OK) {
        free(transport);
        return NULL;
    }

    transport->app_buf_cap = 65536;
    transport->app_buf = (uint8_t *)malloc(transport->app_buf_cap);
    if (!transport->app_buf) {
        free(transport);
        return NULL;
    }

    if (rtc_u32_map_init(&transport->producers_by_ssrc) != RTC_OK ||
        rtc_mutex_init(&transport->producer_mutex) != RTC_OK) {
        rtc_u32_map_free(&transport->producers_by_ssrc);
        free(transport->app_buf);
        free(transport);
        return NULL;
    }
    transport->producer_mutex_ready = true;

    rtc_dtls_role_t dtls_role =
        transport->ice_mode == RTC_ICE_MODE_FULL ? RTC_DTLS_ROLE_CLIENT : RTC_DTLS_ROLE_SERVER;
    if (rtc_dtls_init(&transport->dtls, dtls_role, transport_dtls_send, transport) != RTC_OK) {
        rtc_mutex_destroy(&transport->producer_mutex);
        rtc_u32_map_free(&transport->producers_by_ssrc);
        free(transport->app_buf);
        free(transport);
        return NULL;
    }

    int rc = rtc_listener_register_ufrag(transport->listener, transport->ice_ufrag,
                                         transport_on_packet, transport);
    if (rc != RTC_OK) {
        rtc_dtls_close(&transport->dtls);
        rtc_mutex_destroy(&transport->producer_mutex);
        rtc_u32_map_free(&transport->producers_by_ssrc);
        free(transport->app_buf);
        free(transport);
        return NULL;
    }
    return transport;
}

static rtc_dtls_role_t transport_internal_dtls_role(rtc_transport_dtls_role_t role) {
    return role == RTC_TRANSPORT_DTLS_ROLE_CLIENT ? RTC_DTLS_ROLE_CLIENT : RTC_DTLS_ROLE_SERVER;
}

int rtc_transport_get_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    transport_copy_ice_parameters(transport, out);
    return RTC_OK;
}

int rtc_transport_restart_ice(rtc_transport_t *transport) {
    if (!transport)
        return RTC_ERR_INVALID;
    if (transport->selected_remote_valid)
        return RTC_ERR_INVALID;

    rtc_listener_unregister_ufrag(transport->listener, transport->ice_ufrag);
    if (rtc_random_string(transport->ice_ufrag, sizeof(transport->ice_ufrag)) != RTC_OK ||
        rtc_random_string(transport->ice_pwd, sizeof(transport->ice_pwd)) != RTC_OK) {
        return RTC_ERR_GENERIC;
    }
    return rtc_listener_register_ufrag(transport->listener, transport->ice_ufrag,
                                       transport_on_packet, transport);
}

int rtc_transport_set_remote_ice_parameters(rtc_transport_t *transport,
                                            const rtc_ice_parameters_t *remote) {
    if (!transport || !remote || remote->username_fragment[0] == '\0' ||
        remote->password[0] == '\0')
        return RTC_ERR_INVALID;
    memcpy(transport->remote_ufrag, remote->username_fragment, sizeof(transport->remote_ufrag));
    memcpy(transport->remote_pwd, remote->password, sizeof(transport->remote_pwd));
    transport->remote_ice_valid = true;
    return RTC_OK;
}

int rtc_transport_add_remote_candidate(rtc_transport_t *transport,
                                       const rtc_transport_candidate_t *candidate) {
    if (!transport || !candidate || candidate->address[0] == '\0' || candidate->port == 0)
        return RTC_ERR_INVALID;
    if (strcmp(candidate->protocol, "udp") != 0)
        return RTC_ERR_INVALID;
    int rc =
        rtc_addr_from_string(&transport->remote_candidate, candidate->address, candidate->port);
    if (rc != RTC_OK)
        return rc;
    transport->remote_candidate_valid = true;
    return RTC_OK;
}

int rtc_transport_start_ice(rtc_transport_t *transport) {
    if (!transport)
        return RTC_ERR_INVALID;
    if (transport->ice_mode != RTC_ICE_MODE_FULL)
        return RTC_ERR_INVALID;
    if (transport->selected_remote_valid)
        return RTC_OK;

    int rc = transport_send_ice_check(transport);
    if (rc != RTC_OK)
        return rc;
    transport_arm_ice_timer(transport);
    return RTC_OK;
}

int rtc_transport_start_dtls(rtc_transport_t *transport) {
    if (!transport)
        return RTC_ERR_INVALID;
    if (!transport->selected_remote_valid)
        return RTC_ERR_INVALID;
    if (transport->dtls.state == RTC_DTLS_STATE_CONNECTED)
        return RTC_OK;

    int rc = rtc_dtls_handshake(&transport->dtls);
    if (rc == RTC_OK) {
        transport_arm_dtls_timer(transport, 1000);
        transport_try_export_srtp(transport);
    }
    return rc;
}

int rtc_transport_get_dtls_parameters(rtc_transport_t *transport, rtc_dtls_parameters_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->role = transport_public_dtls_role(transport->dtls.role);
    memcpy(out->fingerprint, rtc_dtls_get_fingerprint(&transport->dtls), sizeof(out->fingerprint));
    return RTC_OK;
}

int rtc_transport_get_stats(rtc_transport_t *transport, rtc_transport_stats_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->closed = atomic_load_explicit(&transport->closed, memory_order_acquire);
    out->ice_mode = transport->ice_mode;
    out->selected_tuple_valid = transport->selected_remote_valid;
    if (transport->selected_remote_valid)
        out->selected_remote = transport->selected_remote;
    out->dtls_state = transport_dtls_state(transport->dtls.state);
    out->srtp_ready = transport->srtp_ready;
    out->packets_received =
        atomic_load_explicit(&transport->packets_received, memory_order_relaxed);
    out->bytes_received = atomic_load_explicit(&transport->bytes_received, memory_order_relaxed);
    out->dtls_packets_received =
        atomic_load_explicit(&transport->dtls_packets_received, memory_order_relaxed);
    return RTC_OK;
}

void rtc_transport_close(rtc_transport_t *transport) {
    if (!transport)
        return;
    bool was_closed = atomic_exchange_explicit(&transport->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    if (transport->listener) {
        rtc_listener_unregister_ufrag(transport->listener, transport->ice_ufrag);
        transport_unregister_ice_txn(transport);
        if (transport->selected_remote_valid)
            rtc_listener_unregister_tuple(transport->listener, &transport->selected_remote);
    }
    transport_cancel_ice_timer(transport);
    transport_cancel_dtls_timer(transport);
    if (transport->srtp_ready) {
        rtc_srtp_close(&transport->srtp_send);
        rtc_srtp_close(&transport->srtp_recv);
        transport->srtp_ready = false;
    }
    rtc_dtls_close(&transport->dtls);
}

void rtc_transport_destroy(rtc_transport_t *transport) {
    if (!transport)
        return;
    rtc_transport_close(transport);
    if (transport->producer_mutex_ready) {
        rtc_mutex_destroy(&transport->producer_mutex);
        transport->producer_mutex_ready = false;
    }
    rtc_u32_map_free(&transport->producers_by_ssrc);
    free(transport->app_buf);
    free(transport);
}

int rtc_transport_set_dtls_role_internal(rtc_transport_t *transport,
                                         rtc_transport_dtls_role_t role) {
    if (!transport)
        return RTC_ERR_INVALID;
    if (transport->dtls.state != RTC_DTLS_STATE_NEW)
        return RTC_ERR_INVALID;

    rtc_dtls_role_t internal_role = transport_internal_dtls_role(role);
    if (transport->dtls.role == internal_role)
        return RTC_OK;

    return rtc_dtls_set_role(&transport->dtls, internal_role);
}

void rtc_transport_on_rtp(rtc_transport_t *transport, rtc_transport_rtp_fn fn, void *user) {
    if (!transport)
        return;
    transport->on_rtp = fn;
    transport->on_rtp_user = user;
}

void rtc_transport_on_rtcp(rtc_transport_t *transport, rtc_transport_rtcp_fn fn, void *user) {
    if (!transport)
        return;
    transport->on_rtcp = fn;
    transport->on_rtcp_user = user;
}

void rtc_transport_on_data(rtc_transport_t *transport, rtc_transport_data_fn fn, void *user) {
    if (!transport)
        return;
    transport->on_data = fn;
    transport->on_data_user = user;
}

int rtc_transport_send_data(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport || !data || transport->dtls.state != RTC_DTLS_STATE_CONNECTED)
        return RTC_ERR_INVALID;
    int written = SSL_write(transport->dtls.ssl, data, (int)len);
    if (written <= 0)
        return RTC_ERR_SSL;

    uint8_t buf[2048];
    int pending;
    while ((pending = BIO_read(transport->dtls.wbio, buf, sizeof(buf))) > 0) {
        int rc = transport_dtls_send(buf, (size_t)pending, transport);
        if (rc != RTC_OK)
            return rc;
    }
    return RTC_OK;
}

int rtc_transport_register_producer(rtc_transport_t *transport, rtc_producer_t *producer,
                                    uint32_t ssrc) {
    if (!transport || !producer || ssrc == 0)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&transport->producer_mutex);
    int rc = rtc_u32_map_set(&transport->producers_by_ssrc, ssrc, producer);
    rtc_mutex_unlock(&transport->producer_mutex);
    return rc;
}

void rtc_transport_unregister_producer(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport || ssrc == 0)
        return;
    rtc_mutex_lock(&transport->producer_mutex);
    rtc_u32_map_remove(&transport->producers_by_ssrc, ssrc);
    rtc_mutex_unlock(&transport->producer_mutex);
}

int rtc_transport_send_raw(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport || !data || len == 0)
        return RTC_ERR_INVALID;
    if (!transport->selected_remote_valid)
        return RTC_ERR_INVALID;
    return rtc_listener_send_to(transport->listener, data, len, &transport->selected_remote);
}

int rtc_transport_send_rtp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap) {
    if (!transport || !buf || !len)
        return RTC_ERR_INVALID;
    if (!transport->srtp_ready || !transport->selected_remote_valid)
        return RTC_ERR_INVALID;

    int rc = rtc_srtp_protect(&transport->srtp_send, buf, len, buf_cap);
    if (rc != RTC_OK)
        return rc;
    return rtc_transport_send_raw(transport, buf, *len);
}

int rtc_transport_send_rtcp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap) {
    if (!transport || !buf || !len)
        return RTC_ERR_INVALID;
    if (!transport->srtp_ready || !transport->selected_remote_valid)
        return RTC_ERR_INVALID;

    int rc = rtc_srtp_protect_rtcp(&transport->srtp_send, buf, len, buf_cap);
    if (rc != RTC_OK)
        return rc;
    return rtc_transport_send_raw(transport, buf, *len);
}