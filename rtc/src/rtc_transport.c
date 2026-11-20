/*
 * Logical endpoint transport skeleton.
 */
#include "rtc_transport_internal.h"
#include "rtc_rtp_pacer.h"

#include "rtc_listener_internal.h"
#include "rtc_dtls.h"
#include "rtc/rtc_rtp.h"
#include "rtc/rtc_sdp.h"
#include "rtc/rtc_rtcp.h"
#include "rtc_rtp_demux.h"
#ifdef MRTC_ENABLE_TWCC
#  include "rtc/rtc_rtcp.h"
#  include "rtc/rtc_rtp_ext.h"
#  include "rtc_twcc_sender.h"
#  include "rtc_twcc_receiver.h"
#  include "rtc_bwe.h"
#endif
#include "rtc_srtp.h"
#include "rtc_stun.h"
#include "rtc_worker_internal.h"
#include "rtc_turn_runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <openssl/rand.h>

#define RTC_TURN_ADDRESS_TIMEOUT_MS 5000
#define RTC_TURN_RETRY_POLL_MS      100
#define LOCAL_SOURCE_MAX            64
#define REMOTE_SOURCE_MAX           256

typedef struct {
    rtc_turn_config_t config;
    char host[TURN_HOST_MAX], username[TURN_USERNAME_MAX], credential[TURN_CREDENTIAL_MAX];
    char *ca_file;
    rtc_addr_t address;
} transport_turn_attempt_t;

static void transport_turn_retry_timer(void *user);
static int transport_restart_failed_turn(rtc_transport_t *transport);

struct rtc_transport {
    _Atomic unsigned refs;
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    char ice_ufrag[RTC_ICE_UFRAG_MAX];
    char ice_pwd[RTC_ICE_PWD_MAX];
    char remote_ufrag[RTC_ICE_UFRAG_MAX];
    char remote_pwd[RTC_ICE_PWD_MAX];
    rtc_ice_mode_t ice_mode;
    bool enable_sctp;
    bool enable_twcc;
    uint32_t initial_outgoing_bitrate_bps;
    rtc_dtls_transport_t dtls;
    rtc_srtp_ctx_t srtp_send;
    rtc_srtp_ctx_t srtp_recv;
    rtc_rtcp_membership_t rtcp_membership; /* guarded by send_mutex */
    char rtcp_cname[33];
    uint64_t rtcp_next_membership_check;
    uint32_t rtcp_target_bps;
    rtc_transport_rtp_fn on_rtp;
    void *on_rtp_user;
    rtc_rtp_demux_t rtp_demux;
    rtc_transport_rtcp_fn on_rtcp;
    void *on_rtcp_user;
    rtc_transport_data_fn on_data;
    void *on_data_user;
    uint8_t *app_buf;
    size_t app_buf_cap;
    rtc_addr_t selected_remote;
    _Atomic bool selected_remote_valid;
    rtc_addr_t remote_candidate;
    rtc_addr_t remote_candidates[ICE_MAX_CANDIDATES];
    int remote_candidate_count;
    unsigned remote_candidate_index;
    uint32_t remote_candidates_deferred;
    uint32_t remote_candidates_ineligible;
    bool remote_ice_valid;
    bool remote_candidate_valid;
    uint8_t ice_txn_id[STUN_TXN_ID_SIZE];
    bool ice_txn_registered;
    bool ice_txn_controlling;
    rtc_worker_timer_t ice_timer;
    int ice_check_count;
    bool controlling;
    uint64_t tie_breaker;
    _Atomic uint64_t consent_last_ms;
    uint32_t consent_timeout_ms;
    bool consent_expired;
    bool ice_restarting;
    rtc_mutex_t send_mutex;
    struct {
        uint32_t ssrc;
        rtc_local_source_fn fn;
        void *user;
        char cname[33];
    } local_sources[LOCAL_SOURCE_MAX];
    struct {
        uint32_t ssrc;
        bool retired;
        bool crypto_used;
    } used_sources[LOCAL_SOURCE_MAX];
    unsigned used_source_count;
    uint32_t remote_sources[REMOTE_SOURCE_MAX];
    unsigned remote_source_count;
    _Atomic bool sources_closed;
    unsigned source_callback_depth;
    bool enable_rtp_pacing;
    rtc_rtp_pacer_t pacer;
    rtc_worker_timer_t pacer_timer;
    uint64_t pacer_last_log_ms;
    uint64_t rtp_retransmissions_sent, rtp_retransmissions_rejected;
    rtc_turn_runtime_t *turn;
    transport_turn_attempt_t turn_attempts[RTC_TURN_MAX_ADDRESSES];
    unsigned turn_address_count, turn_address_attempts;
    bool turn_retrying;
    uint64_t turn_address_deadline;
    rtc_worker_timer_t turn_retry_timer;
    bool receiving_turn;
    _Atomic bool srtp_ready;
    rtc_worker_timer_t dtls_timer;
    _Atomic bool closed;
    _Atomic uint64_t packets_received;
    _Atomic uint64_t bytes_received;
    _Atomic uint64_t dtls_packets_received;
#ifdef MRTC_ENABLE_TWCC
    bool twcc_enabled;
    uint8_t twcc_ext_id;
    uint32_t twcc_local_ssrc;
    uint32_t twcc_remote_ssrc;
    bool twcc_have_packets;
    rtc_twcc_sender_t twcc_sender;
    rtc_twcc_receiver_t twcc_receiver;
    rtc_bwe_t *bwe;
    rtc_worker_timer_t twcc_fb_timer;
    rtc_transport_bitrate_fn on_bitrate;
    void *on_bitrate_user;
#endif
};

void rtc_transport_retain(rtc_transport_t *transport) {
    if (transport)
        atomic_fetch_add_explicit(&transport->refs, 1, memory_order_relaxed);
}

void rtc_transport_release(rtc_transport_t *transport) {
    if (transport && atomic_fetch_sub_explicit(&transport->refs, 1, memory_order_acq_rel) == 1) {
        rtc_rtp_pacer_close(&transport->pacer);
        rtc_mutex_destroy(&transport->send_mutex);
        free(transport->app_buf);
        free(transport);
    }
}

static void transport_expire_members_locked(rtc_transport_t *transport, uint64_t now) {
    if (now < transport->rtcp_next_membership_check)
        return;
    uint32_t target = transport->rtcp_target_bps ? transport->rtcp_target_bps
                                                 : transport->initial_outgoing_bitrate_bps;
    rtc_rtcp_membership_expire(&transport->rtcp_membership, target, now);
    transport->rtcp_next_membership_check = now + RTC_RTCP_RECONSIDER_POLL_MS;
}

static void transport_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                const rtc_addr_t *from, void *user);
#ifdef MRTC_ENABLE_TWCC
static void transport_record_twcc_arrival(rtc_transport_t *transport, const rtc_rtp_packet_t *pkt);
static void transport_consume_twcc_feedback(rtc_transport_t *transport, const uint8_t *buf,
                                            size_t len);
#endif

/* Marshal a control/query operation onto the worker loop thread so all
 * per-transport protocol state (ICE, DTLS/SSL, SRTP) is mutated by a
 * single thread. The hot RTP/RTCP send path is the only exception; it
 * relies on the atomic published flags selected_remote_valid / srtp_ready. */
typedef int (*tx_op_fn)(rtc_transport_t *transport, void *arg);

typedef struct {
    tx_op_fn fn;
    rtc_transport_t *transport;
    void *arg;
    int rc;
} tx_op_call_t;

static void tx_op_trampoline(void *user) {
    tx_op_call_t *call = (tx_op_call_t *)user;
    call->rc = call->fn(call->transport, call->arg);
}

static int tx_op_invoke(rtc_transport_t *transport, tx_op_fn fn, void *arg) {
    tx_op_call_t call = {fn, transport, arg, RTC_OK};
    rtc_worker_invoke(transport->worker, tx_op_trampoline, &call);
    return call.rc;
}

static bool source_retired_locked(rtc_transport_t *t, uint32_t ssrc) {
    for (unsigned i = 0; i < t->used_source_count; i++)
        if (t->used_sources[i].ssrc == ssrc)
            return t->used_sources[i].retired;
    return false;
}

static int source_reserve_locked(rtc_transport_t *t, uint32_t ssrc) {
    if (!ssrc)
        return RTC_ERR_INVALID;
    for (unsigned i = 0; i < t->used_source_count; i++)
        if (t->used_sources[i].ssrc == ssrc)
            return t->used_sources[i].retired ? RTC_ERR_INVALID : RTC_OK;
    if (t->used_source_count == LOCAL_SOURCE_MAX)
        return RTC_ERR_NOMEM;
    t->used_sources[t->used_source_count++].ssrc = ssrc;
    return RTC_OK;
}

static int source_random_locked(rtc_transport_t *t, uint32_t *out) {
    if (t->used_source_count == LOCAL_SOURCE_MAX)
        return RTC_ERR_NOMEM;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        uint32_t ssrc;
        if (rtc_random_bytes((uint8_t *)&ssrc, sizeof(ssrc)) != RTC_OK)
            return RTC_ERR_GENERIC;
        if (!ssrc)
            continue;
        bool used = false;
        for (unsigned i = 0; i < t->used_source_count; i++)
            used |= t->used_sources[i].ssrc == ssrc;
        for (unsigned i = 0; i < t->remote_source_count; i++)
            used |= t->remote_sources[i] == ssrc;
        if (atomic_load_explicit(&t->srtp_ready, memory_order_acquire))
            used |=
                rtc_srtp_ssrc_used(&t->srtp_send, ssrc) || rtc_srtp_ssrc_used(&t->srtp_recv, ssrc);
        if (!used) {
            *out = ssrc;
            return source_reserve_locked(t, ssrc);
        }
    }
    return RTC_ERR_NOMEM;
}

int rtc_transport_new_ssrc(rtc_transport_t *t, uint32_t *ssrc) {
    if (!t || !ssrc)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&t->send_mutex);
    int rc = atomic_load(&t->closed) ? RTC_ERR_INVALID : source_random_locked(t, ssrc);
    rtc_mutex_unlock(&t->send_mutex);
    return rc;
}

typedef struct {
    uint32_t ssrc;
    rtc_local_source_fn fn;
    void *user;
    const char *cname;
} source_registration_t;

static int source_register_impl(rtc_transport_t *t, void *arg) {
    source_registration_t *a = arg;
    rtc_mutex_lock(&t->send_mutex);
    int rc = RTC_ERR_NOMEM;
    unsigned slot = LOCAL_SOURCE_MAX;
    for (unsigned i = 0; i < LOCAL_SOURCE_MAX; i++) {
        if (t->local_sources[i].ssrc == a->ssrc || t->local_sources[i].user == a->user) {
            rc = RTC_ERR_INVALID;
            goto done;
        }
        if (!t->local_sources[i].ssrc)
            slot = i;
    }
    for (unsigned i = 0; i < t->used_source_count; i++)
        if (t->used_sources[i].ssrc == a->ssrc && t->used_sources[i].crypto_used) {
            rc = RTC_ERR_INVALID;
            goto done;
        }
    if (slot < LOCAL_SOURCE_MAX && !atomic_load(&t->closed)) {
        rc = source_reserve_locked(t, a->ssrc);
        if (rc == RTC_OK) {
            t->local_sources[slot].ssrc = a->ssrc;
            t->local_sources[slot].fn = a->fn;
            t->local_sources[slot].user = a->user;
            snprintf(t->local_sources[slot].cname, sizeof(t->local_sources[slot].cname), "%s",
                     a->cname && a->cname[0] ? a->cname : t->rtcp_cname);
        }
    }
done:
    rtc_mutex_unlock(&t->send_mutex);
    return rc;
}

int rtc_transport_register_source(rtc_transport_t *t, uint32_t ssrc, rtc_local_source_fn fn,
                                  void *user, const char *cname) {
    if (!t || !ssrc || !fn || !user || atomic_load(&t->closed))
        return RTC_ERR_INVALID;
    source_registration_t args = {ssrc, fn, user, cname};
    return tx_op_invoke(t, source_register_impl, &args);
}

#ifdef MRTC_ENABLE_TWCC
static int transport_twcc_rotate(uint32_t previous, uint32_t current, void *user) {
    (void)previous;
    rtc_transport_t *transport = user;
    transport->twcc_local_ssrc = current;
    return RTC_OK;
}
#endif

static int source_unregister_impl(rtc_transport_t *t, void *user) {
    rtc_mutex_lock(&t->send_mutex);
    int rc = RTC_OK;
    for (unsigned i = 0; i < LOCAL_SOURCE_MAX; i++)
        if (t->local_sources[i].user == user) {
            uint32_t ssrc = t->local_sources[i].ssrc;
            for (unsigned j = 0; j < t->used_source_count; j++)
                if (t->used_sources[j].ssrc == ssrc)
                    t->used_sources[j].retired = true;
            memset(&t->local_sources[i], 0, sizeof(t->local_sources[i]));
#ifdef MRTC_ENABLE_TWCC
            if (t->twcc_enabled && t->twcc_local_ssrc == ssrc && user != t &&
                !atomic_load(&t->closed)) {
                uint32_t reporter;
                rc = source_random_locked(t, &reporter);
                if (rc == RTC_OK) {
                    t->twcc_local_ssrc = reporter;
                    t->local_sources[i].ssrc = reporter;
                    t->local_sources[i].fn = transport_twcc_rotate;
                    t->local_sources[i].user = t;
                    memcpy(t->local_sources[i].cname, t->rtcp_cname, sizeof(t->rtcp_cname));
                }
            }
#endif
        }
    rtc_mutex_unlock(&t->send_mutex);
    return rc;
}

void rtc_transport_unregister_source(rtc_transport_t *t, void *user) {
    /* A retained send stream may outlive the closed transport's worker. The
     * worker publishes this only after all owner callbacks have drained. */
    if (!t || atomic_load_explicit(&t->sources_closed, memory_order_acquire))
        return;
    if (t && tx_op_invoke(t, source_unregister_impl, user) != RTC_OK) {
        RTC_LOG_ERR("Local reporting source capacity exhausted");
        rtc_transport_close(t);
    }
}

static int source_reserve_remote_locked(rtc_transport_t *t, uint32_t ssrc) {
    if (!ssrc)
        return RTC_OK;
    for (unsigned i = 0; i < t->remote_source_count; i++)
        if (t->remote_sources[i] == ssrc)
            return RTC_OK;
    if (t->remote_source_count == REMOTE_SOURCE_MAX)
        return RTC_ERR_NOMEM;
    t->remote_sources[t->remote_source_count++] = ssrc;
    return RTC_OK;
}

int rtc_transport_reserve_remote_ssrc(rtc_transport_t *t, uint32_t ssrc) {
    if (!t)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&t->send_mutex);
    int rc = atomic_load(&t->closed) ? RTC_ERR_INVALID : source_reserve_remote_locked(t, ssrc);
    rtc_mutex_unlock(&t->send_mutex);
    return rc;
}

/* Authenticated packet sources, never report-block targets. Crypto entries are
 * deliberately never removed: an old SSRC must not start another nonce epoch. */
static int transport_observe_source(rtc_transport_t *t, uint32_t ssrc, bool departing) {
    if (!ssrc)
        return RTC_OK;
    rtc_mutex_lock(&t->send_mutex);
    int rc = source_reserve_remote_locked(t, ssrc);
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&t->send_mutex);
        return rc;
    }
    unsigned slot = LOCAL_SOURCE_MAX;
    for (unsigned i = 0; !departing && i < LOCAL_SOURCE_MAX; i++)
        if (t->local_sources[i].ssrc == ssrc)
            slot = i;
    if (slot == LOCAL_SOURCE_MAX) {
        int result = RTC_OK;
        for (unsigned i = 0; !departing && i < t->used_source_count; i++)
            if (t->used_sources[i].ssrc == ssrc && t->used_sources[i].crypto_used &&
                !t->used_sources[i].retired)
                result = RTC_ERR_INVALID;
        rtc_mutex_unlock(&t->send_mutex);
        return result;
    }
    uint32_t replacement;
    rc = source_random_locked(t, &replacement);
    if (rc != RTC_OK) {
        rtc_mutex_unlock(&t->send_mutex);
        return rc;
    }
    rtc_local_source_fn fn = t->local_sources[slot].fn;
    void *user = t->local_sources[slot].user;
    char cname[33];
    memcpy(cname, t->local_sources[slot].cname, sizeof(cname));
    for (unsigned i = 0; i < t->used_source_count; i++)
        if (t->used_sources[i].ssrc == ssrc)
            t->used_sources[i].retired = true;
    t->local_sources[slot].ssrc = replacement;
#ifdef MRTC_ENABLE_TWCC
    if (t->twcc_local_ssrc == ssrc)
        t->twcc_local_ssrc = replacement;
#endif
    rtc_mutex_unlock(&t->send_mutex);
    /* No owner callback or pacing helper may run under send_mutex. */
    rtc_transport_forget_rtp(t, ssrc);
    rc = rtc_transport_send_bye_named(t, ssrc, cname);
    if (rc != RTC_OK)
        return rc;
    t->source_callback_depth++;
    rc = fn(ssrc, replacement, user);
    t->source_callback_depth--;
    if (!t->source_callback_depth && atomic_load(&t->closed))
        atomic_store_explicit(&t->sources_closed, true, memory_order_release);
    return rc == RTC_OK && atomic_load(&t->closed) ? RTC_ERR_INVALID : rc;
}

static uint32_t source_read(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static bool source_departing(const uint8_t *buf, size_t len, uint32_t ssrc) {
    for (size_t off = 0; off + 4 <= len;) {
        size_t size = ((size_t)buf[off + 2] * 256 + buf[off + 3] + 1) * 4;
        if (size > len - off)
            break;
        if (buf[off + 1] == RTCP_PT_BYE)
            for (unsigned i = 0; i < (buf[off] & 31) && 8 + i * 4 <= size; i++)
                if (source_read(buf + off + 4 + i * 4) == ssrc)
                    return true;
        off += size;
    }
    return false;
}

static int transport_observe_rtcp(rtc_transport_t *t, const uint8_t *buf, size_t len,
                                  bool record_only) {
    for (size_t off = 0; off + 4 <= len;) {
        size_t size = ((size_t)buf[off + 2] * 256 + buf[off + 3] + 1) * 4;
        uint8_t pt = buf[off + 1];
        if (pt == RTCP_PT_SDES) {
            size_t pos = off + 4;
            for (unsigned i = 0; i < (buf[off] & 31) && pos + 4 <= off + size; i++) {
                uint32_t ssrc = source_read(buf + pos);
                int rc = transport_observe_source(t, ssrc,
                                                  record_only || source_departing(buf, len, ssrc));
                if (rc != RTC_OK)
                    return rc;
                pos += 4;
                while (pos < off + size && buf[pos]) {
                    if (pos + 2 > off + size || buf[pos + 1] > off + size - pos - 2)
                        return RTC_ERR_INVALID;
                    pos += 2 + buf[pos + 1];
                }
                pos = (pos + 4) & ~(size_t)3;
            }
        } else if (pt == RTCP_PT_BYE) {
            for (unsigned i = 0; i < (buf[off] & 31); i++) {
                int rc = transport_observe_source(t, source_read(buf + off + 4 + i * 4), true);
                if (rc != RTC_OK)
                    return rc;
            }
        } else if (size >= 8 && (pt == RTCP_PT_SR || pt == RTCP_PT_RR || pt == RTCP_PT_RTPFB ||
                                 pt == RTCP_PT_PSFB)) {
            uint32_t ssrc = source_read(buf + off + 4);
            int rc =
                transport_observe_source(t, ssrc, record_only || source_departing(buf, len, ssrc));
            if (rc != RTC_OK)
                return rc;
        }
        off += size;
    }
    return RTC_OK;
}

int rtc_transport_invoke(rtc_transport_t *transport, rtc_transport_task_fn fn, void *user) {
    if (!transport || !fn)
        return RTC_ERR_INVALID;
    return rtc_worker_invoke(transport->worker, fn, user);
}

bool rtc_transport_on_worker_thread(rtc_transport_t *transport) {
    return transport && rtc_worker_on_loop_thread(transport->worker);
}

/* Define a `void rtc_transport_<func>(transport, fn, user)` callback setter that
 * marshals the (fn, user) store onto the worker thread. The store itself is the
 * same shape for every callback, so generate the args struct + impl + wrapper
 * instead of repeating them. */
#define TX_DEFINE_CB_SETTER(func, fntype, fn_field, user_field)                    \
    typedef struct {                                                               \
        fntype fn;                                                                 \
        void *user;                                                                \
    } tx_##func##_args_t;                                                          \
    static int tx_##func##_impl(rtc_transport_t *transport, void *arg) {           \
        tx_##func##_args_t *a = (tx_##func##_args_t *)arg;                         \
        transport->fn_field = a->fn;                                               \
        transport->user_field = a->user;                                           \
        return RTC_OK;                                                             \
    }                                                                              \
    void rtc_transport_##func(rtc_transport_t *transport, fntype fn, void *user) { \
        if (!transport)                                                            \
            return;                                                                \
        tx_##func##_args_t args = {fn, user};                                      \
        tx_op_invoke(transport, tx_##func##_impl, &args);                          \
    }

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

static int transport_send_to(rtc_transport_t *transport, const uint8_t *data, size_t len,
                             const rtc_addr_t *to) {
    if (transport->turn)
        return rtc_turn_runtime_send(transport->turn, data, len, to);
    if (transport->turn_address_count)
        return RTC_ERR_INVALID;
    return rtc_listener_send_to(transport->listener, data, len, to);
}

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
    if (atomic_load(&transport->selected_remote_valid) && !transport->ice_restarting) {
        transport->remote_candidate = transport->selected_remote;
    } else if (transport->remote_candidate_count) {
        transport->remote_candidate =
            transport->remote_candidates[transport->remote_candidate_index++ %
                                         (unsigned)transport->remote_candidate_count];
    }
    if (transport->turn &&
        rtc_turn_runtime_set_peer(transport->turn, &transport->remote_candidate) != RTC_OK)
        return RTC_ERR_INVALID;

    char username[RTC_ICE_UFRAG_MAX * 2 + 2];
    snprintf(username, sizeof(username), "%s:%s", transport->remote_ufrag, transport->ice_ufrag);

    rtc_stun_msg_t req;
    int rc = rtc_stun_build_binding_request(&req, username, transport->remote_pwd, 0x7E0000FF,
                                            transport->controlling, transport->tie_breaker,
                                            transport->controlling);
    if (rc != RTC_OK)
        return rc;

    transport_unregister_ice_txn(transport);
    memcpy(transport->ice_txn_id, req.txn_id, STUN_TXN_ID_SIZE);
    rc = rtc_listener_register_stun_txn(transport->listener, transport->ice_txn_id,
                                        transport_on_packet, transport);
    if (rc != RTC_OK)
        return rc;
    transport->ice_txn_registered = true;
    transport->ice_txn_controlling = transport->controlling;

    rc = transport_send_to(transport, req.buf, req.buf_len, &transport->remote_candidate);
    if (rc == RTC_OK)
        transport->ice_check_count++;
    return rc;
}

static void transport_arm_ice_timer(rtc_transport_t *transport) {
    if (!transport->worker || transport->consent_expired ||
        atomic_load_explicit(&transport->closed, memory_order_acquire)) {
        return;
    }
    if (transport->ice_timer != RTC_WORKER_TIMER_INVALID)
        return;
    uint64_t delay = atomic_load(&transport->selected_remote_valid) ? 5000 : 250;
    if (delay > transport->consent_timeout_ms / 3)
        delay = transport->consent_timeout_ms / 3;
    if (atomic_load(&transport->selected_remote_valid)) {
        uint64_t now = rtc_time_ms();
        uint64_t deadline =
            atomic_load(&transport->consent_last_ms) + transport->consent_timeout_ms;
        if (deadline <= now)
            delay = 1;
        else if (delay > deadline - now)
            delay = deadline - now;
    }
    transport->ice_timer = rtc_worker_add_timer(transport->worker, rtc_time_ms() + delay,
                                                transport_ice_timer, transport);
}

static void transport_ice_timer(void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    transport->ice_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire)) {
        return;
    }
    if (atomic_load(&transport->selected_remote_valid) &&
        rtc_time_ms() - transport->consent_last_ms >= transport->consent_timeout_ms) {
        rtc_mutex_lock(&transport->send_mutex);
        atomic_store(&transport->selected_remote_valid, false);
        rtc_mutex_unlock(&transport->send_mutex);
        if (!transport->turn)
            rtc_listener_unregister_tuple(transport->listener, &transport->selected_remote);
        transport->consent_expired = true;
        transport_unregister_ice_txn(transport);
        return;
    }
    if (!atomic_load(&transport->selected_remote_valid) && transport->ice_check_count >= 120)
        return;
    (void)transport_send_ice_check(transport);
    transport_arm_ice_timer(transport);
}

static void transport_dtls_timer(void *user);

static void transport_cancel_dtls_timer(rtc_transport_t *transport) {
    if (transport->worker && transport->dtls_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(transport->worker, transport->dtls_timer);
        transport->dtls_timer = RTC_WORKER_TIMER_INVALID;
        transport->turn_retry_timer = RTC_WORKER_TIMER_INVALID;
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
    if (atomic_load_explicit(&transport->srtp_ready, memory_order_acquire) ||
        transport->dtls.state != RTC_DTLS_STATE_CONNECTED)
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

    size_t key_len = transport->dtls.srtp_key_len;
    size_t salt_len = transport->dtls.srtp_salt_len;
    rtc_srtp_profile_t profile;
    if (!transport->dtls.srtp_aead_gcm)
        profile = RTC_SRTP_PROFILE_AES128_CM_SHA1_80;
    else if (key_len >= 32)
        profile = RTC_SRTP_PROFILE_AEAD_AES_256_GCM;
    else
        profile = RTC_SRTP_PROFILE_AEAD_AES_128_GCM;

    int rc = rtc_srtp_init_profile(&transport->srtp_send, profile, send_key, key_len, send_salt,
                                   salt_len);
    if (rc != RTC_OK) {
        transport->dtls.state = RTC_DTLS_STATE_FAILED;
        return;
    }
    rc = rtc_srtp_init_profile(&transport->srtp_recv, profile, recv_key, key_len, recv_salt,
                               salt_len);
    if (rc != RTC_OK) {
        rtc_srtp_close(&transport->srtp_send);
        transport->dtls.state = RTC_DTLS_STATE_FAILED;
        return;
    }
    atomic_store_explicit(&transport->srtp_ready, true, memory_order_release);
}

static void transport_handle_rtp(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!atomic_load_explicit(&transport->srtp_ready, memory_order_acquire) ||
        len > SRTP_MAX_PACKET)
        return;

    uint8_t buf[SRTP_MAX_PACKET];
    memcpy(buf, data, len);
    size_t pkt_len = len;
    int rc = rtc_srtp_unprotect(&transport->srtp_recv, buf, &pkt_len);
    if (rc != RTC_OK) {
        if (rc == RTC_ERR_NOMEM)
            rtc_transport_close(transport);
        return;
    }

    rtc_rtp_packet_t pkt;
    if (rtc_rtp_parse(&pkt, buf, pkt_len) != RTC_OK)
        return;
    rc = transport_observe_source(transport, pkt.header.ssrc, true);
    for (unsigned i = 0; rc == RTC_OK && i < pkt.header.csrc_count; i++)
        rc = transport_observe_source(transport, source_read(buf + RTP_HEADER_SIZE + i * 4), true);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("RTP source history exhausted: %d", rc);
        rtc_transport_close(transport);
        return;
    }
    if (transport_observe_source(transport, pkt.header.ssrc, false) != RTC_OK) {
        RTC_LOG_ERR("Local SSRC recovery exhausted or failed");
        rtc_transport_close(transport);
        return;
    }
    for (unsigned i = 0; i < pkt.header.csrc_count; i++)
        if (transport_observe_source(transport, source_read(buf + RTP_HEADER_SIZE + i * 4),
                                     false) != RTC_OK) {
            rtc_transport_close(transport);
            return;
        }

    rtc_mutex_lock(&transport->send_mutex);
    uint64_t now = rtc_time_ms();
    transport_expire_members_locked(transport, now);
    int membership_rc =
        rtc_rtcp_membership_touch(&transport->rtcp_membership, pkt.header.ssrc, false, true, now);
    for (unsigned i = 0; membership_rc == RTC_OK && i < pkt.header.csrc_count; i++) {
        uint32_t csrc;
        memcpy(&csrc, buf + RTP_HEADER_SIZE + i * 4, sizeof(csrc));
        membership_rc =
            rtc_rtcp_membership_touch(&transport->rtcp_membership, ntohl(csrc), false, false, now);
    }
    rtc_mutex_unlock(&transport->send_mutex);
    if (membership_rc != RTC_OK) {
        rtc_transport_close(transport);
        return;
    }

    bool delivered = false;
    if (transport->rtp_demux.ready)
        delivered = rtc_rtp_demux_dispatch(&transport->rtp_demux, &pkt);
    if (!delivered && transport->on_rtp)
        transport->on_rtp(&pkt, transport->on_rtp_user);

#ifdef MRTC_ENABLE_TWCC
    transport_record_twcc_arrival(transport, &pkt);
#endif
}

static void transport_handle_rtcp(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!atomic_load_explicit(&transport->srtp_ready, memory_order_acquire) ||
        len > SRTP_MAX_PACKET)
        return;
    uint8_t buf[SRTP_MAX_PACKET];
    memcpy(buf, data, len);
    size_t pkt_len = len;
    int rc = rtc_srtp_unprotect_rtcp(&transport->srtp_recv, buf, &pkt_len);
    if (rc != RTC_OK) {
        if (rc == RTC_ERR_NOMEM)
            rtc_transport_close(transport);
        return;
    }
    if (rtc_rtcp_validate_compound(buf, pkt_len) != RTC_OK)
        return;
    rc = transport_observe_rtcp(transport, buf, pkt_len, true);
    if (rc == RTC_OK)
        rc = transport_observe_rtcp(transport, buf, pkt_len, false);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("Local RTCP source recovery failed: %d", rc);
        rtc_transport_close(transport);
        return;
    }
    rtc_mutex_lock(&transport->send_mutex);
    transport_expire_members_locked(transport, rtc_time_ms());
    rc = rtc_rtcp_membership_on_rtcp(&transport->rtcp_membership, buf, pkt_len, false, len + 48,
                                     rtc_time_ms());
    rtc_mutex_unlock(&transport->send_mutex);
    if (rc != RTC_OK) {
        if (rc == RTC_ERR_NOMEM)
            rtc_transport_close(transport);
        return;
    }
#ifdef MRTC_ENABLE_TWCC
    transport_consume_twcc_feedback(transport, buf, pkt_len);
#endif
    if (transport->on_rtcp)
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

static bool transport_same_addr(const rtc_addr_t *a, const rtc_addr_t *b) {
    char ai[64], bi[64];
    uint16_t ap, bp;
    return rtc_addr_to_string(a, ai, sizeof(ai), &ap) == RTC_OK &&
           rtc_addr_to_string(b, bi, sizeof(bi), &bp) == RTC_OK && ap == bp && strcmp(ai, bi) == 0;
}

static void transport_select_remote(rtc_transport_t *transport, const rtc_addr_t *from) {
    bool selected = atomic_load(&transport->selected_remote_valid);
    if (selected && transport_same_addr(&transport->selected_remote, from)) {
        transport->ice_restarting = false;
        return;
    }
    if (selected && !transport->turn)
        rtc_listener_unregister_tuple(transport->listener, &transport->selected_remote);
    rtc_mutex_lock(&transport->send_mutex);
    transport->selected_remote = *from;
    atomic_store_explicit(&transport->selected_remote_valid, true, memory_order_release);
    rtc_mutex_unlock(&transport->send_mutex);
    if (!transport->turn)
        (void)rtc_listener_register_tuple(transport->listener, from, transport_on_packet,
                                          transport);
    transport->ice_restarting = false;
    transport->consent_last_ms = rtc_time_ms();
    transport_arm_ice_timer(transport);
}

static void transport_binding_reply(rtc_transport_t *transport, const rtc_stun_msg_t *request,
                                    const rtc_addr_t *from, bool conflict) {
    rtc_stun_msg_t reply;
    memset(&reply, 0, sizeof(reply));
    if (conflict) {
        if (rtc_stun_build_request(&reply, STUN_BINDING_ERROR_RESPONSE, NULL, NULL) != RTC_OK)
            return;
        /* RFC 8445 role conflict, authenticated with the recipient's ICE password. */
        const uint8_t error[] = {0, 9, 0, 4, 0, 0, 4, 87};
        memcpy(reply.buf + reply.buf_len, error, sizeof(error));
        reply.buf_len += sizeof(error);
    } else {
        if (rtc_stun_build_binding_response(request->buf, request->buf_len, from, reply.buf,
                                            sizeof(reply.buf), &reply.buf_len) != RTC_OK)
            return;
        reply.type = STUN_BINDING_RESPONSE;
    }
    memcpy(reply.txn_id, request->txn_id, STUN_TXN_ID_SIZE);
    memcpy(reply.buf + 8, request->txn_id, STUN_TXN_ID_SIZE);
    if (rtc_stun_finalize(&reply, transport->ice_pwd) == RTC_OK)
        (void)transport_send_to(transport, reply.buf, reply.buf_len, from);
}

static void transport_handle_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                    const rtc_addr_t *from, void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    if (transport->turn_address_count && !transport->receiving_turn)
        return;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire))
        return;
    atomic_fetch_add_explicit(&transport->packets_received, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&transport->bytes_received, (uint64_t)len, memory_order_relaxed);

    if (type != RTC_PKT_STUN)
        goto maybe_dtls;

    rtc_stun_msg_t msg;
    if (rtc_stun_parse(&msg, data, len) != RTC_OK)
        return;

    if (msg.type == STUN_BINDING_RESPONSE || msg.type == STUN_BINDING_ERROR_RESPONSE) {
        if (!transport->ice_txn_registered ||
            memcmp(msg.txn_id, transport->ice_txn_id, STUN_TXN_ID_SIZE) != 0 ||
            !transport_same_addr(from, &transport->remote_candidate) ||
            rtc_stun_verify_integrity(data, len, transport->remote_pwd) != RTC_OK) {
            return;
        }
        transport_unregister_ice_txn(transport);
        if (msg.type == STUN_BINDING_ERROR_RESPONSE) {
            int code = 0;
            if (rtc_stun_get_error_code(&msg, &code) == RTC_OK && code == 487 &&
                transport->ice_mode == RTC_ICE_MODE_FULL) {
                transport->controlling = !transport->ice_txn_controlling;
                (void)transport_send_ice_check(transport);
            }
            return;
        }
        transport->consent_last_ms = rtc_time_ms();
        if (transport->controlling)
            transport_select_remote(transport, from);
        transport_arm_ice_timer(transport);
        return;
    }

    if (msg.type != STUN_BINDING_REQUEST)
        return;

    if (transport->consent_expired ||
        rtc_stun_verify_integrity(data, len, transport->ice_pwd) != RTC_OK)
        return;
    char expected[RTC_ICE_UFRAG_MAX * 2 + 2];
    snprintf(expected, sizeof(expected), "%s:%s", transport->ice_ufrag, transport->remote_ufrag);
    uint16_t attr_len = 0;
    const uint8_t *username = rtc_stun_find_attr(&msg, STUN_ATTR_USERNAME, &attr_len);
    if (!username || !transport->remote_ice_valid || attr_len != strlen(expected) ||
        memcmp(username, expected, attr_len) != 0)
        return;
    uint16_t role_len = 0;
    const uint8_t *controlling = rtc_stun_find_attr(&msg, STUN_ATTR_ICE_CONTROLLING, &role_len);
    const uint8_t *controlled = rtc_stun_find_attr(&msg, STUN_ATTR_ICE_CONTROLLED, &attr_len);
    if (!controlling && !controlled && atomic_load(&transport->selected_remote_valid) &&
        transport_same_addr(from, &transport->selected_remote)) {
        transport_binding_reply(transport, &msg, from, false);
        return;
    }
    if ((!controlling && !controlled) || (controlling && controlled) ||
        (controlling ? role_len : attr_len) != 8)
        return;
    const uint8_t *role = controlling ? controlling : controlled;
    uint64_t tie = 0;
    for (int i = 0; i < 8; i++)
        tie = (tie << 8) | role[i];
    bool conflict = false;
    if (transport->controlling && controlling) {
        if (transport->tie_breaker >= tie)
            conflict = true;
        else
            transport->controlling = false;
    } else if (!transport->controlling && controlled) {
        if (transport->ice_mode == RTC_ICE_MODE_LITE || transport->tie_breaker < tie)
            conflict = true;
        else
            transport->controlling = true;
    }
    transport_binding_reply(transport, &msg, from, conflict);
    if (conflict)
        return;
    if (!transport->controlling && controlling &&
        rtc_stun_find_attr(&msg, STUN_ATTR_USE_CANDIDATE, NULL)) {
        transport_select_remote(transport, from);
    }
    if (!transport->remote_candidate_valid) {
        transport->remote_candidate = *from;
        transport->remote_candidate_valid = true;
    }
    if (transport->ice_mode == RTC_ICE_MODE_FULL &&
        !atomic_load(&transport->selected_remote_valid)) {
        (void)transport_send_ice_check(transport);
        transport_arm_ice_timer(transport);
    }
    return;

maybe_dtls:
    if (transport->consent_expired || !atomic_load(&transport->selected_remote_valid) ||
        !transport_same_addr(from, &transport->selected_remote))
        return;
    if (type == RTC_PKT_DTLS) {
        if (!transport->dtls.remote_fingerprint_set)
            return;
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

static void transport_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                const rtc_addr_t *from, void *user) {
    rtc_transport_t *transport = user;
    rtc_transport_retain(transport);
    transport_handle_packet(type, data, len, from, user);
    rtc_transport_release(transport);
}

static void transport_copy_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->username_fragment, transport->ice_ufrag, sizeof(out->username_fragment));
    memcpy(out->password, transport->ice_pwd, sizeof(out->password));
    out->mode = transport->ice_mode;
}

rtc_transport_t *rtc_transport_create(rtc_worker_t *worker, const rtc_transport_config_t *cfg) {
    if (!worker || !cfg || !cfg->listener)
        return NULL;

    rtc_transport_t *transport = (rtc_transport_t *)calloc(1, sizeof(*transport));
    if (!transport)
        return NULL;

    transport->worker = worker;
    atomic_init(&transport->refs, 1);
    transport->listener = cfg->listener;
    transport->dtls_timer = RTC_WORKER_TIMER_INVALID;
    transport->ice_timer = RTC_WORKER_TIMER_INVALID;
#ifdef MRTC_ENABLE_TWCC
    transport->twcc_fb_timer = RTC_WORKER_TIMER_INVALID;
#endif
    transport->ice_mode = cfg->ice_mode;
    transport->enable_sctp = cfg->enable_sctp;
    transport->enable_twcc = cfg->enable_twcc;
    transport->initial_outgoing_bitrate_bps = cfg->initial_outgoing_bitrate_bps;
    transport->enable_rtp_pacing = cfg->enable_rtp_pacing;
    transport->pacer.bitrate_bps = cfg->initial_outgoing_bitrate_bps;
    transport->pacer_timer = RTC_WORKER_TIMER_INVALID;
    transport->controlling = cfg->ice_mode == RTC_ICE_MODE_FULL;
    transport->consent_timeout_ms = cfg->consent_timeout_ms ? cfg->consent_timeout_ms : 30000;
    if (transport->consent_timeout_ms < 30)
        transport->consent_timeout_ms = 30;
    if (RAND_bytes((unsigned char *)&transport->tie_breaker, sizeof(transport->tie_breaker)) != 1) {
        free(transport);
        return NULL;
    }
    if (transport->ice_mode != RTC_ICE_MODE_FULL && transport->ice_mode != RTC_ICE_MODE_LITE)
        transport->ice_mode = RTC_ICE_MODE_LITE;

    if (rtc_random_string(transport->ice_ufrag, 8) != RTC_OK ||
        rtc_random_string(transport->ice_pwd, 24) != RTC_OK ||
        rtc_random_string(transport->rtcp_cname, sizeof(transport->rtcp_cname)) != RTC_OK) {
        free(transport);
        return NULL;
    }

    transport->app_buf_cap = 65536;
    transport->app_buf = (uint8_t *)malloc(transport->app_buf_cap);
    if (!transport->app_buf) {
        free(transport);
        return NULL;
    }

    rtc_dtls_role_t dtls_role =
        transport->ice_mode == RTC_ICE_MODE_FULL ? RTC_DTLS_ROLE_CLIENT : RTC_DTLS_ROLE_SERVER;
    if (rtc_dtls_init(&transport->dtls, dtls_role, transport_dtls_send, transport) != RTC_OK) {
        free(transport->app_buf);
        free(transport);
        return NULL;
    }

    if (rtc_mutex_init(&transport->send_mutex) != RTC_OK) {
        rtc_dtls_close(&transport->dtls);
        free(transport->app_buf);
        free(transport);
        return NULL;
    }
    int rc = rtc_listener_register_ufrag(transport->listener, transport->ice_ufrag,
                                         transport_on_packet, transport);
    if (rc != RTC_OK) {
        rtc_mutex_destroy(&transport->send_mutex);
        rtc_dtls_close(&transport->dtls);
        free(transport->app_buf);
        free(transport);
        return NULL;
    }
    return transport;
}

static rtc_dtls_role_t transport_internal_dtls_role(rtc_transport_dtls_role_t role) {
    return role == RTC_TRANSPORT_DTLS_ROLE_CLIENT ? RTC_DTLS_ROLE_CLIENT : RTC_DTLS_ROLE_SERVER;
}

static int tx_get_ice_parameters_impl(rtc_transport_t *transport, void *arg) {
    transport_copy_ice_parameters(transport, (rtc_ice_parameters_t *)arg);
    return RTC_OK;
}

int rtc_transport_get_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_get_ice_parameters_impl, out);
}

static int tx_restart_ice_impl(rtc_transport_t *transport, void *arg) {
    (void)arg;
    if (atomic_load(&transport->closed))
        return RTC_ERR_INVALID;
    char ufrag[RTC_ICE_UFRAG_MAX] = {0}, pwd[RTC_ICE_PWD_MAX] = {0};
    if (rtc_random_string(ufrag, 8) != RTC_OK || rtc_random_string(pwd, 24) != RTC_OK)
        return RTC_ERR_GENERIC;
    int rc =
        rtc_listener_register_ufrag(transport->listener, ufrag, transport_on_packet, transport);
    if (rc != RTC_OK)
        return rc;
    rtc_listener_unregister_ufrag(transport->listener, transport->ice_ufrag);
    memcpy(transport->ice_ufrag, ufrag, sizeof(ufrag));
    memcpy(transport->ice_pwd, pwd, sizeof(pwd));
    transport_cancel_ice_timer(transport);
    transport_unregister_ice_txn(transport);
    transport->ice_check_count = 0;
    transport->consent_expired = false;
    transport->ice_restarting = true;
    transport->consent_last_ms = rtc_time_ms();
    transport->remote_ice_valid = false;
    transport->remote_candidate_valid = false;
    transport->remote_candidate_count = 0;
    transport->remote_candidate_index = 0;
    rc = transport_restart_failed_turn(transport);
    if (rc != RTC_OK)
        return rc;
    transport_arm_ice_timer(transport);
    return RTC_OK;
}

int rtc_transport_restart_ice(rtc_transport_t *transport) {
    if (!transport)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_restart_ice_impl, NULL);
}

static int tx_set_remote_ice_impl(rtc_transport_t *transport, void *arg) {
    const rtc_ice_parameters_t *remote = (const rtc_ice_parameters_t *)arg;
    if (remote->mode == RTC_ICE_MODE_LITE) {
        if (transport->ice_mode == RTC_ICE_MODE_LITE)
            return RTC_ERR_INVALID;
        transport->controlling = true;
    }
    if (transport->remote_ice_valid &&
        (strcmp(transport->remote_ufrag, remote->username_fragment) != 0 ||
         strcmp(transport->remote_pwd, remote->password) != 0)) {
        if (transport->turn_address_count && !transport->turn_retrying &&
            (!transport->turn || rtc_turn_runtime_failed(transport->turn))) {
            int rc = tx_restart_ice_impl(transport, NULL);
            if (rc != RTC_OK)
                return rc;
        }
        transport_unregister_ice_txn(transport);
        transport_cancel_ice_timer(transport);
        transport->ice_restarting = true;
        transport->ice_check_count = 0;
        transport->consent_expired = false;
        transport->consent_last_ms = rtc_time_ms();
        transport->remote_candidate_valid = false;
        transport->remote_candidate_count = 0;
        transport->remote_candidate_index = 0;
        int rc = transport_restart_failed_turn(transport);
        if (rc != RTC_OK)
            return rc;
    }
    memcpy(transport->remote_ufrag, remote->username_fragment, sizeof(transport->remote_ufrag));
    memcpy(transport->remote_pwd, remote->password, sizeof(transport->remote_pwd));
    transport->remote_ice_valid = true;
    if (transport->ice_restarting)
        transport_arm_ice_timer(transport);
    return RTC_OK;
}

int rtc_transport_set_remote_ice_parameters(rtc_transport_t *transport,
                                            const rtc_ice_parameters_t *remote) {
    if (!transport || !remote || remote->username_fragment[0] == '\0' ||
        remote->password[0] == '\0' ||
        !memchr(remote->username_fragment, '\0', sizeof(remote->username_fragment)) ||
        !memchr(remote->password, '\0', sizeof(remote->password)))
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_set_remote_ice_impl, (void *)remote);
}

static int tx_add_candidate_impl(rtc_transport_t *transport, void *arg) {
    const rtc_transport_candidate_t *candidate = (const rtc_transport_candidate_t *)arg;
    bool mdns = rtc_ice_candidate_is_mdns_hostname(candidate->address);
    rtc_addr_t address = {0};
    if ((mdns && candidate->type != RTC_TRANSPORT_CANDIDATE_HOST) ||
        (!mdns && rtc_addr_from_string(&address, candidate->address, candidate->port) != RTC_OK))
        return RTC_ERR_INVALID;
    if (candidate->username_fragment[0] && transport->remote_ice_valid &&
        strcmp(candidate->username_fragment, transport->remote_ufrag)) {
        RTC_LOG_DBG("ICE: ignoring candidate from an older ICE generation");
        return RTC_OK;
    }
    if (strcmp(candidate->protocol, "tcp") == 0) {
        if (transport->remote_candidates_ineligible != UINT32_MAX)
            transport->remote_candidates_ineligible++;
        RTC_LOG_INFO("ICE: TCP candidate is ineligible for this UDP transport");
        return RTC_OK;
    }
    if (mdns) {
        if (transport->remote_candidates_deferred != UINT32_MAX)
            transport->remote_candidates_deferred++;
        /* Do not leak .local names to unicast DNS or block the media worker.
         * A validated incoming check supplies the peer-reflexive address. */
        RTC_LOG_INFO("ICE: mDNS candidate deferred to authenticated peer-reflexive checks");
        return RTC_OK;
    }
    if (transport->turn_address_count) {
        int family = transport
                         ->turn_attempts[transport->turn_address_attempts
                                             ? transport->turn_address_attempts - 1
                                             : 0]
                         .config.relay_family;
        if (address.addr.ss_family != (family == AF_INET6 ? AF_INET6 : AF_INET)) {
            if (transport->remote_candidates_ineligible != UINT32_MAX)
                transport->remote_candidates_ineligible++;
            return RTC_OK;
        }
    }
    for (int i = 0; i < transport->remote_candidate_count; i++) {
        if (transport_same_addr(&address, &transport->remote_candidates[i]))
            return RTC_OK;
    }
    if (transport->remote_candidate_count == ICE_MAX_CANDIDATES)
        return RTC_ERR_NOMEM;
    transport->remote_candidates[transport->remote_candidate_count++] = address;
    if (!transport->remote_candidate_valid)
        transport->remote_candidate = address;
    transport->remote_candidate_valid = true;
    if (transport->turn && transport->remote_candidate_count == 1 &&
        !(transport->turn_retrying && rtc_turn_runtime_failed(transport->turn)))
        return rtc_turn_runtime_set_peer(transport->turn, &transport->remote_candidate);
    return RTC_OK;
}

int rtc_transport_add_remote_candidate(rtc_transport_t *transport,
                                       const rtc_transport_candidate_t *candidate) {
    if (!transport || !candidate || candidate->address[0] == '\0' || candidate->port == 0 ||
        !memchr(candidate->address, '\0', sizeof(candidate->address)) ||
        !memchr(candidate->protocol, '\0', sizeof(candidate->protocol)) ||
        !memchr(candidate->username_fragment, '\0', sizeof(candidate->username_fragment)) ||
        (unsigned)candidate->type > RTC_TRANSPORT_CANDIDATE_PRFLX)
        return RTC_ERR_INVALID;
    if (strcmp(candidate->protocol, "udp") && strcmp(candidate->protocol, "tcp"))
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_add_candidate_impl, (void *)candidate);
}

int rtc_transport_candidate_from_ice(rtc_transport_candidate_t *out,
                                     const rtc_ice_candidate_t *in) {
    if (!out || !in)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    if (!memchr(in->hostname, '\0', sizeof(in->hostname)) ||
        !memchr(in->protocol, '\0', sizeof(in->protocol)) ||
        !memchr(in->username_fragment, '\0', sizeof(in->username_fragment)))
        return RTC_ERR_INVALID;
    char ip[256];
    uint16_t port = 0;
    if (in->hostname[0]) {
        if (in->type != ICE_CANDIDATE_HOST || !in->hostname_port ||
            !rtc_ice_candidate_is_mdns_hostname(in->hostname))
            return RTC_ERR_INVALID;
        memcpy(ip, in->hostname, strlen(in->hostname) + 1);
        port = in->hostname_port;
    } else {
        int rc = rtc_addr_to_string(&in->addr, ip, sizeof(ip), &port);
        if (rc != RTC_OK)
            return rc;
    }

    size_t flen = strlen(in->foundation);
    if (flen >= sizeof(out->foundation))
        flen = sizeof(out->foundation) - 1;
    memcpy(out->foundation, in->foundation, flen);
    out->foundation[flen] = '\0';
    size_t ilen = strlen(ip);
    if (ilen >= sizeof(out->address))
        ilen = sizeof(out->address) - 1;
    memcpy(out->address, ip, ilen);
    out->address[ilen] = '\0';
    const char *protocol = in->protocol[0] ? in->protocol : "udp";
    if (strcmp(protocol, "udp") && strcmp(protocol, "tcp"))
        return RTC_ERR_INVALID;
    memcpy(out->protocol, protocol, strlen(protocol) + 1);
    memcpy(out->username_fragment, in->username_fragment, strlen(in->username_fragment) + 1);
    out->port = port;
    out->type = RTC_TRANSPORT_CANDIDATE_HOST;
    if (in->type == ICE_CANDIDATE_SRFLX)
        out->type = RTC_TRANSPORT_CANDIDATE_SRFLX;
    else if (in->type == ICE_CANDIDATE_RELAY)
        out->type = RTC_TRANSPORT_CANDIDATE_RELAY;
    else if (in->type == ICE_CANDIDATE_PRFLX)
        out->type = RTC_TRANSPORT_CANDIDATE_PRFLX;
    else if (in->type != ICE_CANDIDATE_HOST)
        return RTC_ERR_INVALID;

    if (in->has_related_addr) {
        char related_ip[64];
        uint16_t related_port = 0;
        int rc =
            rtc_addr_to_string(&in->related_addr, related_ip, sizeof(related_ip), &related_port);
        if (rc != RTC_OK)
            return rc;
        size_t rlen = strlen(related_ip);
        if (rlen >= sizeof(out->related_address))
            rlen = sizeof(out->related_address) - 1;
        memcpy(out->related_address, related_ip, rlen);
        out->related_address[rlen] = '\0';
        out->related_port = related_port;
        out->has_related_address = true;
    }

    return RTC_OK;
}

static int tx_start_ice_impl(rtc_transport_t *transport, void *arg) {
    (void)arg;
    if (transport->ice_mode != RTC_ICE_MODE_FULL)
        return RTC_ERR_INVALID;
    if (atomic_load_explicit(&transport->selected_remote_valid, memory_order_acquire) &&
        !transport->ice_restarting)
        return RTC_OK;
    int rc = transport_send_ice_check(transport);
    transport_arm_ice_timer(transport);
    return transport->turn ? RTC_OK : rc;
}

int rtc_transport_start_ice(rtc_transport_t *transport) {
    if (!transport)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_start_ice_impl, NULL);
}

static int tx_start_dtls_impl(rtc_transport_t *transport, void *arg) {
    (void)arg;
    if (!transport->dtls.remote_fingerprint_set ||
        !atomic_load_explicit(&transport->selected_remote_valid, memory_order_acquire))
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

int rtc_transport_start_dtls(rtc_transport_t *transport) {
    if (!transport)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_start_dtls_impl, NULL);
}

static int tx_get_dtls_parameters_impl(rtc_transport_t *transport, void *arg) {
    rtc_dtls_parameters_t *out = (rtc_dtls_parameters_t *)arg;
    memset(out, 0, sizeof(*out));
    out->role = transport_public_dtls_role(transport->dtls.role);
    memcpy(out->fingerprint, rtc_dtls_get_fingerprint(&transport->dtls), sizeof(out->fingerprint));
    return RTC_OK;
}

int rtc_transport_get_dtls_parameters(rtc_transport_t *transport, rtc_dtls_parameters_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_get_dtls_parameters_impl, out);
}

static int tx_set_remote_dtls_impl(rtc_transport_t *transport, void *arg) {
    const rtc_dtls_parameters_t *remote = arg;
    rtc_dtls_role_t role = remote->role == RTC_TRANSPORT_DTLS_ROLE_CLIENT ? RTC_DTLS_ROLE_SERVER
                                                                          : RTC_DTLS_ROLE_CLIENT;
    if (transport->dtls.state != RTC_DTLS_STATE_NEW && transport->dtls.role != role)
        return RTC_ERR_INVALID;
    int rc = rtc_dtls_set_remote_fingerprint(&transport->dtls, remote->fingerprint);
    if (rc != RTC_OK || transport->dtls.state != RTC_DTLS_STATE_NEW)
        return rc;
    return rtc_dtls_set_role(&transport->dtls, role);
}

int rtc_transport_set_remote_dtls_parameters(rtc_transport_t *transport,
                                             const rtc_dtls_parameters_t *remote) {
    if (!transport || !remote || !memchr(remote->fingerprint, '\0', sizeof(remote->fingerprint)) ||
        (remote->role != RTC_TRANSPORT_DTLS_ROLE_CLIENT &&
         remote->role != RTC_TRANSPORT_DTLS_ROLE_SERVER))
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_set_remote_dtls_impl, (void *)remote);
}

static int tx_set_controlling_impl(rtc_transport_t *transport, void *arg) {
    bool controlling = *(bool *)arg;
    if (transport->ice_mode == RTC_ICE_MODE_LITE && controlling)
        return RTC_ERR_INVALID;
    transport->controlling = controlling;
    return RTC_OK;
}

int rtc_transport_set_ice_controlling(rtc_transport_t *transport, bool controlling) {
    return transport ? tx_op_invoke(transport, tx_set_controlling_impl, &controlling)
                     : RTC_ERR_INVALID;
}

static void transport_on_turn_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                     const rtc_addr_t *from, void *user) {
    rtc_transport_t *transport = user;
    rtc_transport_retain(transport);
    transport->receiving_turn = true;
    transport_handle_packet(type, data, len, from, user);
    transport->receiving_turn = false;
    rtc_transport_release(transport);
}

typedef struct {
    rtc_turn_config_t configs[RTC_TURN_MAX_ADDRESSES];
    rtc_addr_t addresses[RTC_TURN_MAX_ADDRESSES];
    unsigned count;
} turn_setup_t;

static bool transport_same_turn_address(const rtc_addr_t *a, const rtc_addr_t *b) {
    if (a->addr.ss_family != b->addr.ss_family)
        return false;
    if (a->addr.ss_family == AF_INET) {
        struct sockaddr_in left, right;
        memcpy(&left, &a->addr, sizeof(left));
        memcpy(&right, &b->addr, sizeof(right));
        return left.sin_port == right.sin_port && left.sin_addr.s_addr == right.sin_addr.s_addr;
    }
    struct sockaddr_in6 left, right;
    memcpy(&left, &a->addr, sizeof(left));
    memcpy(&right, &b->addr, sizeof(right));
    return left.sin6_port == right.sin6_port && left.sin6_scope_id == right.sin6_scope_id &&
           memcmp(&left.sin6_addr, &right.sin6_addr, sizeof(left.sin6_addr)) == 0;
}

static void transport_clear_turn_config(rtc_transport_t *transport) {
    for (unsigned i = 0; i < RTC_TURN_MAX_ADDRESSES; i++) {
        transport_turn_attempt_t *attempt = &transport->turn_attempts[i];
        OPENSSL_cleanse(attempt->credential, sizeof(attempt->credential));
        free(attempt->ca_file);
        memset(attempt, 0, sizeof(*attempt));
    }
    transport->turn_retrying = false;
}

static int transport_next_turn_address(rtc_transport_t *transport) {
    while (transport->turn_address_attempts < transport->turn_address_count) {
        unsigned index = transport->turn_address_attempts++;
        const rtc_turn_config_t *config = &transport->turn_attempts[index].config;
        rtc_turn_runtime_t *next =
            rtc_turn_runtime_create(transport->worker, config, transport_on_turn_packet, transport);
        if (!next) {
            RTC_LOG_WARN("TURN address %u/%u initialization failed", index + 1,
                         transport->turn_address_count);
            continue;
        }
        /* Keep the failed instance until a replacement exists, so relay-only
         * sends never fall through to the direct listener during retries. */
        rtc_turn_runtime_destroy(transport->turn);
        transport->turn = next;
        transport->turn_address_deadline = rtc_time_ms() + RTC_TURN_ADDRESS_TIMEOUT_MS;
        if (!transport->remote_candidate_valid ||
            rtc_turn_runtime_set_peer(next, &transport->remote_candidate) == RTC_OK)
            return RTC_OK;
        rtc_turn_runtime_abort(next);
    }
    RTC_LOG_ERR("TURN address alternatives exhausted");
    return RTC_ERR_SOCKET;
}

static void transport_turn_retry_timer(void *user) {
    rtc_transport_t *transport = user;
    transport->turn_retry_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load(&transport->closed))
        return;
    if (rtc_turn_runtime_allocated(transport->turn)) {
        transport->turn_retrying = false;
        return;
    }
    if (rtc_turn_runtime_failed(transport->turn) ||
        rtc_time_ms() >= transport->turn_address_deadline) {
        rtc_mutex_lock(&transport->send_mutex);
        rtc_turn_runtime_abort(transport->turn);
        int rc = transport_next_turn_address(transport);
        rtc_mutex_unlock(&transport->send_mutex);
        if (rc != RTC_OK) {
            transport->turn_retrying = false;
            return;
        }
    }
    transport->turn_retry_timer =
        rtc_worker_add_timer(transport->worker, rtc_time_ms() + RTC_TURN_RETRY_POLL_MS,
                             transport_turn_retry_timer, transport);
    if (transport->turn_retry_timer == RTC_WORKER_TIMER_INVALID) {
        RTC_LOG_ERR("TURN retry timer allocation failed");
        rtc_turn_runtime_abort(transport->turn);
        transport->turn_retrying = false;
    }
}

static int tx_set_turn_impl(rtc_transport_t *transport, void *arg) {
    const turn_setup_t *setup = arg;
    if (transport->turn_address_count || transport->turn || transport->ice_txn_registered ||
        atomic_load(&transport->selected_remote_valid) || atomic_load(&transport->closed))
        return RTC_ERR_INVALID;
    for (unsigned i = 0; i < setup->count; i++) {
        const rtc_turn_config_t *cfg = &setup->configs[i];
        transport_turn_attempt_t *attempt = &transport->turn_attempts[i];
        if (cfg->ca_file) {
            attempt->ca_file = malloc(strlen(cfg->ca_file) + 1);
            if (!attempt->ca_file) {
                transport_clear_turn_config(transport);
                return RTC_ERR_NOMEM;
            }
            strcpy(attempt->ca_file, cfg->ca_file);
        }
        strcpy(attempt->host, cfg->server_host);
        strcpy(attempt->username, cfg->username);
        strcpy(attempt->credential, cfg->credential);
        attempt->address = setup->addresses[i];
        attempt->config = *cfg;
        attempt->config.server_host = attempt->host;
        attempt->config.username = attempt->username;
        attempt->config.credential = attempt->credential;
        attempt->config.ca_file = attempt->ca_file;
        attempt->config.resolved_address = &attempt->address;
    }
    transport->turn_address_count = setup->count;
    transport->turn_address_attempts = 0;
    transport->turn_retrying = true;
    rtc_mutex_lock(&transport->send_mutex);
    int rc = transport_next_turn_address(transport);
    rtc_mutex_unlock(&transport->send_mutex);
    if (rc != RTC_OK) {
        transport->turn_retrying = false;
        return rc;
    }
    transport_turn_retry_timer(transport);
    return rtc_turn_runtime_failed(transport->turn) && !transport->turn_retrying ? RTC_ERR_GENERIC
                                                                                 : RTC_OK;
}

static int transport_restart_failed_turn(rtc_transport_t *transport) {
    if (!transport->turn_address_count || transport->turn_retrying ||
        (transport->turn && !rtc_turn_runtime_failed(transport->turn)))
        return RTC_OK;
    rtc_mutex_lock(&transport->send_mutex);
    atomic_store(&transport->selected_remote_valid, false);
    transport->turn_address_attempts = 0;
    transport->turn_retrying = true;
    int rc = transport_next_turn_address(transport);
    rtc_mutex_unlock(&transport->send_mutex);
    if (rc != RTC_OK) {
        transport->turn_retrying = false;
        return rc;
    }
    transport_turn_retry_timer(transport);
    return RTC_OK;
}

int rtc_transport_set_turn_server(rtc_transport_t *transport, const char *host, uint16_t port,
                                  const char *username, const char *credential) {
    rtc_turn_server_t server = {.host = host,
                                .port = port,
                                .username = username,
                                .credential = credential,
                                .protocol = RTC_TURN_UDP};
    return rtc_transport_set_turn_server_ex(transport, &server);
}

static int transport_resolve_turn_server(rtc_transport_t *transport,
                                         const rtc_turn_server_t *server, turn_setup_t *out) {
    if (!transport || !server || !server->host || !server->host[0] || !server->username ||
        !server->credential || (unsigned)server->protocol > RTC_TURN_TLS ||
        (server->ca_file && server->protocol != RTC_TURN_TLS) ||
        strlen(server->host) >= TURN_HOST_MAX || strlen(server->username) >= TURN_USERNAME_MAX ||
        strlen(server->credential) >= TURN_CREDENTIAL_MAX ||
        server->address_count > RTC_TURN_MAX_ADDRESSES ||
        (!!server->addresses != !!server->address_count) ||
        (server->relay_family && server->relay_family != AF_INET &&
         server->relay_family != AF_INET6))
        return RTC_ERR_INVALID;
    uint16_t port = server->port ? server->port : server->protocol == RTC_TURN_TLS ? 5349 : 3478;
    turn_setup_t setup = {0};
    if (server->addresses) {
        for (size_t i = 0; i < server->address_count; i++) {
            const rtc_addr_t *address = &server->addresses[i];
            size_t expected = address->addr.ss_family == AF_INET    ? sizeof(struct sockaddr_in)
                              : address->addr.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                                    : 0;
            char ip[64];
            uint16_t address_port;
            if (!expected || address->len != expected ||
                rtc_addr_to_string(address, ip, sizeof(ip), &address_port) != RTC_OK ||
                address_port != port)
                return RTC_ERR_INVALID;
            setup.addresses[setup.count++] = *address;
        }
    } else if (rtc_addr_from_string(&setup.addresses[0], server->host, port) == RTC_OK) {
        setup.count = 1;
    } else {
        if (rtc_worker_on_loop_thread(transport->worker)) {
            RTC_LOG_ERR("TURN hostname resolution must occur outside the media worker");
            return RTC_ERR_INVALID;
        }
        struct addrinfo hints = {0}, *addresses = NULL;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = server->protocol == RTC_TURN_UDP ? SOCK_DGRAM : SOCK_STREAM;
        char service[8];
        snprintf(service, sizeof(service), "%u", port);
        if (getaddrinfo(server->host, service, &hints, &addresses) != 0 || !addresses) {
            RTC_LOG_ERR("TURN server DNS resolution failed");
            return RTC_ERR_SOCKET;
        }
        for (struct addrinfo *entry = addresses; entry; entry = entry->ai_next) {
            size_t expected = entry->ai_family == AF_INET    ? sizeof(struct sockaddr_in)
                              : entry->ai_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                             : 0;
            if (!expected || entry->ai_addrlen != expected)
                continue;
            rtc_addr_t candidate = {0};
            memcpy(&candidate.addr, entry->ai_addr, expected);
            candidate.len = (socklen_t)expected;
            bool duplicate = false, family_present = false;
            for (unsigned i = 0; i < setup.count; i++) {
                if (setup.addresses[i].addr.ss_family == candidate.addr.ss_family)
                    family_present = true;
                if (transport_same_turn_address(&setup.addresses[i], &candidate))
                    duplicate = true;
            }
            if (duplicate)
                continue;
            if (setup.count && !family_present) {
                unsigned end = setup.count < RTC_TURN_MAX_ADDRESSES ? setup.count++
                                                                    : RTC_TURN_MAX_ADDRESSES - 1;
                for (unsigned i = end; i > 1; i--)
                    setup.addresses[i] = setup.addresses[i - 1];
                setup.addresses[1] = candidate;
            } else if (setup.count < RTC_TURN_MAX_ADDRESSES) {
                setup.addresses[setup.count++] = candidate;
            }
        }
        freeaddrinfo(addresses);
        if (!setup.count) {
            RTC_LOG_ERR("TURN DNS returned no supported address");
            return RTC_ERR_SOCKET;
        }
    }
    for (unsigned i = 2; i < setup.count; i++) {
        if (setup.addresses[i].addr.ss_family != setup.addresses[0].addr.ss_family &&
            setup.addresses[1].addr.ss_family == setup.addresses[0].addr.ss_family) {
            rtc_addr_t alternate = setup.addresses[i];
            memmove(&setup.addresses[2], &setup.addresses[1], (i - 1) * sizeof(setup.addresses[0]));
            setup.addresses[1] = alternate;
            break;
        }
    }
    rtc_turn_config_t config = (rtc_turn_config_t){.server_host = server->host,
                                                   .server_port = port,
                                                   .username = server->username,
                                                   .credential = server->credential,
                                                   .protocol = server->protocol,
                                                   .ca_file = server->ca_file,
                                                   .relay_family = server->relay_family};
    for (unsigned i = 0; i < setup.count; i++)
        setup.configs[i] = config;
    *out = setup;
    return RTC_OK;
}

int rtc_transport_set_turn_server_ex(rtc_transport_t *transport, const rtc_turn_server_t *server) {
    return rtc_transport_set_turn_servers(transport, server, 1);
}

int rtc_transport_set_turn_servers(rtc_transport_t *transport, const rtc_turn_server_t *servers,
                                   size_t count) {
    if (!transport || !servers || !count || count > RTC_TURN_MAX_ADDRESSES)
        return RTC_ERR_INVALID;
    turn_setup_t resolved[RTC_TURN_MAX_ADDRESSES] = {0}, setup = {0};
    for (size_t i = 0; i < count; i++) {
        if ((servers[i].relay_family == AF_INET6) != (servers[0].relay_family == AF_INET6))
            return RTC_ERR_INVALID;
        int rc = transport_resolve_turn_server(transport, &servers[i], &resolved[i]);
        if (rc != RTC_OK && rc != RTC_ERR_SOCKET)
            return rc;
    }
    bool used[RTC_TURN_MAX_ADDRESSES][RTC_TURN_MAX_ADDRESSES] = {0};
    int previous_family = AF_UNSPEC;
    for (unsigned round = 0; round < RTC_TURN_MAX_ADDRESSES; round++)
        for (size_t identity = 0; identity < count && setup.count < RTC_TURN_MAX_ADDRESSES;
             identity++) {
            unsigned address = resolved[identity].count;
            for (unsigned i = 0; i < resolved[identity].count; i++) {
                if (used[identity][i])
                    continue;
                if (address == resolved[identity].count)
                    address = i;
                if (previous_family != AF_UNSPEC &&
                    resolved[identity].addresses[i].addr.ss_family != previous_family) {
                    address = i;
                    break;
                }
            }
            if (address == resolved[identity].count)
                continue;
            used[identity][address] = true;
            unsigned index = setup.count++;
            setup.configs[index] = resolved[identity].configs[address];
            setup.addresses[index] = resolved[identity].addresses[address];
            previous_family = setup.addresses[index].addr.ss_family;
        }
    if (!setup.count)
        return RTC_ERR_SOCKET;
    return tx_op_invoke(transport, tx_set_turn_impl, &setup);
}

static int tx_get_relay_impl(rtc_transport_t *transport, void *arg) {
    rtc_transport_candidate_t *candidate = arg;
    rtc_addr_t relay;
    int rc = rtc_turn_runtime_candidate(transport->turn, &relay);
    if (rc != RTC_OK)
        return rc;
    memset(candidate, 0, sizeof(*candidate));
    rc = rtc_addr_to_string(&relay, candidate->address, sizeof(candidate->address),
                            &candidate->port);
    if (rc != RTC_OK)
        return rc;
    strcpy(candidate->protocol, "udp");
    strcpy(candidate->foundation, "turn");
    candidate->type = RTC_TRANSPORT_CANDIDATE_RELAY;
    snprintf(candidate->username_fragment, sizeof(candidate->username_fragment), "%s",
             transport->ice_ufrag);
    return RTC_OK;
}

int rtc_transport_get_relay_candidate(rtc_transport_t *transport,
                                      rtc_transport_candidate_t *candidate) {
    if (!transport || !candidate)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_get_relay_impl, candidate);
}

static int tx_get_stats_impl(rtc_transport_t *transport, void *arg) {
    rtc_transport_stats_t *out = (rtc_transport_stats_t *)arg;
    memset(out, 0, sizeof(*out));
    out->closed = atomic_load_explicit(&transport->closed, memory_order_acquire);
    out->turn_address_attempts = transport->turn_address_attempts;
    out->turn_failed = !out->closed && transport->turn_address_attempts &&
                       !transport->turn_retrying &&
                       (!transport->turn || rtc_turn_runtime_failed(transport->turn));
    rtc_mutex_lock(&transport->send_mutex);
    transport_expire_members_locked(transport, rtc_time_ms());
    out->rtcp_members = transport->rtcp_membership.members;
    out->rtcp_senders = transport->rtcp_membership.senders;
    out->rtp_packets_sent = transport->pacer.packets_sent;
    out->rtp_bytes_sent = transport->pacer.bytes_sent;
    out->rtp_packets_queued = transport->pacer.packets;
    out->rtp_bytes_queued = transport->pacer.bytes;
    out->rtp_packets_dropped = transport->pacer.packets_dropped;
    out->rtp_frames_dropped = transport->pacer.frames_dropped;
    out->rtp_retransmissions_sent = transport->rtp_retransmissions_sent;
    out->rtp_retransmissions_rejected = transport->rtp_retransmissions_rejected;
    rtc_mutex_unlock(&transport->send_mutex);
    out->ice_mode = transport->ice_mode;
    out->selected_tuple_valid =
        atomic_load_explicit(&transport->selected_remote_valid, memory_order_acquire);
    if (out->selected_tuple_valid)
        out->selected_remote = transport->selected_remote;
    out->dtls_state = transport_dtls_state(transport->dtls.state);
    out->srtp_ready = atomic_load_explicit(&transport->srtp_ready, memory_order_acquire);
    out->packets_received =
        atomic_load_explicit(&transport->packets_received, memory_order_relaxed);
    out->bytes_received = atomic_load_explicit(&transport->bytes_received, memory_order_relaxed);
    out->dtls_packets_received =
        atomic_load_explicit(&transport->dtls_packets_received, memory_order_relaxed);
    out->ice_controlling = transport->controlling;
    out->consent_expired = transport->consent_expired;
    out->ice_restarting = transport->ice_restarting;
    out->remote_candidates_deferred = transport->remote_candidates_deferred;
    out->remote_candidates_ineligible = transport->remote_candidates_ineligible;
    out->target_bitrate_bps =
        transport->initial_outgoing_bitrate_bps ? transport->initial_outgoing_bitrate_bps : 500000;
#ifdef MRTC_ENABLE_TWCC
    if (transport->bwe)
        out->target_bitrate_bps = rtc_bwe_target_bitrate(transport->bwe);
#endif
    return RTC_OK;
}

const char *rtc_transport_cname(const rtc_transport_t *transport) {
    return transport ? transport->rtcp_cname : NULL;
}

rtc_worker_t *rtc_transport_worker(rtc_transport_t *transport) {
    return transport ? transport->worker : NULL;
}

int rtc_transport_get_stats(rtc_transport_t *transport, rtc_transport_stats_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_get_stats_impl, out);
}

static int tx_close_impl(rtc_transport_t *transport, void *arg) {
    (void)arg;
    if (transport->listener) {
        rtc_listener_unregister_ufrag(transport->listener, transport->ice_ufrag);
        transport_unregister_ice_txn(transport);
        if (!transport->turn &&
            atomic_load_explicit(&transport->selected_remote_valid, memory_order_acquire))
            rtc_listener_unregister_tuple(transport->listener, &transport->selected_remote);
    }
    transport_cancel_ice_timer(transport);
    transport_cancel_dtls_timer(transport);
    if (transport->turn_retry_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(transport->worker, transport->turn_retry_timer);
        transport->turn_retry_timer = RTC_WORKER_TIMER_INVALID;
    }
    transport_clear_turn_config(transport);
    rtc_rtp_demux_close(&transport->rtp_demux);
#ifdef MRTC_ENABLE_TWCC
    if (transport->twcc_fb_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(transport->worker, transport->twcc_fb_timer);
        transport->twcc_fb_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (transport->bwe) {
        rtc_bwe_destroy(transport->bwe);
        transport->bwe = NULL;
    }
#endif
    rtc_mutex_lock(&transport->send_mutex);
    if (transport->pacer_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(transport->worker, transport->pacer_timer);
        transport->pacer_timer = RTC_WORKER_TIMER_INVALID;
    }
    rtc_rtp_pacer_clear(&transport->pacer);
    if (transport->turn) {
        rtc_turn_runtime_destroy(transport->turn);
        transport->turn = NULL;
    }
    if (atomic_load_explicit(&transport->srtp_ready, memory_order_acquire)) {
        rtc_srtp_close(&transport->srtp_send);
        rtc_srtp_close(&transport->srtp_recv);
        atomic_store_explicit(&transport->srtp_ready, false, memory_order_release);
    }
    rtc_mutex_unlock(&transport->send_mutex);
    rtc_dtls_close(&transport->dtls);
    memset(transport->local_sources, 0, sizeof(transport->local_sources));
    if (!transport->source_callback_depth)
        atomic_store_explicit(&transport->sources_closed, true, memory_order_release);
    return RTC_OK;
}

void rtc_transport_close(rtc_transport_t *transport) {
    if (!transport)
        return;
    bool was_closed = atomic_exchange_explicit(&transport->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    /* Serialize teardown with packet/timer dispatch. Retained work can then
     * only observe closure and release its reference. */
    tx_op_invoke(transport, tx_close_impl, NULL);
}

void rtc_transport_destroy(rtc_transport_t *transport) {
    if (!transport)
        return;
    rtc_transport_close(transport);
    rtc_transport_release(transport);
}

static int tx_set_dtls_role_impl(rtc_transport_t *transport, void *arg) {
    rtc_transport_dtls_role_t role = *(const rtc_transport_dtls_role_t *)arg;
    if (transport->dtls.state != RTC_DTLS_STATE_NEW)
        return RTC_ERR_INVALID;
    rtc_dtls_role_t internal_role = transport_internal_dtls_role(role);
    if (transport->dtls.role == internal_role)
        return RTC_OK;
    return rtc_dtls_set_role(&transport->dtls, internal_role);
}

int rtc_transport_set_dtls_role(rtc_transport_t *transport, rtc_transport_dtls_role_t role) {
    if (!transport)
        return RTC_ERR_INVALID;
    return tx_op_invoke(transport, tx_set_dtls_role_impl, &role);
}

TX_DEFINE_CB_SETTER(on_rtp, rtc_transport_rtp_fn, on_rtp, on_rtp_user)

typedef struct {
    rtc_rtp_sink_fn sink;
    rtc_rtp_resolve_fn resolve;
    void *user;
} tx_rtp_router_args_t;

static int tx_set_rtp_router_impl(rtc_transport_t *transport, void *arg) {
    tx_rtp_router_args_t *a = (tx_rtp_router_args_t *)arg;
    if (!transport->rtp_demux.ready)
        return rtc_rtp_demux_init(&transport->rtp_demux, a->sink, a->resolve, a->user);
    transport->rtp_demux.sink = a->sink;
    transport->rtp_demux.resolve = a->resolve;
    transport->rtp_demux.resolve_user = a->user;
    return RTC_OK;
}

void rtc_transport_set_rtp_router(rtc_transport_t *transport, rtc_rtp_sink_fn sink,
                                  rtc_rtp_resolve_fn resolve, void *resolve_user) {
    if (!transport || !sink)
        return;
    tx_rtp_router_args_t args = {sink, resolve, resolve_user};
    tx_op_invoke(transport, tx_set_rtp_router_impl, &args);
}

typedef struct {
    uint32_t ssrc;
    void *user;
} tx_bind_rtp_args_t;

static int tx_bind_rtp_impl(rtc_transport_t *transport, void *arg) {
    tx_bind_rtp_args_t *a = (tx_bind_rtp_args_t *)arg;
    int rc = transport_observe_source(transport, a->ssrc, true);
    if (rc != RTC_OK)
        return rc;
    return rtc_rtp_demux_bind(&transport->rtp_demux, a->ssrc, a->user);
}

int rtc_transport_bind_rtp(rtc_transport_t *transport, uint32_t ssrc, void *user) {
    if (!transport || !user)
        return RTC_ERR_INVALID;
    tx_bind_rtp_args_t args = {ssrc, user};
    return tx_op_invoke(transport, tx_bind_rtp_impl, &args);
}

static int tx_unbind_rtp_impl(rtc_transport_t *transport, void *arg) {
    uint32_t ssrc = *(const uint32_t *)arg;
    rtc_rtp_demux_unbind(&transport->rtp_demux, ssrc);
    return RTC_OK;
}

void rtc_transport_unbind_rtp(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport)
        return;
    tx_op_invoke(transport, tx_unbind_rtp_impl, &ssrc);
}

typedef struct {
    uint32_t ssrc;
    void *result;
} tx_rtp_bound_args_t;

static int tx_rtp_bound_impl(rtc_transport_t *transport, void *arg) {
    tx_rtp_bound_args_t *a = (tx_rtp_bound_args_t *)arg;
    a->result = rtc_rtp_demux_get(&transport->rtp_demux, a->ssrc);
    return RTC_OK;
}

void *rtc_transport_rtp_bound(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport)
        return NULL;
    tx_rtp_bound_args_t args = {ssrc, NULL};
    tx_op_invoke(transport, tx_rtp_bound_impl, &args);
    return args.result;
}

#ifdef MRTC_ENABLE_TWCC
static void transport_record_twcc_arrival(rtc_transport_t *transport, const rtc_rtp_packet_t *pkt) {
    if (!transport->twcc_enabled || transport->twcc_ext_id == 0 || !pkt->header.extension ||
        !pkt->ext_data || pkt->ext_len == 0)
        return;
    rtc_rtp_ext_t exts[RTC_RTP_EXT_MAX_ENTRIES];
    size_t cnt = RTC_RTP_EXT_MAX_ENTRIES;
    if (rtc_rtp_ext_parse_body(pkt->ext_data, pkt->ext_len, exts, &cnt) != RTC_OK)
        return;
    const rtc_rtp_ext_t *e = rtc_rtp_ext_find(exts, cnt, transport->twcc_ext_id);
    if (!e)
        return;
    uint16_t tseq = rtc_rtp_ext_read_transport_cc(e);
    rtc_twcc_receiver_on_packet(&transport->twcc_receiver, tseq, rtc_time_us());
    if (transport->twcc_remote_ssrc == 0)
        transport->twcc_remote_ssrc = pkt->header.ssrc;
    transport->twcc_have_packets = true;
}

/* Scan a compound RTCP packet for transport-cc feedback (PT=205, FMT=15) and
 * feed each reported packet's send/recv timing to the bandwidth estimator. Does
 * not consume the datagram: the same compound is still delivered to on_rtcp so
 * the media session sees any bundled SR/RR/NACK/PLI. */
static void transport_consume_twcc_feedback(rtc_transport_t *transport, const uint8_t *buf,
                                            size_t len) {
    if (!transport->twcc_enabled || !transport->bwe)
        return;
    size_t offset = 0;
    while (offset + 4 <= len) {
        if (((buf[offset] >> 6) & 0x03) != 2)
            break;
        size_t sub_len = (((size_t)buf[offset + 2] << 8 | buf[offset + 3]) + 1) * 4;
        if (offset + sub_len > len)
            break;
        uint8_t pt, fmt;
        if (rtc_rtcp_get_pt_fmt(buf + offset, sub_len, &pt, &fmt) && pt == RTCP_PT_RTPFB &&
            fmt == 15) {
            rtc_rtcp_twcc_t tw;
            if (rtc_rtcp_parse_twcc(&tw, buf + offset, sub_len) == RTC_OK) {
                for (int i = 0; i < tw.item_count; i++) {
                    const rtc_twcc_sent_pkt_t *s =
                        rtc_twcc_sender_lookup(&transport->twcc_sender, tw.items[i].seq);
                    if (!s)
                        continue;
                    uint64_t recv_us = tw.items[i].received ? tw.items[i].recv_time_us : 0;
                    rtc_bwe_on_packet_feedback(transport->bwe, s->send_time_us, recv_us, s->size);
                }
            }
        }
        offset += sub_len;
    }
}

static void transport_twcc_fb_timer(void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    transport->twcc_fb_timer = RTC_WORKER_TIMER_INVALID;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire))
        return;
    if (transport->twcc_have_packets && transport->twcc_receiver.have_base) {
        uint8_t fb[RTCP_MAX_PACKET];
        size_t fb_len = 0;
        if (rtc_twcc_receiver_build_feedback(&transport->twcc_receiver, transport->twcc_local_ssrc,
                                             transport->twcc_remote_ssrc, fb, sizeof(fb) - 64,
                                             &fb_len) == RTC_OK) {
            uint8_t out[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
            if (fb_len <= sizeof(out)) {
                memcpy(out, fb, fb_len);
                size_t out_len = fb_len;
                int rc = rtc_transport_send_rtcp(transport, out, &out_len, sizeof(out));
                if (rc != RTC_OK)
                    RTC_LOG_WARN("Transport-wide feedback send failed: %d", rc);
            }
        }
        transport->twcc_have_packets = false;
    }
    transport->twcc_fb_timer = rtc_worker_add_timer(transport->worker, rtc_time_ms() + 100,
                                                    transport_twcc_fb_timer, transport);
}

static void transport_bwe_trampoline(uint32_t bitrate_bps, void *user) {
    rtc_transport_t *transport = (rtc_transport_t *)user;
    if (transport->on_bitrate)
        transport->on_bitrate(bitrate_bps, transport->on_bitrate_user);
}

typedef struct {
    uint8_t ext_id;
    uint32_t local_ssrc;
} tx_enable_twcc_args_t;

static int tx_enable_twcc_impl(rtc_transport_t *transport, void *arg) {
    tx_enable_twcc_args_t *a = (tx_enable_twcc_args_t *)arg;
    if (!transport->enable_twcc)
        return RTC_ERR_INVALID;
    if (transport->twcc_enabled)
        return RTC_OK;
    bool shared = false;
    for (unsigned i = 0; i < LOCAL_SOURCE_MAX; i++)
        shared |= a->local_ssrc && transport->local_sources[i].ssrc == a->local_ssrc;
    uint32_t reporter = a->local_ssrc;
    if (!shared) {
        int rc = rtc_transport_new_ssrc(transport, &reporter);
        if (rc != RTC_OK)
            return rc;
        rc = rtc_transport_register_source(transport, reporter, transport_twcc_rotate, transport,
                                           NULL);
        if (rc != RTC_OK)
            return rc;
    }
    transport->twcc_ext_id = a->ext_id;
    transport->twcc_local_ssrc = reporter;
    rtc_twcc_sender_init(&transport->twcc_sender);
    rtc_twcc_receiver_init(&transport->twcc_receiver);
    rtc_bwe_config_t cfg = {
        .initial_bps = transport->initial_outgoing_bitrate_bps
                           ? transport->initial_outgoing_bitrate_bps
                           : 500000,
        .min_bps = 100000,
        .max_bps = 4000000,
    };
    transport->bwe = rtc_bwe_create(&cfg);
    if (!transport->bwe)
        return RTC_ERR_NOMEM;
    rtc_bwe_on_bitrate_change(transport->bwe, transport_bwe_trampoline, transport);
    transport->twcc_enabled = true;
    if (transport->twcc_fb_timer == RTC_WORKER_TIMER_INVALID)
        transport->twcc_fb_timer = rtc_worker_add_timer(transport->worker, rtc_time_ms() + 100,
                                                        transport_twcc_fb_timer, transport);
    return RTC_OK;
}

int rtc_transport_enable_twcc(rtc_transport_t *transport, uint8_t ext_id, uint32_t local_ssrc) {
    if (!transport || ext_id == 0 || ext_id > 14)
        return RTC_ERR_INVALID;
    tx_enable_twcc_args_t args = {ext_id, local_ssrc};
    return tx_op_invoke(transport, tx_enable_twcc_impl, &args);
}

rtc_twcc_sender_t *rtc_transport_twcc_sender(rtc_transport_t *transport) {
    if (!transport || !transport->twcc_enabled)
        return NULL;
    return &transport->twcc_sender;
}

TX_DEFINE_CB_SETTER(on_bitrate_estimate, rtc_transport_bitrate_fn, on_bitrate, on_bitrate_user)

void rtc_transport_report_rtcp_loss(rtc_transport_t *transport, uint8_t fraction_lost) {
    if (transport && transport->twcc_enabled && transport->bwe)
        rtc_bwe_on_loss(transport->bwe, fraction_lost);
}
#endif /* MRTC_ENABLE_TWCC */

TX_DEFINE_CB_SETTER(on_rtcp, rtc_transport_rtcp_fn, on_rtcp, on_rtcp_user)

TX_DEFINE_CB_SETTER(on_data, rtc_transport_data_fn, on_data, on_data_user)

typedef struct {
    const uint8_t *data;
    size_t len;
} tx_send_data_args_t;

static int tx_send_data_impl(rtc_transport_t *transport, void *arg) {
    tx_send_data_args_t *a = (tx_send_data_args_t *)arg;
    if (transport->dtls.state != RTC_DTLS_STATE_CONNECTED || atomic_load(&transport->closed) ||
        !atomic_load(&transport->selected_remote_valid) || a->len > INT_MAX)
        return RTC_ERR_INVALID;
    int written = SSL_write(transport->dtls.ssl, a->data, (int)a->len);
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

int rtc_transport_send_data(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport || !data)
        return RTC_ERR_INVALID;
    tx_send_data_args_t args = {data, len};
    return tx_op_invoke(transport, tx_send_data_impl, &args);
}

static int transport_send_raw_locked(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport || !data || len == 0 || atomic_load(&transport->closed))
        return RTC_ERR_INVALID;
    if (!atomic_load_explicit(&transport->selected_remote_valid, memory_order_acquire))
        return RTC_ERR_INVALID;
    if (rtc_time_ms() - atomic_load(&transport->consent_last_ms) >= transport->consent_timeout_ms)
        return RTC_ERR_INVALID;
    return transport_send_to(transport, data, len, &transport->selected_remote);
}

static int transport_pacer_write(const uint8_t *wire, size_t len, void *user) {
    rtc_transport_t *transport = user;
    int rc = transport_send_raw_locked(transport, wire, len);
    if (rc == RTC_OK) {
#ifdef MRTC_ENABLE_TWCC
        if (transport->twcc_enabled) {
            rtc_rtp_packet_t packet;
            rtc_rtp_ext_t exts[16];
            size_t count = 16;
            if (rtc_rtp_parse(&packet, wire, len) == RTC_OK && packet.ext_data &&
                rtc_rtp_ext_parse_body(packet.ext_data, packet.ext_len, exts, &count) == RTC_OK) {
                const rtc_rtp_ext_t *ext = rtc_rtp_ext_find(exts, count, transport->twcc_ext_id);
                if (ext && ext->len == 2) {
                    uint16_t seq = rtc_rtp_ext_read_transport_cc(ext);
                    rtc_twcc_sent_pkt_t *entry =
                        &transport->twcc_sender.ring[seq & (RTC_TWCC_SENDER_RING - 1)];
                    *entry = (rtc_twcc_sent_pkt_t){.seq = seq,
                                                   .size = (uint16_t)len,
                                                   .send_time_us = rtc_time_us(),
                                                   .used = true};
                }
            }
        }
#endif
        uint32_t ssrc;
        memcpy(&ssrc, wire + 8, sizeof(ssrc));
        (void)rtc_rtcp_membership_touch(&transport->rtcp_membership, ntohl(ssrc), true, true,
                                        rtc_time_ms());
    }
    return rc;
}

bool rtc_transport_rtp_pacing_enabled(const rtc_transport_t *transport) {
    return transport && transport->enable_rtp_pacing;
}

static void transport_pacer_tick(void *user);

static int transport_pacer_schedule_locked(rtc_transport_t *transport) {
    if (!rtc_rtp_pacer_pending(&transport->pacer) ||
        transport->pacer_timer != RTC_WORKER_TIMER_INVALID)
        return RTC_OK;
    uint64_t now = rtc_time_ms();
    transport->pacer_timer =
        rtc_worker_add_timer(transport->worker,
                             now + rtc_rtp_pacer_delay_ms(&transport->pacer, now),
                             transport_pacer_tick, transport);
    if (transport->pacer_timer == RTC_WORKER_TIMER_INVALID) {
        rtc_rtp_pacer_clear(&transport->pacer);
        RTC_LOG_ERR("RTP pacing timer exhausted; pending frames discarded");
        return RTC_ERR_NOMEM;
    }
    return RTC_OK;
}

static void transport_pacer_tick(void *user) {
    rtc_transport_t *transport = user;
    rtc_mutex_lock(&transport->send_mutex);
    transport->pacer_timer = RTC_WORKER_TIMER_INVALID;
    if (!atomic_load(&transport->closed)) {
        uint64_t before = transport->pacer.frames_dropped;
        uint64_t now = rtc_time_ms();
        rtc_rtp_pacer_run(&transport->pacer, now, transport_pacer_write, transport);
        if (transport->pacer.frames_dropped != before &&
            now - transport->pacer_last_log_ms >= 1000) {
            RTC_LOG_WARN("RTP pacing expired/failed frames=%llu queued_bytes=%zu",
                         (unsigned long long)transport->pacer.frames_dropped,
                         transport->pacer.bytes);
            transport->pacer_last_log_ms = now;
        }
        (void)transport_pacer_schedule_locked(transport);
    }
    rtc_mutex_unlock(&transport->send_mutex);
}

void rtc_transport_discard_rtp(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport)
        return;
    rtc_mutex_lock(&transport->send_mutex);
    rtc_rtp_pacer_discard(&transport->pacer, ssrc);
    rtc_mutex_unlock(&transport->send_mutex);
}

void rtc_transport_forget_rtp(rtc_transport_t *transport, uint32_t ssrc) {
    if (!transport)
        return;
    rtc_mutex_lock(&transport->send_mutex);
    rtc_rtp_pacer_forget(&transport->pacer, ssrc);
    rtc_mutex_unlock(&transport->send_mutex);
}

bool rtc_transport_sync_rtp_stats(rtc_transport_t *transport, rtc_rtcp_stats_t *stats,
                                  uint64_t *dropped) {
    if (!transport || !stats)
        return false;
    rtc_pacer_sent_t sent;
    if (!rtc_transport_rtp_sent(transport, stats->ssrc, &sent))
        return false;
    stats->packets_sent = (uint32_t)sent.packets_sent;
    stats->octets_sent = (uint32_t)sent.octets_sent;
    stats->rtp_timestamp = sent.timestamp;
    stats->send_time_ms = sent.send_time_ms;
    stats->send_clock_valid = sent.packets_sent != 0;
    if (dropped)
        *dropped = sent.packets_dropped - sent.packets_rejected;
    return true;
}

bool rtc_transport_rtp_sent(rtc_transport_t *transport, uint32_t ssrc, rtc_pacer_sent_t *out) {
    if (!transport || !out)
        return false;
    rtc_mutex_lock(&transport->send_mutex);
    bool found = rtc_rtp_pacer_sent(&transport->pacer, ssrc, out);
    rtc_mutex_unlock(&transport->send_mutex);
    return found;
}

int rtc_transport_send_raw(rtc_transport_t *transport, const uint8_t *data, size_t len) {
    if (!transport || !data)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&transport->send_mutex);
    int rc = transport_send_raw_locked(transport, data, len);
    rtc_mutex_unlock(&transport->send_mutex);
    return rc;
}

static int tx_retransmit_rtp_impl(rtc_transport_t *transport, void *arg) {
    tx_send_data_args_t *a = arg;
    if (atomic_load(&transport->closed) || !atomic_load(&transport->srtp_ready))
        return RTC_ERR_INVALID;
    size_t tag_len = transport->srtp_send.profile == RTC_SRTP_PROFILE_AES128_CM_SHA1_80
                         ? SRTP_AUTH_TAG_LEN
                         : SRTP_GCM_TAG_LEN;
    if (a->len < RTP_HEADER_SIZE + tag_len || (a->data[0] >> 6) != 2)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&transport->send_mutex);
    int rc = RTC_ERR_TIMEOUT;
    /* Video repair is allowed only after an initial successful write, never
     * from a staged, expired, failed, or retired frame. Ciphertext is reused. */
    uint32_t ssrc;
    memcpy(&ssrc, a->data + 8, sizeof(ssrc));
    rtc_pacer_sent_t sent;
    bool video = rtc_rtp_pacer_sent(&transport->pacer, ntohl(ssrc), &sent);
    if (!source_retired_locked(transport, ntohl(ssrc)) &&
        (!transport->enable_rtp_pacing ||
         (video && rtc_rtp_pacer_repair(&transport->pacer, a->data, a->len, rtc_time_ms()))))
        rc = transport_send_raw_locked(transport, a->data, a->len);
    if (rc == RTC_OK)
        transport->rtp_retransmissions_sent++;
    else
        transport->rtp_retransmissions_rejected++;
    rtc_mutex_unlock(&transport->send_mutex);
    return rc;
}

int rtc_transport_retransmit_rtp(rtc_transport_t *transport, const uint8_t *wire, size_t len) {
    if (!transport || !wire || len < RTP_HEADER_SIZE + SRTP_AUTH_TAG_LEN || len > SRTP_MAX_PACKET)
        return RTC_ERR_INVALID;
    tx_send_data_args_t args = {wire, len};
    return tx_op_invoke(transport, tx_retransmit_rtp_impl, &args);
}

int rtc_transport_send_rtp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap) {
    return rtc_transport_send_rtp_media(transport, buf, len, buf_cap, false, 0);
}

int rtc_transport_send_rtp_media(rtc_transport_t *transport, uint8_t *buf, size_t *len,
                                 size_t buf_cap, bool video, uint32_t bitrate_bps) {
    if (!transport || !buf || !len || *len < RTP_HEADER_SIZE)
        return RTC_ERR_INVALID;
    if (*len > buf_cap)
        return RTC_ERR_NOMEM;
    uint32_t ssrc;
    memcpy(&ssrc, buf + 8, sizeof(ssrc));
    ssrc = ntohl(ssrc);
    size_t payload_len = 0;
    if (video && transport->enable_rtp_pacing) {
        rtc_rtp_packet_t packet;
        if (rtc_rtp_parse(&packet, buf, *len) != RTC_OK) {
            rtc_transport_discard_rtp(transport, ssrc);
            return RTC_ERR_INVALID;
        }
        payload_len = packet.payload_len;
    }
    rtc_mutex_lock(&transport->send_mutex);
    int rc = RTC_ERR_INVALID;
    if (!atomic_load(&transport->closed) && atomic_load(&transport->srtp_ready) &&
        atomic_load(&transport->selected_remote_valid)) {
        transport_expire_members_locked(transport, rtc_time_ms());
        if (!rtc_rtcp_membership_can_add(&transport->rtcp_membership, ssrc, true)) {
            RTC_LOG_ERR("Local RTP membership capacity exhausted");
            if (video && transport->enable_rtp_pacing)
                rtc_rtp_pacer_abort(&transport->pacer, ssrc, false);
            rtc_mutex_unlock(&transport->send_mutex);
            return RTC_ERR_NOMEM;
        }
        rc = source_reserve_locked(transport, ssrc);
        if (rc == RTC_OK) {
            for (unsigned i = 0; i < transport->used_source_count; i++)
                if (transport->used_sources[i].ssrc == ssrc)
                    transport->used_sources[i].crypto_used = true;
            rc = rtc_srtp_protect(&transport->srtp_send, buf, len, buf_cap);
        }
        if (rc != RTC_OK && video && transport->enable_rtp_pacing)
            rtc_rtp_pacer_abort(&transport->pacer, ssrc, false);
        if (rc == RTC_OK && video && transport->enable_rtp_pacing) {
            if (bitrate_bps)
                transport->pacer.bitrate_bps = bitrate_bps;
            rc = rtc_rtp_pacer_enqueue(&transport->pacer, buf, *len, payload_len, rtc_time_ms());
            if (rc == RTC_OK) {
                rc = transport_pacer_schedule_locked(transport);
                if (rc != RTC_OK)
                    rtc_rtp_pacer_abort(&transport->pacer, ssrc, true);
            } else if (rtc_time_ms() - transport->pacer_last_log_ms >= 1000) {
                RTC_LOG_WARN("RTP pacing frame rejected: %d", rc);
                transport->pacer_last_log_ms = rtc_time_ms();
            }
        } else if (rc == RTC_OK) {
            rc = transport_send_raw_locked(transport, buf, *len);
        }
        if (rc == RTC_OK && !(video && transport->enable_rtp_pacing))
            rc = rtc_rtcp_membership_touch(&transport->rtcp_membership, ssrc, true, true,
                                           rtc_time_ms());
    }
    rtc_mutex_unlock(&transport->send_mutex);
    return rc;
}

int rtc_transport_send_rtcp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap) {
    if (!transport || !buf || !len || buf_cap < 20)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&transport->send_mutex);
    int rc = RTC_ERR_INVALID;
    if (!atomic_load(&transport->closed) && atomic_load(&transport->srtp_ready) &&
        atomic_load(&transport->selected_remote_valid)) {
        rc = rtc_rtcp_compound_feedback(buf, len, buf_cap - 20);
        transport_expire_members_locked(transport, rtc_time_ms());
        rtc_rtcp_membership_t updated = transport->rtcp_membership;
        if (rc == RTC_OK) {
            size_t tag = transport->srtp_send.profile == RTC_SRTP_PROFILE_AES128_CM_SHA1_80
                             ? SRTP_AUTH_TAG_LEN
                             : SRTP_GCM_TAG_LEN;
            rc = rtc_rtcp_membership_on_rtcp(&updated, buf, *len, true, *len + tag + 4 + 48,
                                             rtc_time_ms());
        }
        if (rc == RTC_OK) {
            size_t offset = 0;
            while (offset + 4 <= *len) {
                size_t size = (((size_t)buf[offset + 2] << 8) | buf[offset + 3]) * 4 + 4;
                if (size > *len - offset)
                    break;
                unsigned count = buf[offset] & 31;
                if (buf[offset + 1] == RTCP_PT_BYE && 4 + count * 4 <= size)
                    for (unsigned i = 0; i < count; i++) {
                        uint32_t ssrc;
                        memcpy(&ssrc, buf + offset + 4 + i * 4, sizeof(ssrc));
                        rtc_rtp_pacer_discard(&transport->pacer, ntohl(ssrc));
                    }
                offset += size;
            }
        }
        if (rc == RTC_OK && *len >= 8) {
            uint32_t ssrc = source_read(buf + 4);
            bool departing = source_departing(buf, *len, ssrc);
            if (!departing)
                rc = source_reserve_locked(transport, ssrc);
            if (rc == RTC_OK)
                for (unsigned i = 0; i < transport->used_source_count; i++)
                    if (transport->used_sources[i].ssrc == ssrc)
                        transport->used_sources[i].crypto_used = true;
        }
        if (rc == RTC_OK)
            rc = rtc_srtp_protect_rtcp(&transport->srtp_send, buf, len, buf_cap);
        if (rc == RTC_OK)
            rc = transport_send_raw_locked(transport, buf, *len);
        if (rc == RTC_OK)
            transport->rtcp_membership = updated;
    }
    rtc_mutex_unlock(&transport->send_mutex);
    return rc;
}

int rtc_transport_send_report(rtc_transport_t *transport, rtc_rtcp_stats_t *stats, bool sender) {
    if (!transport || !stats)
        return RTC_ERR_INVALID;
    if (sender && rtc_transport_sync_rtp_stats(transport, stats, NULL) && !stats->packets_sent)
        return RTC_ERR_TIMEOUT;
    rtc_transport_stats_t state;
    int rc = rtc_transport_get_stats(transport, &state);
    if (rc != RTC_OK || state.closed || !state.srtp_ready)
        return rc != RTC_OK ? rc : RTC_ERR_INVALID;
    uint64_t now = rtc_time_ms();
    rtc_mutex_lock(&transport->send_mutex);
    transport->rtcp_target_bps = state.target_bitrate_bps;
    rtc_rtcp_membership_expire(&transport->rtcp_membership, state.target_bitrate_bps, now);
    rc = rtc_rtcp_membership_touch(&transport->rtcp_membership, stats->ssrc, true, false, now);
    if (rc == RTC_OK)
        rtc_rtcp_stats_set_membership(stats, &transport->rtcp_membership, now);
    rtc_mutex_unlock(&transport->send_mutex);
    if (rc != RTC_OK)
        return rc;
    rtc_rtcp_packet_t packet;
    rc = rtc_rtcp_build_report(&packet, stats, sender && stats->report_we_sent);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_rtcp_report_due(stats, state.target_bitrate_bps, packet.buf_len + 20 + 48, now);
    if (rc != RTC_OK)
        return rc;
    uint8_t wire[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    memcpy(wire, packet.buf, packet.buf_len);
    size_t length = packet.buf_len;
    rc = rtc_transport_send_rtcp(transport, wire, &length, sizeof(wire));
    if (rc == RTC_OK) {
        if (packet.header.packet_type == RTCP_PT_SR)
            rtc_rtcp_stats_record_sr(stats, &packet.sr, rtc_time_ms());
        stats->last_report_size = length + 48;
        rtc_mutex_lock(&transport->send_mutex);
        rtc_rtcp_stats_set_membership(stats, &transport->rtcp_membership, rtc_time_ms());
        rtc_mutex_unlock(&transport->send_mutex);
        rc = rtc_rtcp_stats_report_sent(stats);
    }
    return rc;
}

int rtc_transport_send_bye(rtc_transport_t *transport, uint32_t ssrc) {
    return rtc_transport_send_bye_named(transport, ssrc, NULL);
}

int rtc_transport_send_bye_named(rtc_transport_t *transport, uint32_t ssrc, const char *cname) {
    rtc_transport_discard_rtp(transport, ssrc);
    uint8_t wire[8 + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    size_t length;
    int rc = rtc_rtcp_build_bye(wire, sizeof(wire), &length, ssrc);
    if (rc == RTC_OK && cname)
        rc = rtc_rtcp_compound_feedback_named(wire, &length, sizeof(wire) - 20, cname);
    return rc == RTC_OK ? rtc_transport_send_rtcp(transport, wire, &length, sizeof(wire)) : rc;
}
