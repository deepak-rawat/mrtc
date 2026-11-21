#include "rtc_ice.h"

#include <stdio.h>
#include <string.h>

#define RTC_ICE_CHECK_INTERVAL_MS   250
#define RTC_ICE_CONSENT_INTERVAL_MS 5000
#define RTC_ICE_MAX_CHECKS          120
#define RTC_ICE_CONSENT_TIMEOUT_MS  30000
#define RTC_ICE_CHECK_PRIORITY      0x7E0000FF

static void ice_timer(void *user);

static bool ice_closed(const rtc_ice_agent_t *ice) {
    return atomic_load_explicit(ice->config.closed, memory_order_acquire);
}

static bool ice_same_addr(const rtc_addr_t *a, const rtc_addr_t *b) {
    char ai[64], bi[64];
    uint16_t ap, bp;
    return rtc_addr_to_string(a, ai, sizeof(ai), &ap) == RTC_OK &&
           rtc_addr_to_string(b, bi, sizeof(bi), &bp) == RTC_OK && ap == bp && strcmp(ai, bi) == 0;
}

static void ice_cancel_timer(rtc_ice_agent_t *ice) {
    if (ice->timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(ice->config.worker, ice->timer);
        ice->timer = RTC_WORKER_TIMER_INVALID;
    }
}

static void ice_unregister_txn(rtc_ice_agent_t *ice) {
    if (ice->txn_registered) {
        rtc_listener_unregister_stun_txn(ice->config.listener, ice->txn_id);
        ice->txn_registered = false;
    }
}

static int ice_send_check(rtc_ice_agent_t *ice) {
    if (!ice->remote_ice_valid || !ice->remote_candidate_valid)
        return RTC_ERR_INVALID;
    if (atomic_load(&ice->selected_remote_valid) && !ice->restarting) {
        ice->remote_candidate = ice->selected_remote;
    } else if (ice->remote_candidate_count) {
        ice->remote_candidate = ice->remote_candidates[ice->remote_candidate_index++ %
                                                       (unsigned)ice->remote_candidate_count];
    }
    int rc = ice->config.prepare_peer(&ice->remote_candidate, ice->config.user);
    if (rc != RTC_OK)
        return rc;

    char username[RTC_ICE_UFRAG_MAX * 2 + 2];
    snprintf(username, sizeof(username), "%s:%s", ice->remote_ufrag, ice->ufrag);
    rtc_stun_msg_t req;
    rc = rtc_stun_build_binding_request(&req, username, ice->remote_pwd, RTC_ICE_CHECK_PRIORITY,
                                        ice->controlling, ice->tie_breaker, ice->controlling);
    if (rc != RTC_OK)
        return rc;

    ice_unregister_txn(ice);
    memcpy(ice->txn_id, req.txn_id, STUN_TXN_ID_SIZE);
    rc = rtc_listener_register_stun_txn(ice->config.listener, ice->txn_id, ice->config.on_packet,
                                        ice->config.user);
    if (rc != RTC_OK)
        return rc;
    ice->txn_registered = true;
    ice->txn_controlling = ice->controlling;
    rc = ice->config.send(req.buf, req.buf_len, &ice->remote_candidate, ice->config.user);
    if (rc == RTC_OK)
        ice->check_count++;
    return rc;
}

void rtc_ice_arm_timer(rtc_ice_agent_t *ice) {
    if (ice->consent_expired || ice_closed(ice) || ice->timer != RTC_WORKER_TIMER_INVALID)
        return;
    uint64_t delay = atomic_load(&ice->selected_remote_valid) ? RTC_ICE_CONSENT_INTERVAL_MS
                                                              : RTC_ICE_CHECK_INTERVAL_MS;
    if (delay > ice->consent_timeout_ms / 3)
        delay = ice->consent_timeout_ms / 3;
    if (atomic_load(&ice->selected_remote_valid)) {
        uint64_t now = rtc_time_ms();
        uint64_t deadline = atomic_load(&ice->consent_last_ms) + ice->consent_timeout_ms;
        if (deadline <= now)
            delay = 1;
        else if (delay > deadline - now)
            delay = deadline - now;
    }
    ice->timer = rtc_worker_add_timer(ice->config.worker, rtc_time_ms() + delay, ice_timer, ice);
    if (ice->timer == RTC_WORKER_TIMER_INVALID)
        RTC_LOG_ERR("ICE timer allocation failed");
}

void rtc_ice_clear_selected(rtc_ice_agent_t *ice) {
    rtc_mutex_lock(ice->config.send_mutex);
    bool selected = atomic_exchange(&ice->selected_remote_valid, false);
    rtc_mutex_unlock(ice->config.send_mutex);
    if (selected && !ice->relayed)
        rtc_listener_unregister_tuple(ice->config.listener, &ice->selected_remote);
}

static void ice_timer(void *user) {
    rtc_ice_agent_t *ice = user;
    ice->timer = RTC_WORKER_TIMER_INVALID;
    if (ice_closed(ice))
        return;
    if (atomic_load(&ice->selected_remote_valid) &&
        rtc_time_ms() - atomic_load(&ice->consent_last_ms) >= ice->consent_timeout_ms) {
        rtc_ice_clear_selected(ice);
        ice->consent_expired = true;
        ice_unregister_txn(ice);
        return;
    }
    if (!atomic_load(&ice->selected_remote_valid) && ice->check_count >= RTC_ICE_MAX_CHECKS)
        return;
    int rc = ice_send_check(ice);
    if (rc != RTC_OK && rc != RTC_ERR_INVALID)
        RTC_LOG_WARN("ICE connectivity check failed: %d", rc);
    rtc_ice_arm_timer(ice);
}

static void ice_select_remote(rtc_ice_agent_t *ice, const rtc_addr_t *from) {
    bool selected = atomic_load(&ice->selected_remote_valid);
    if (selected && ice_same_addr(&ice->selected_remote, from)) {
        ice->restarting = false;
        return;
    }
    if (!ice->relayed) {
        int rc = rtc_listener_register_tuple(ice->config.listener, from, ice->config.on_packet,
                                             ice->config.user);
        if (rc != RTC_OK) {
            RTC_LOG_ERR("ICE selected tuple registration failed: %d", rc);
            return;
        }
        if (selected)
            rtc_listener_unregister_tuple(ice->config.listener, &ice->selected_remote);
    }
    rtc_mutex_lock(ice->config.send_mutex);
    ice->selected_remote = *from;
    atomic_store_explicit(&ice->selected_remote_valid, true, memory_order_release);
    rtc_mutex_unlock(ice->config.send_mutex);
    ice->restarting = false;
    ice->consent_last_ms = rtc_time_ms();
    rtc_ice_arm_timer(ice);
}

static void ice_binding_reply(rtc_ice_agent_t *ice, const rtc_stun_msg_t *request,
                              const rtc_addr_t *from, bool conflict) {
    rtc_stun_msg_t reply = {0};
    int rc;
    if (conflict) {
        rc = rtc_stun_build_request(&reply, STUN_BINDING_ERROR_RESPONSE, NULL, NULL);
        if (rc != RTC_OK)
            goto failed;
        /* RFC 8445: authenticated 487 Role Conflict. */
        const uint8_t error[] = {0, 9, 0, 4, 0, 0, 4, 87};
        memcpy(reply.buf + reply.buf_len, error, sizeof(error));
        reply.buf_len += sizeof(error);
    } else {
        rc = rtc_stun_build_binding_response(request->buf, request->buf_len, from, reply.buf,
                                             sizeof(reply.buf), &reply.buf_len);
        if (rc != RTC_OK)
            goto failed;
        reply.type = STUN_BINDING_RESPONSE;
    }
    memcpy(reply.txn_id, request->txn_id, STUN_TXN_ID_SIZE);
    memcpy(reply.buf + 8, request->txn_id, STUN_TXN_ID_SIZE);
    rc = rtc_stun_finalize(&reply, ice->pwd);
    if (rc == RTC_OK)
        rc = ice->config.send(reply.buf, reply.buf_len, from, ice->config.user);
failed:
    if (rc != RTC_OK)
        RTC_LOG_WARN("ICE binding reply failed: %d", rc);
}

void rtc_ice_handle_stun(rtc_ice_agent_t *ice, const uint8_t *data, size_t len,
                         const rtc_addr_t *from) {
    if (ice_closed(ice))
        return;
    rtc_stun_msg_t msg;
    if (rtc_stun_parse(&msg, data, len) != RTC_OK)
        return;
    if (msg.type == STUN_BINDING_RESPONSE || msg.type == STUN_BINDING_ERROR_RESPONSE) {
        if (!ice->txn_registered || memcmp(msg.txn_id, ice->txn_id, STUN_TXN_ID_SIZE) != 0 ||
            !ice_same_addr(from, &ice->remote_candidate) ||
            rtc_stun_verify_integrity(data, len, ice->remote_pwd) != RTC_OK)
            return;
        ice_unregister_txn(ice);
        if (msg.type == STUN_BINDING_ERROR_RESPONSE) {
            int code = 0;
            if (rtc_stun_get_error_code(&msg, &code) == RTC_OK && code == 487 &&
                ice->mode == RTC_ICE_MODE_FULL) {
                ice->controlling = !ice->txn_controlling;
                int rc = ice_send_check(ice);
                if (rc != RTC_OK)
                    RTC_LOG_WARN("ICE role-conflict retry failed: %d", rc);
            }
            return;
        }
        ice->consent_last_ms = rtc_time_ms();
        if (ice->controlling)
            ice_select_remote(ice, from);
        rtc_ice_arm_timer(ice);
        return;
    }
    if (msg.type != STUN_BINDING_REQUEST || ice->consent_expired ||
        rtc_stun_verify_integrity(data, len, ice->pwd) != RTC_OK)
        return;
    char expected[RTC_ICE_UFRAG_MAX * 2 + 2];
    snprintf(expected, sizeof(expected), "%s:%s", ice->ufrag, ice->remote_ufrag);
    uint16_t attr_len = 0;
    const uint8_t *username = rtc_stun_find_attr(&msg, STUN_ATTR_USERNAME, &attr_len);
    if (!username || !ice->remote_ice_valid || attr_len != strlen(expected) ||
        memcmp(username, expected, attr_len) != 0)
        return;
    uint16_t role_len = 0;
    const uint8_t *controlling = rtc_stun_find_attr(&msg, STUN_ATTR_ICE_CONTROLLING, &role_len);
    const uint8_t *controlled = rtc_stun_find_attr(&msg, STUN_ATTR_ICE_CONTROLLED, &attr_len);
    if (!controlling && !controlled && atomic_load(&ice->selected_remote_valid) &&
        ice_same_addr(from, &ice->selected_remote)) {
        ice_binding_reply(ice, &msg, from, false);
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
    if (ice->controlling && controlling) {
        if (ice->tie_breaker >= tie)
            conflict = true;
        else
            ice->controlling = false;
    } else if (!ice->controlling && controlled) {
        if (ice->mode == RTC_ICE_MODE_LITE || ice->tie_breaker < tie)
            conflict = true;
        else
            ice->controlling = true;
    }
    ice_binding_reply(ice, &msg, from, conflict);
    if (conflict)
        return;
    if (!ice->controlling && controlling && rtc_stun_find_attr(&msg, STUN_ATTR_USE_CANDIDATE, NULL))
        ice_select_remote(ice, from);
    if (!ice->remote_candidate_valid) {
        ice->remote_candidate = *from;
        ice->remote_candidate_valid = true;
    }
    if (ice->mode == RTC_ICE_MODE_FULL && !atomic_load(&ice->selected_remote_valid)) {
        int rc = ice_send_check(ice);
        if (rc != RTC_OK)
            RTC_LOG_WARN("ICE peer-reflexive check failed: %d", rc);
        rtc_ice_arm_timer(ice);
    }
}

int rtc_ice_init(rtc_ice_agent_t *ice, const rtc_ice_config_t *config) {
    if (!ice || !config || !config->worker || !config->listener || !config->send_mutex ||
        !config->closed || !config->send || !config->prepare_peer || !config->on_packet)
        return RTC_ERR_INVALID;
    memset(ice, 0, sizeof(*ice));
    ice->config = *config;
    ice->timer = RTC_WORKER_TIMER_INVALID;
    ice->mode = config->mode == RTC_ICE_MODE_FULL ? RTC_ICE_MODE_FULL : RTC_ICE_MODE_LITE;
    ice->controlling = ice->mode == RTC_ICE_MODE_FULL;
    ice->consent_timeout_ms =
        config->consent_timeout_ms ? config->consent_timeout_ms : RTC_ICE_CONSENT_TIMEOUT_MS;
    if (ice->consent_timeout_ms < 30)
        ice->consent_timeout_ms = 30;
    atomic_init(&ice->selected_remote_valid, false);
    atomic_init(&ice->consent_last_ms, 0);
    if (rtc_random_bytes((uint8_t *)&ice->tie_breaker, sizeof(ice->tie_breaker)) != RTC_OK ||
        rtc_random_string(ice->ufrag, 8) != RTC_OK || rtc_random_string(ice->pwd, 24) != RTC_OK)
        return RTC_ERR_GENERIC;
    return RTC_OK;
}

int rtc_ice_bind(rtc_ice_agent_t *ice) {
    int rc = rtc_listener_register_ufrag(ice->config.listener, ice->ufrag, ice->config.on_packet,
                                         ice->config.user);
    if (rc == RTC_OK)
        ice->ufrag_registered = true;
    return rc;
}

void rtc_ice_close(rtc_ice_agent_t *ice) {
    if (ice->ufrag_registered) {
        rtc_listener_unregister_ufrag(ice->config.listener, ice->ufrag);
        ice->ufrag_registered = false;
    }
    ice_unregister_txn(ice);
    if (!ice->relayed && atomic_load(&ice->selected_remote_valid))
        rtc_listener_unregister_tuple(ice->config.listener, &ice->selected_remote);
    ice_cancel_timer(ice);
}

void rtc_ice_get_parameters(const rtc_ice_agent_t *ice, rtc_ice_parameters_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->username_fragment, ice->ufrag, sizeof(out->username_fragment));
    memcpy(out->password, ice->pwd, sizeof(out->password));
    out->mode = ice->mode;
}

bool rtc_ice_remote_changed(const rtc_ice_agent_t *ice, const rtc_ice_parameters_t *remote) {
    return ice->remote_ice_valid && (strcmp(ice->remote_ufrag, remote->username_fragment) != 0 ||
                                     strcmp(ice->remote_pwd, remote->password) != 0);
}

static void ice_reset_checks(rtc_ice_agent_t *ice) {
    ice_cancel_timer(ice);
    ice_unregister_txn(ice);
    ice->check_count = 0;
    ice->consent_expired = false;
    ice->restarting = true;
    ice->consent_last_ms = rtc_time_ms();
    ice->remote_candidate_valid = false;
    ice->remote_candidate_count = 0;
    ice->remote_candidate_index = 0;
}

int rtc_ice_restart(rtc_ice_agent_t *ice) {
    if (ice_closed(ice))
        return RTC_ERR_INVALID;
    char ufrag[RTC_ICE_UFRAG_MAX] = {0}, pwd[RTC_ICE_PWD_MAX] = {0};
    if (rtc_random_string(ufrag, 8) != RTC_OK || rtc_random_string(pwd, 24) != RTC_OK)
        return RTC_ERR_GENERIC;
    int rc = rtc_listener_register_ufrag(ice->config.listener, ufrag, ice->config.on_packet,
                                         ice->config.user);
    if (rc != RTC_OK)
        return rc;
    rtc_listener_unregister_ufrag(ice->config.listener, ice->ufrag);
    ice->ufrag_registered = true;
    memcpy(ice->ufrag, ufrag, sizeof(ufrag));
    memcpy(ice->pwd, pwd, sizeof(pwd));
    ice_reset_checks(ice);
    ice->remote_ice_valid = false;
    return RTC_OK;
}

int rtc_ice_set_remote_parameters(rtc_ice_agent_t *ice, const rtc_ice_parameters_t *remote) {
    if (!remote || remote->username_fragment[0] == '\0' || remote->password[0] == '\0' ||
        !memchr(remote->username_fragment, '\0', sizeof(remote->username_fragment)) ||
        !memchr(remote->password, '\0', sizeof(remote->password)))
        return RTC_ERR_INVALID;
    if (remote->mode == RTC_ICE_MODE_LITE) {
        if (ice->mode == RTC_ICE_MODE_LITE)
            return RTC_ERR_INVALID;
        ice->controlling = true;
    }
    if (rtc_ice_remote_changed(ice, remote))
        ice_reset_checks(ice);
    memcpy(ice->remote_ufrag, remote->username_fragment, sizeof(ice->remote_ufrag));
    memcpy(ice->remote_pwd, remote->password, sizeof(ice->remote_pwd));
    ice->remote_ice_valid = true;
    return RTC_OK;
}

int rtc_ice_add_remote_candidate(rtc_ice_agent_t *ice, const rtc_transport_candidate_t *candidate,
                                 int relay_family) {
    if (!candidate || candidate->address[0] == '\0' || candidate->port == 0 ||
        !memchr(candidate->address, '\0', sizeof(candidate->address)) ||
        !memchr(candidate->protocol, '\0', sizeof(candidate->protocol)) ||
        !memchr(candidate->username_fragment, '\0', sizeof(candidate->username_fragment)) ||
        (unsigned)candidate->type > RTC_TRANSPORT_CANDIDATE_PRFLX ||
        (strcmp(candidate->protocol, "udp") && strcmp(candidate->protocol, "tcp")))
        return RTC_ERR_INVALID;
    bool mdns = rtc_ice_candidate_is_mdns_hostname(candidate->address);
    rtc_addr_t address = {0};
    if ((mdns && candidate->type != RTC_TRANSPORT_CANDIDATE_HOST) ||
        (!mdns && rtc_addr_from_string(&address, candidate->address, candidate->port) != RTC_OK))
        return RTC_ERR_INVALID;
    if (candidate->username_fragment[0] && ice->remote_ice_valid &&
        strcmp(candidate->username_fragment, ice->remote_ufrag)) {
        RTC_LOG_DBG("ICE: ignoring candidate from an older ICE generation");
        return RTC_OK;
    }
    if (strcmp(candidate->protocol, "tcp") == 0) {
        if (ice->remote_candidates_ineligible != UINT32_MAX)
            ice->remote_candidates_ineligible++;
        RTC_LOG_INFO("ICE: TCP candidate is ineligible for this UDP transport");
        return RTC_OK;
    }
    if (mdns) {
        if (ice->remote_candidates_deferred != UINT32_MAX)
            ice->remote_candidates_deferred++;
        /* Never resolve .local through unicast DNS; authenticated checks
         * supply the peer-reflexive address without blocking the worker. */
        RTC_LOG_INFO("ICE: mDNS candidate deferred to authenticated peer-reflexive checks");
        return RTC_OK;
    }
    if (relay_family != AF_UNSPEC && address.addr.ss_family != relay_family) {
        if (ice->remote_candidates_ineligible != UINT32_MAX)
            ice->remote_candidates_ineligible++;
        return RTC_OK;
    }
    for (int i = 0; i < ice->remote_candidate_count; i++)
        if (ice_same_addr(&address, &ice->remote_candidates[i]))
            return RTC_OK;
    if (ice->remote_candidate_count == ICE_MAX_CANDIDATES)
        return RTC_ERR_NOMEM;
    ice->remote_candidates[ice->remote_candidate_count++] = address;
    if (!ice->remote_candidate_valid)
        ice->remote_candidate = address;
    ice->remote_candidate_valid = true;
    return RTC_OK;
}

int rtc_ice_candidate_from_sdp(rtc_transport_candidate_t *out, const rtc_ice_candidate_t *in) {
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

int rtc_ice_set_controlling(rtc_ice_agent_t *ice, bool controlling) {
    if (ice->mode == RTC_ICE_MODE_LITE && controlling)
        return RTC_ERR_INVALID;
    ice->controlling = controlling;
    return RTC_OK;
}

int rtc_ice_start(rtc_ice_agent_t *ice) {
    if (ice->mode != RTC_ICE_MODE_FULL)
        return RTC_ERR_INVALID;
    if (atomic_load_explicit(&ice->selected_remote_valid, memory_order_acquire) && !ice->restarting)
        return RTC_OK;
    int rc = ice_send_check(ice);
    rtc_ice_arm_timer(ice);
    return rc;
}

bool rtc_ice_accepts_media(const rtc_ice_agent_t *ice, const rtc_addr_t *from) {
    return !ice->consent_expired && atomic_load(&ice->selected_remote_valid) &&
           ice_same_addr(from, &ice->selected_remote);
}

bool rtc_ice_can_send(const rtc_ice_agent_t *ice) {
    return atomic_load_explicit(&ice->selected_remote_valid, memory_order_acquire) &&
           rtc_time_ms() - atomic_load(&ice->consent_last_ms) < ice->consent_timeout_ms;
}
