/*
 * Peer Connection: control plane.
 *
 * Lifecycle, SDP, connect choreography, timers, public API. The per-peer
 * media plane (RTP routing, RTCP feedback, SR/RR) lives in the core
 * rtc_media_session, owned here as pc->media_session.
 */
#include "rtc_peer_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int peer_dc_send(const uint8_t *data, size_t len, void *user);

static void peer_runtime_close(rtc_peer_connection_t *pc);
static void peer_runtime_on_listener_candidate(const rtc_transport_candidate_t *cand, void *user);
static void peer_runtime_on_listener_done(void *user);
static void peer_runtime_connect_timer(void *user);
static void peer_runtime_on_data(const uint8_t *data, size_t len, void *user);
static rtc_worker_t *peer_runtime_worker(rtc_peer_connection_t *pc);
static int peer_get_stats(const rtc_peer_connection_t *pc, rtc_stats_report_t *report);
static void peer_relay_timer(void *user);
static void peer_health_timer(void *user) {
    rtc_peer_connection_t *pc = user;
    pc->health_timer = RTC_WORKER_TIMER_INVALID;
    rtc_transport_stats_t stats;
    if (pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    if (rtc_transport_get_stats(pc->runtime_transport, &stats) != RTC_OK || stats.closed ||
        stats.dtls_state == RTC_TRANSPORT_DTLS_FAILED) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }
    if (stats.consent_expired) {
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_DISCONNECTED);
        peer_set_connection(pc, RTC_CONNECTION_DISCONNECTED);
    }
    pc->health_timer =
        rtc_worker_add_timer(peer_runtime_worker(pc), rtc_time_ms() + 100, peer_health_timer, pc);
}
static int peer_dispatch(void *owner, void (*fn)(void *), void *arg) {
    rtc_peer_connection_t *pc = owner;
    return rtc_transport_invoke(pc->runtime_transport, fn, arg);
}

static void peer_sctp_timer(void *user) {
    rtc_peer_connection_t *pc = user;
    pc->sctp_timer = RTC_WORKER_TIMER_INVALID;
    if (pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    rtc_dc_manager_tick(&pc->dc_manager, 0);
    pc->sctp_timer =
        rtc_worker_add_timer(peer_runtime_worker(pc), rtc_time_ms() + 10, peer_sctp_timer, pc);
}

static rtc_worker_t *peer_runtime_worker(rtc_peer_connection_t *pc) {
    return rtc_client_runtime_worker(pc->runtime);
}

static rtc_listener_t *peer_runtime_listener(rtc_peer_connection_t *pc) {
    return rtc_client_runtime_listener(pc->runtime);
}

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0)
        return;
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void peer_runtime_parse_stun_url(const char *url, rtc_client_runtime_config_t *out) {
    if (!url || !out || out->stun_server[0] != '\0')
        return;
    const char *host = url;
    if (strncmp(host, "stun:", 5) == 0)
        host += 5;
    else
        return;
    if (strncmp(host, "//", 2) == 0)
        host += 2;

    char tmp[256];
    copy_string(tmp, sizeof(tmp), host);
    char *slash = strchr(tmp, '/');
    if (slash)
        *slash = '\0';

    char *colon = strrchr(tmp, ':');
    if (colon && strchr(tmp, ']') == NULL) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port > 0 && port <= 65535)
            out->stun_port = (uint16_t)port;
    }
    if (tmp[0] != '\0')
        copy_string(out->stun_server, sizeof(out->stun_server), tmp);
}

static void peer_runtime_config_from_peer(const rtc_config_t *config,
                                          rtc_client_runtime_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->stun_port = 3478;
    if (!config)
        return;
    if (config->stun_server) {
        copy_string(out->stun_server, sizeof(out->stun_server), config->stun_server);
        if (config->stun_port)
            out->stun_port = config->stun_port;
        return;
    }
    for (int i = 0; i < config->ice_server_count && i < 4; i++) {
        const rtc_ice_server_t *server = &config->ice_servers[i];
        for (int j = 0; j < server->url_count && j < 4; j++) {
            peer_runtime_parse_stun_url(server->urls[j], out);
            if (out->stun_server[0] != '\0')
                return;
        }
    }
}

static int peer_runtime_init(rtc_peer_connection_t *pc, const rtc_config_t *config) {
    rtc_client_runtime_config_t runtime_config;
    peer_runtime_config_from_peer(config, &runtime_config);
    pc->runtime = rtc_client_runtime_acquire(&runtime_config);
    if (!pc->runtime)
        return RTC_ERR_NOMEM;

    rtc_listener_t *listener = peer_runtime_listener(pc);
    rtc_worker_t *worker = peer_runtime_worker(pc);
    if (!listener || !worker) {
        peer_runtime_close(pc);
        return RTC_ERR_GENERIC;
    }

    pc->runtime_transport = rtc_transport_create(worker, &(rtc_transport_config_t){
                                                             .listener = listener,
                                                             .ice_mode = RTC_ICE_MODE_FULL,
                                                             .enable_rtp_pacing = true,
#ifdef MRTC_ENABLE_TWCC
                                                             .enable_twcc = true,
#endif
                                                         });
    if (!pc->runtime_transport) {
        peer_runtime_close(pc);
        return RTC_ERR_GENERIC;
    }
    if (config) {
        rtc_turn_server_t relays[RTC_TURN_MAX_ADDRESSES] = {0};
        char hosts[RTC_TURN_MAX_ADDRESSES][256];
        size_t relay_count = 0;
        if (config->ice_server_count < 0 || config->ice_server_count > 4)
            goto invalid_config;
        for (int i = 0; i < config->ice_server_count; i++) {
            const rtc_ice_server_t *server = &config->ice_servers[i];
            if (server->url_count < 0 || server->url_count > 4)
                goto invalid_config;
            for (int j = 0; j < server->url_count; j++) {
                const char *url = server->urls[j];
                if (!url || strncmp(url, "stuns:", 6) == 0)
                    goto invalid_config;
                if (strncmp(url, "stun:", 5) == 0)
                    continue;
                bool tls = strncmp(url, "turns:", 6) == 0;
                if ((!tls && strncmp(url, "turn:", 5) != 0) ||
                    relay_count == RTC_TURN_MAX_ADDRESSES || !server->username ||
                    !server->credential)
                    goto invalid_config;
                char *host = hosts[relay_count];
                const char *authority = url + (tls ? 6 : 5);
                if (strlen(authority) >= sizeof(hosts[0]))
                    goto invalid_config;
                copy_string(host, sizeof(hosts[0]), authority);
                rtc_turn_protocol_t protocol = tls ? RTC_TURN_TLS : RTC_TURN_UDP;
                char *query = strchr(host, '?');
                if (query) {
                    if (!strcmp(query, "?transport=tcp"))
                        protocol = tls ? RTC_TURN_TLS : RTC_TURN_TCP;
                    else if (tls || strcmp(query, "?transport=udp"))
                        goto invalid_config;
                    *query = '\0';
                }
                unsigned long port = tls ? 5349 : 3478;
                char *name = host;
                char *colon;
                if (host[0] == '[') {
                    char *bracket = strchr(host + 1, ']');
                    if (!bracket || (bracket[1] && bracket[1] != ':'))
                        goto invalid_config;
                    colon = bracket[1] == ':' ? bracket + 1 : NULL;
                    *bracket = '\0';
                    name++;
                } else {
                    colon = strchr(host, ':');
                    if (colon && strchr(colon + 1, ':'))
                        goto invalid_config;
                }
                if (colon) {
                    char *end = NULL;
                    if (colon[1] < '0' || colon[1] > '9')
                        goto invalid_config;
                    port = strtoul(colon + 1, &end, 10);
                    if (!end || *end || !port || port > 65535)
                        goto invalid_config;
                    *colon = '\0';
                }
                rtc_turn_server_t relay = {.host = name,
                                           .port = (uint16_t)port,
                                           .username = server->username,
                                           .credential = server->credential,
                                           .protocol = protocol,
                                           .ca_file = tls ? (server->tls_ca_file
                                                                 ? server->tls_ca_file
                                                                 : config->turn_tls_ca_file)
                                                          : NULL,
                                           .relay_family = server->relay_family};
                if (!name[0])
                    goto invalid_config;
                relays[relay_count++] = relay;
            }
        }
        if (relay_count) {
            if (rtc_transport_set_turn_servers(pc->runtime_transport, relays, relay_count) !=
                RTC_OK)
                goto invalid_config;
            pc->relay_only = true;
        }
    }
    rtc_transport_on_data(pc->runtime_transport, peer_runtime_on_data, pc);
    pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;

    /* The media session installs the transport's RTP router + RTCP handler and
     * drives SR/RR; senders and receivers are registered as tracks are added. */
    if (rtc_media_session_init(&pc->media_session, pc->runtime_transport, worker) != RTC_OK) {
        peer_runtime_close(pc);
        return RTC_ERR_GENERIC;
    }

    rtc_dtls_parameters_t dtls;
    if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) == RTC_OK)
        memcpy(pc->runtime_fingerprint, dtls.fingerprint, sizeof(pc->runtime_fingerprint));

    int rc = rtc_client_runtime_register_peer(pc->runtime, peer_runtime_on_listener_candidate,
                                              peer_runtime_on_listener_done, pc);
    if (rc != RTC_OK) {
        peer_runtime_close(pc);
        return rc;
    }
    pc->runtime_registered = true;
    return RTC_OK;
invalid_config:
    RTC_LOG_ERR("Unsupported ICE server configuration (supported: STUN, one authenticated "
                "UDP/TCP/TLS TURN server)");
    peer_runtime_close(pc);
    return RTC_ERR_INVALID;
}

static void peer_unregister_on_worker(void *arg) {
    rtc_peer_connection_t *pc = arg;
    rtc_client_runtime_unregister_peer(pc->runtime, pc);
    pc->runtime_registered = false;
}

static void peer_runtime_close(rtc_peer_connection_t *pc) {
    rtc_worker_t *worker = pc->runtime ? peer_runtime_worker(pc) : NULL;
    if (pc->runtime && pc->runtime_registered) {
        (void)rtc_transport_invoke(pc->runtime_transport, peer_unregister_on_worker, pc);
    }
    if (worker && pc->runtime_connect_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_connect_timer);
        pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    }
    rtc_media_session_send_bye(&pc->media_session);
    rtc_media_session_stop(&pc->media_session);
    if (pc->runtime_transport) {
        rtc_transport_destroy(pc->runtime_transport);
        pc->runtime_transport = NULL;
    }
    if (pc->runtime) {
        rtc_client_runtime_release(pc->runtime);
        pc->runtime = NULL;
    }
}

static void peer_stop_on_worker(void *arg) {
    rtc_peer_connection_t *pc = arg;
    pc->connection_state = RTC_CONNECTION_CLOSED;
    pc->signaling_state = RTC_SIGNALING_CLOSED;
    rtc_worker_t *worker = peer_runtime_worker(pc);
    if (pc->sctp_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->sctp_timer);
        pc->sctp_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (pc->relay_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->relay_timer);
        pc->relay_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (pc->health_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->health_timer);
        pc->health_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (pc->runtime_connect_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_connect_timer);
        pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (pc->runtime_registered) {
        rtc_client_runtime_unregister_peer(pc->runtime, pc);
        pc->runtime_registered = false;
    }
    rtc_media_session_stop(&pc->media_session);
    rtc_dc_manager_close(&pc->dc_manager);
}

static bool peer_candidate_addr_equal(const rtc_ice_candidate_t *a, const rtc_ice_candidate_t *b) {
    char aip[64];
    char bip[64];
    uint16_t aport = 0;
    uint16_t bport = 0;
    if (rtc_addr_to_string(&a->addr, aip, sizeof(aip), &aport) != RTC_OK ||
        rtc_addr_to_string(&b->addr, bip, sizeof(bip), &bport) != RTC_OK)
        return false;
    return aport == bport && strcmp(aip, bip) == 0;
}

static bool peer_local_sdp_has_candidate(rtc_peer_connection_t *pc,
                                         const rtc_ice_candidate_t *candidate) {
    size_t count = rtc_sdp_candidate_count(&pc->local_sdp);
    for (size_t i = 0; i < count; i++) {
        const rtc_ice_candidate_t *existing = rtc_sdp_get_candidate(&pc->local_sdp, i);
        if (!existing || existing->type != candidate->type ||
            strcmp(existing->foundation, candidate->foundation) != 0 ||
            !peer_candidate_addr_equal(existing, candidate)) {
            continue;
        }
        return true;
    }
    return false;
}

static void peer_emit_ice_candidate(rtc_peer_connection_t *pc,
                                    const rtc_ice_candidate_t *candidate) {
    if (!pc->on_ice_candidate)
        return;

    char cand_line[sizeof(((rtc_ice_candidate_desc_t *)0)->candidate)];
    if (rtc_ice_candidate_to_string(candidate, cand_line, sizeof(cand_line)) != RTC_OK)
        return;

    int n_emit = pc->transceiver_count > 0 ? pc->transceiver_count : 1;
    for (int j = 0; j < n_emit; j++) {
        rtc_ice_candidate_desc_t cand_desc;
        memset(&cand_desc, 0, sizeof(cand_desc));
        copy_string(cand_desc.candidate, sizeof(cand_desc.candidate), cand_line);

        if (pc->transceiver_count > 0) {
            const struct rtc_rtp_transceiver *t = &pc->transceivers[j];
            copy_string(cand_desc.mid, sizeof(cand_desc.mid), t->mid);
            cand_desc.mid_index = t->mid_index;
        } else {
            cand_desc.mid[0] = '0';
            cand_desc.mid[1] = '\0';
            cand_desc.mid_index = 0;
        }

        pc->on_ice_candidate(&cand_desc, pc->on_ice_candidate_user);
    }
}

static int peer_add_local_transport_candidate(rtc_peer_connection_t *pc,
                                              const rtc_transport_candidate_t *candidate,
                                              int local_pref, bool emit) {
    rtc_ice_candidate_t ice_candidate;
    int rc = rtc_listener_candidate_to_ice(candidate, local_pref, &ice_candidate);
    if (rc != RTC_OK)
        return rc;
    if (peer_local_sdp_has_candidate(pc, &ice_candidate))
        return RTC_OK;
    rc = rtc_sdp_add_candidate(&pc->local_sdp, &ice_candidate);
    if (rc != RTC_OK)
        return rc;
    if (emit)
        peer_emit_ice_candidate(pc, &ice_candidate);
    return RTC_OK;
}

static void peer_replay_listener_candidates(rtc_peer_connection_t *pc) {
    if (pc->relay_only)
        return;
    rtc_listener_t *listener = peer_runtime_listener(pc);
    if (!listener)
        return;
    rtc_transport_candidate_t candidates[ICE_MAX_CANDIDATES];
    int count = ICE_MAX_CANDIDATES;
    int rc = rtc_listener_get_candidates(listener, candidates, &count);
    if (rc != RTC_OK && rc != RTC_ERR_NOMEM)
        return;
    int host_index = 0;
    int srflx_index = 0;
    for (int i = 0; i < count; i++) {
        int local_pref = 65535;
        if (candidates[i].type == RTC_TRANSPORT_CANDIDATE_HOST)
            local_pref -= host_index++;
        else if (candidates[i].type == RTC_TRANSPORT_CANDIDATE_SRFLX)
            local_pref -= srflx_index++;
        (void)peer_add_local_transport_candidate(pc, &candidates[i], local_pref, false);
    }
}

static void peer_runtime_on_listener_candidate(const rtc_transport_candidate_t *cand, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (!pc || !cand || !pc->has_local_desc || pc->relay_only ||
        pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    (void)peer_add_local_transport_candidate(pc, cand, 65535, true);
}

static void peer_runtime_on_listener_done(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (!pc || !pc->has_local_desc || pc->connection_state == RTC_CONNECTION_CLOSED ||
        (pc->relay_only && !pc->relay_candidate_emitted))
        return;
    if (pc->ice_gathering_state == RTC_ICE_GATHERING_COMPLETE)
        return;
    peer_set_ice_gathering(pc, RTC_ICE_GATHERING_COMPLETE);
    if (pc->on_ice_candidate)
        pc->on_ice_candidate(NULL, pc->on_ice_candidate_user);
}

static void peer_relay_timer(void *user) {
    rtc_peer_connection_t *pc = user;
    pc->relay_timer = RTC_WORKER_TIMER_INVALID;
    if (pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    rtc_transport_candidate_t candidate;
    if (rtc_transport_get_relay_candidate(pc->runtime_transport, &candidate) == RTC_OK) {
        if (!pc->relay_candidate_emitted) {
            (void)peer_add_local_transport_candidate(pc, &candidate, 65535, true);
            pc->relay_candidate_emitted = true;
        }
        peer_runtime_on_listener_done(pc);
        return;
    }
    if (rtc_time_ms() >= pc->relay_deadline) {
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_FAILED);
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }
    pc->relay_timer =
        rtc_worker_add_timer(peer_runtime_worker(pc), rtc_time_ms() + 50, peer_relay_timer, pc);
}

static int peer_runtime_fill_sdp_transport(rtc_peer_connection_t *pc, rtc_sdp_t *sdp) {
    rtc_listener_t *listener = peer_runtime_listener(pc);
    if (!pc->runtime_transport || !listener)
        return RTC_ERR_INVALID;

    rtc_ice_parameters_t ice;
    int rc = rtc_transport_get_ice_parameters(pc->runtime_transport, &ice);
    if (rc != RTC_OK)
        return rc;
    memcpy(sdp->ice_ufrag, ice.username_fragment, sizeof(sdp->ice_ufrag));
    memcpy(sdp->ice_pwd, ice.password, sizeof(sdp->ice_pwd));

    rtc_dtls_parameters_t dtls;
    rc = rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls);
    if (rc != RTC_OK)
        return rc;
    memcpy(sdp->fingerprint, dtls.fingerprint, sizeof(sdp->fingerprint));

    if (pc->relay_only) {
        rtc_transport_candidate_t relay;
        if (rtc_transport_get_relay_candidate(pc->runtime_transport, &relay) == RTC_OK) {
            rtc_ice_candidate_t candidate;
            rc = rtc_listener_candidate_to_ice(&relay, 65535, &candidate);
            return rc == RTC_OK ? rtc_sdp_add_candidate(sdp, &candidate) : rc;
        }
        return RTC_OK;
    }
    return rtc_listener_fill_sdp_candidates(listener, sdp);
}

static int peer_runtime_add_remote_candidate(rtc_peer_connection_t *pc,
                                             const rtc_ice_candidate_t *candidate) {
    if (!pc->runtime_transport || !candidate)
        return RTC_ERR_INVALID;

    rtc_transport_candidate_t tc;
    int rc = rtc_transport_candidate_from_ice(&tc, candidate);
    if (rc != RTC_OK)
        return rc;
    return rtc_transport_add_remote_candidate(pc->runtime_transport, &tc);
}

static int peer_runtime_set_remote_desc(rtc_peer_connection_t *pc, const rtc_sdp_t *sdp) {
    if (!pc->runtime_transport || !sdp)
        return RTC_ERR_INVALID;

    rtc_dtls_parameters_t remote_dtls;
    memset(&remote_dtls, 0, sizeof(remote_dtls));
    copy_string(remote_dtls.fingerprint, sizeof(remote_dtls.fingerprint), sdp->fingerprint);
    remote_dtls.role = sdp->setup == RTC_SETUP_ACTIVE ? RTC_TRANSPORT_DTLS_ROLE_CLIENT
                                                      : RTC_TRANSPORT_DTLS_ROLE_SERVER;
    if (sdp->setup == RTC_SETUP_ACTPASS && pc->runtime_connected) {
        rtc_dtls_parameters_t local_dtls;
        if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &local_dtls) != RTC_OK)
            return RTC_ERR_GENERIC;
        remote_dtls.role = local_dtls.role == RTC_TRANSPORT_DTLS_ROLE_CLIENT
                               ? RTC_TRANSPORT_DTLS_ROLE_SERVER
                               : RTC_TRANSPORT_DTLS_ROLE_CLIENT;
    }
    int rc = rtc_transport_set_remote_dtls_parameters(pc->runtime_transport, &remote_dtls);
    if (rc != RTC_OK)
        return rc;

    rtc_ice_parameters_t ice;
    memset(&ice, 0, sizeof(ice));
    copy_string(ice.username_fragment, sizeof(ice.username_fragment), sdp->ice_ufrag);
    copy_string(ice.password, sizeof(ice.password), sdp->ice_pwd);
    ice.mode = sdp->ice_lite ? RTC_ICE_MODE_LITE : RTC_ICE_MODE_FULL;
    rc = rtc_transport_set_remote_ice_parameters(pc->runtime_transport, &ice);
    if (rc != RTC_OK)
        return rc;

    size_t ncand = rtc_sdp_candidate_count(sdp);
    for (size_t i = 0; i < ncand; i++) {
        rc = peer_runtime_add_remote_candidate(pc, rtc_sdp_get_candidate(sdp, i));
        if (rc != RTC_OK)
            return rc;
    }

    return RTC_OK;
}

static int peer_maybe_start_sctp(rtc_peer_connection_t *pc) {
    if (pc->on_data_channel)
        rtc_dc_manager_on_channel(&pc->dc_manager, pc->on_data_channel, pc->on_data_channel_user);
    bool has_sctp = false;
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        if (m->media_type == RTC_MEDIA_APPLICATION) {
            has_sctp = true;
            if (!m->sctp_port ||
                (pc->dc_manager.socket && pc->dc_manager.remote_port != m->sctp_port))
                return RTC_ERR_SDP;
            pc->dc_manager.remote_port = m->sctp_port;
            pc->dc_manager.max_message_size =
                m->max_message_size && m->max_message_size < RTC_DC_MAX_MSG_SIZE
                    ? m->max_message_size
                    : RTC_DC_MAX_MSG_SIZE;
        }
    }
    if (has_sctp && !pc->dc_manager.socket) {
        rtc_dtls_parameters_t dtls;
        if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) != RTC_OK ||
            rtc_dc_manager_connect(&pc->dc_manager, dtls.role == RTC_TRANSPORT_DTLS_ROLE_CLIENT) !=
                RTC_OK) {
            return RTC_ERR_GENERIC;
        }
        peer_sctp_timer(pc);
    }
    return RTC_OK;
}

static void peer_runtime_complete_connection(rtc_peer_connection_t *pc) {
    if (pc->runtime_connected)
        return;
    pc->runtime_connected = true;

    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_sender *s = &pc->transceivers[i].sender;
        if (!rtc_rtp_send_stream_is_active(s->stream))
            continue;
        rtc_rtp_sender_attach_logical(s, pc->runtime_transport);
#ifdef MRTC_ENABLE_TWCC
        if (pc->twcc_ext_id != 0)
            rtc_rtp_sender_attach_twcc(s, pc->twcc_ext_id);
#endif
        rtc_rtp_sender_arm_video(s);
    }
    if (peer_maybe_start_sctp(pc) != RTC_OK) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    rtc_media_session_start(&pc->media_session);
    peer_health_timer(pc);
    if (pc->connection_state == RTC_CONNECTION_FAILED)
        return;

    peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CONNECTED);
    peer_set_connection(pc, RTC_CONNECTION_CONNECTED);
}

static void peer_runtime_connect_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    if (pc->connection_state == RTC_CONNECTION_CLOSED ||
        pc->connection_state == RTC_CONNECTION_FAILED)
        return;

    rtc_transport_stats_t stats;
    if (rtc_transport_get_stats(pc->runtime_transport, &stats) != RTC_OK) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }
    if (stats.dtls_state == RTC_TRANSPORT_DTLS_FAILED || rtc_time_ms() >= pc->connect_deadline) {
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_FAILED);
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    if (!stats.selected_tuple_valid || stats.ice_restarting) {
        pc->runtime_connect_timer = rtc_worker_add_timer(
            peer_runtime_worker(pc), rtc_time_ms() + 50, peer_runtime_connect_timer, pc);
        return;
    }

    if (stats.dtls_state == RTC_TRANSPORT_DTLS_NEW &&
        rtc_transport_start_dtls(pc->runtime_transport) != RTC_OK) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    if (stats.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED && stats.srtp_ready) {
        if (pc->runtime_connected) {
            peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CONNECTED);
            peer_set_connection(pc, RTC_CONNECTION_CONNECTED);
        } else {
            peer_runtime_complete_connection(pc);
        }
        return;
    }

    pc->runtime_connect_timer = rtc_worker_add_timer(peer_runtime_worker(pc), rtc_time_ms() + 50,
                                                     peer_runtime_connect_timer, pc);
}

static void peer_runtime_on_data(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    rtc_dc_manager_recv(&pc->dc_manager, data, len);
}

static int peer_build_sdp(rtc_peer_connection_t *pc, rtc_sdp_t *sdp) {
    int rc = peer_runtime_fill_sdp_transport(pc, sdp);
    if (rc != RTC_OK)
        return rc;

    /* Build multi-media descriptions from transceivers */
    sdp->media_count = 0;
    for (int i = 0; i < pc->transceiver_count && sdp->media_count < SDP_MAX_MEDIA; i++) {
        rtc_rtp_transceiver_fill_sdp_media(&pc->transceivers[i], &sdp->media[sdp->media_count]);
        if (sdp->type == RTC_SDP_ANSWER) {
            rtc_sdp_media_t *m = &sdp->media[sdp->media_count];
            for (int j = 0; j < pc->remote_sdp.media_count; j++) {
                const rtc_sdp_media_t *r = &pc->remote_sdp.media[j];
                if (r->mid_index != m->mid_index)
                    continue;
                m->rtcp_feedback &= r->rtcp_feedback;
                m->rejected = r->rejected;
                rtc_sdp_extmap_t supported[SDP_MAX_EXTMAP];
                int count = m->extmap_count;
                memcpy(supported, m->extmaps, sizeof(supported));
                m->extmap_count = 0;
                for (int k = 0; k < count; k++) {
                    uint8_t id = rtc_sdp_media_find_extmap_id(r, supported[k].uri);
                    if (id)
                        rtc_sdp_media_add_extmap(m, id, supported[k].uri);
                }
                if (!rtc_sdp_media_find_extmap_id(m, RTC_EXT_URI_TRANSPORT_CC))
                    m->rtcp_feedback &= (uint8_t)~RTC_SDP_FB_TRANSPORT_CC;
                bool send =
                    (m->direction == RTC_DIR_SENDRECV || m->direction == RTC_DIR_SENDONLY) &&
                    (r->direction == RTC_DIR_SENDRECV || r->direction == RTC_DIR_RECVONLY);
                bool recv =
                    (m->direction == RTC_DIR_SENDRECV || m->direction == RTC_DIR_RECVONLY) &&
                    (r->direction == RTC_DIR_SENDRECV || r->direction == RTC_DIR_SENDONLY);
                m->direction = send ? (recv ? RTC_DIR_SENDRECV : RTC_DIR_SENDONLY)
                                    : (recv ? RTC_DIR_RECVONLY : RTC_DIR_INACTIVE);
            }
        }
        sdp->media_count++;
    }
    bool application = rtc_u32_map_len(&pc->dc_manager.channels) > 0;
    for (int i = 0; i < pc->remote_sdp.media_count; i++)
        application |= pc->remote_sdp.media[i].media_type == RTC_MEDIA_APPLICATION;
    if (application) {
        if (sdp->media_count >= SDP_MAX_MEDIA)
            return RTC_ERR_SDP;
        rtc_sdp_media_t *m = &sdp->media[sdp->media_count];
        memset(m, 0, sizeof(*m));
        m->media_type = RTC_MEDIA_APPLICATION;
        if (pc->application_mid < 0)
            pc->application_mid = pc->next_mid++;
        m->mid_index = pc->application_mid;
        sdp->media_count++;
        m->sctp_port = 5000;
        m->max_message_size = RTC_DC_MAX_MSG_SIZE;
    }
    for (int i = 1; i < sdp->media_count; i++) {
        rtc_sdp_media_t media = sdp->media[i];
        int j = i;
        while (j > 0 && sdp->media[j - 1].mid_index > media.mid_index) {
            sdp->media[j] = sdp->media[j - 1];
            j--;
        }
        sdp->media[j] = media;
    }

    /* Also set legacy single-media fields for backward compat */
    if (pc->transceiver_count > 0) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[0];
        const char *mime = t->sender.codec.mime_type;

        if (strncmp(mime, "audio/", 6) == 0) {
            sdp->media_type = RTC_MEDIA_AUDIO;
            size_t clen = strlen(mime + 6);
            if (clen >= sizeof(sdp->codec_name))
                clen = sizeof(sdp->codec_name) - 1;
            memcpy(sdp->codec_name, mime + 6, clen);
            sdp->codec_name[clen] = '\0';
        } else if (strncmp(mime, "video/", 6) == 0) {
            sdp->media_type = RTC_MEDIA_VIDEO;
            size_t clen = strlen(mime + 6);
            if (clen >= sizeof(sdp->codec_name))
                clen = sizeof(sdp->codec_name) - 1;
            memcpy(sdp->codec_name, mime + 6, clen);
            sdp->codec_name[clen] = '\0';
        } else {
            sdp->media_type = RTC_MEDIA_APPLICATION;
        }

        sdp->payload_type = t->sender.codec.payload_type;
        sdp->clockrate = (int)t->sender.codec.clock_rate;
        sdp->channels = t->sender.codec.channels;
    }

    return RTC_OK;
}

static void peer_apply_negotiated_media(rtc_peer_connection_t *pc) {
    if (pc->signaling_state != RTC_SIGNALING_STABLE)
        return;
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[i];
        const rtc_sdp_media_t *remote = NULL;
        for (int j = 0; j < pc->remote_sdp.media_count; j++) {
            if (pc->remote_sdp.media[j].mid_index == t->mid_index &&
                pc->remote_sdp.media[j].media_type != RTC_MEDIA_APPLICATION)
                remote = &pc->remote_sdp.media[j];
        }
        bool send =
            remote && (t->direction == RTC_DIR_SENDRECV || t->direction == RTC_DIR_SENDONLY) &&
            (remote->direction == RTC_DIR_SENDRECV || remote->direction == RTC_DIR_RECVONLY);
        bool receive =
            remote && (t->direction == RTC_DIR_SENDRECV || t->direction == RTC_DIR_RECVONLY) &&
            (remote->direction == RTC_DIR_SENDRECV || remote->direction == RTC_DIR_SENDONLY);
        bool was_receiving = rtc_rtp_recv_stream_is_active(t->receiver.stream);
        rtc_rtp_send_stream_set_active(t->sender.stream, send);
        rtc_rtp_recv_stream_set_active(t->receiver.stream, receive);
        if (send && pc->runtime_connected) {
            rtc_rtp_sender_attach_logical(&t->sender, pc->runtime_transport);
            rtc_rtp_sender_arm_video(&t->sender);
#ifdef MRTC_ENABLE_TWCC
            if (pc->twcc_ext_id)
                rtc_rtp_sender_attach_twcc(&t->sender, pc->twcc_ext_id);
#endif
        }
        if (receive && !was_receiving && pc->on_track)
            pc->on_track(&t->receiver, pc->on_track_user);
    }
}

static void peer_try_connect(rtc_peer_connection_t *pc) {
    if (!pc->has_local_desc || !pc->has_remote_desc)
        return;
    if (pc->signaling_state != RTC_SIGNALING_STABLE)
        return;
    peer_apply_negotiated_media(pc);
    if (pc->runtime_connected && peer_maybe_start_sctp(pc) != RTC_OK) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }
    if (pc->connect_started && !pc->ice_restart_pending)
        return;

    pc->connect_started = true;
    pc->connect_deadline = rtc_time_ms() + 30000;
    pc->ice_restart_pending = false;
    (void)rtc_transport_set_ice_controlling(
        pc->runtime_transport, pc->remote_sdp.ice_lite || pc->local_desc.type == RTC_SDP_OFFER);

    peer_set_connection(pc, RTC_CONNECTION_CONNECTING);
    peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CHECKING);
    int rc = rtc_transport_start_ice(pc->runtime_transport);
    if (rc != RTC_OK && rc != RTC_ERR_INVALID) {
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_FAILED);
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }
    pc->runtime_connect_timer = rtc_worker_add_timer(peer_runtime_worker(pc), rtc_time_ms(),
                                                     peer_runtime_connect_timer, pc);
}

rtc_peer_connection_t *rtc_peer_connection_create(const rtc_config_t *config) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)calloc(1, sizeof(*pc));
    if (!pc)
        return NULL;

    pc->signaling_state = RTC_SIGNALING_STABLE;
    pc->ice_gathering_state = RTC_ICE_GATHERING_NEW;
    pc->ice_connection_state = RTC_ICE_CONNECTION_NEW;
    pc->connection_state = RTC_CONNECTION_NEW;
    pc->application_mid = -1;

    (void)config;

    int rc = RTC_OK;

    rc = peer_runtime_init(pc, config);
    if (rc != RTC_OK) {
        free(pc);
        return NULL;
    }
    if (rtc_dc_manager_init(&pc->dc_manager, peer_dc_send, pc) != RTC_OK) {
        peer_runtime_close(pc);
        free(pc);
        return NULL;
    }
    rtc_dc_manager_set_dispatch(&pc->dc_manager, peer_dispatch, pc);

    RTC_LOG_INFO("Peer connection created");
    return pc;
}

void rtc_peer_connection_close(rtc_peer_connection_t *pc) {
    if (!pc)
        return;
    if (pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    if (rtc_transport_on_worker_thread(pc->runtime_transport)) {
        RTC_LOG_ERR("Close/destroy must be scheduled by the application outside peer callbacks");
        return;
    }
    if (rtc_transport_invoke(pc->runtime_transport, peer_stop_on_worker, pc) != RTC_OK)
        return;

    peer_runtime_close(pc);

    rtc_media_session_close(&pc->media_session);
    rtc_sdp_close(&pc->local_sdp);
    rtc_sdp_close(&pc->remote_sdp);
    /* Destroy per-sender resources */
    for (int i = 0; i < pc->transceiver_count; i++) {
        rtc_rtp_transceiver_close_resources(&pc->transceivers[i]);
    }

    pc->connection_state = RTC_CONNECTION_CLOSED;
    pc->signaling_state = RTC_SIGNALING_CLOSED;

    RTC_LOG_INFO("Peer connection closed");
}

void rtc_peer_connection_destroy(rtc_peer_connection_t *pc) {
    if (!pc)
        return;
    /* Defensive: if the caller forgot to close, the transport thread is still
     * running and reads pc->* on packet receive — freeing now is a UAF. Close
     * first; close() is idempotent on CLOSED state. */
    if (pc->connection_state != RTC_CONNECTION_CLOSED) {
        RTC_LOG_WARN("rtc_peer_connection_destroy called without prior close");
        rtc_peer_connection_close(pc);
    }
    if (pc->connection_state != RTC_CONNECTION_CLOSED)
        return;
    free(pc);
}

static rtc_rtp_sender_t *peer_add_track(rtc_peer_connection_t *pc, rtc_kind_t kind,
                                        const rtc_codec_t *codec) {
    if (!pc || !codec)
        return NULL;
    if (pc->transceiver_count >= RTC_MAX_TRANSCEIVERS ||
        pc->transceiver_count + (pc->application_mid >= 0) >= SDP_MAX_MEDIA)
        return NULL;

    struct rtc_rtp_transceiver *t = &pc->transceivers[pc->transceiver_count];
    rtc_rtp_transceiver_init_slot(t, pc->next_mid++, kind, codec);
    if (!t->sender.stream || !t->receiver.stream) {
        rtc_rtp_transceiver_close_resources(t);
        pc->next_mid--;
        return NULL;
    }
    t->owner = pc;
    rtc_rtp_send_stream_set_active(t->sender.stream, !pc->connect_started);

    /* SSRC is fixed at creation, so register the streams with the session now. */
    rtc_media_session_add_sender(&pc->media_session, t->sender.stream);
    rtc_media_session_add_receiver(&pc->media_session, t->receiver.stream);

    pc->transceiver_count++;
    return &t->sender;
}

static rtc_rtp_transceiver_t *peer_add_transceiver(rtc_peer_connection_t *pc, rtc_kind_t kind,
                                                   const rtc_codec_t *codec,
                                                   const rtc_rtp_transceiver_init_t *init) {
    if (!pc || !codec)
        return NULL;
    if (pc->transceiver_count >= RTC_MAX_TRANSCEIVERS ||
        pc->transceiver_count + (pc->application_mid >= 0) >= SDP_MAX_MEDIA)
        return NULL;

    struct rtc_rtp_transceiver *t = &pc->transceivers[pc->transceiver_count];
    rtc_rtp_transceiver_init_slot(t, pc->next_mid++, kind, codec);
    if (!t->sender.stream || !t->receiver.stream) {
        rtc_rtp_transceiver_close_resources(t);
        pc->next_mid--;
        return NULL;
    }
    t->owner = pc;
    if (init)
        t->direction = init->direction;
    /* Sender active only for sendrecv/sendonly. */
    if (pc->connect_started || t->direction == RTC_DIR_RECVONLY || t->direction == RTC_DIR_INACTIVE)
        rtc_rtp_send_stream_set_active(t->sender.stream, false);
    rtc_media_session_add_sender(&pc->media_session, t->sender.stream);
    rtc_media_session_add_receiver(&pc->media_session, t->receiver.stream);

    pc->transceiver_count++;
    return (rtc_rtp_transceiver_t *)t;
}

static int peer_remove_track(rtc_peer_connection_t *pc, rtc_rtp_sender_t *sender) {
    if (!pc || !sender)
        return RTC_ERR_INVALID;

    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[i];
        if (&t->sender != sender)
            continue;
        rtc_rtp_send_stream_set_active(t->sender.stream, false);
        switch (t->direction) {
            case RTC_DIR_SENDRECV:
                t->direction = RTC_DIR_RECVONLY;
                break;
            case RTC_DIR_SENDONLY:
                t->direction = RTC_DIR_INACTIVE;
                break;
            default:
                break;
        }
        return RTC_OK;
    }
    return RTC_ERR_INVALID;
}

static int peer_get_transceivers(rtc_peer_connection_t *pc, rtc_rtp_transceiver_t **out,
                                 int *count) {
    if (!pc || !count)
        return RTC_ERR_INVALID;
    int n = 0;
    for (int i = 0; i < pc->transceiver_count && i < *count; i++) {
        if (out)
            out[i] = (rtc_rtp_transceiver_t *)&pc->transceivers[i];
        n++;
    }
    *count = n;
    return RTC_OK;
}

static int peer_create_offer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    if (pc->signaling_state != RTC_SIGNALING_STABLE)
        return RTC_ERR_INVALID;
    memset(desc, 0, sizeof(*desc));
    desc->type = RTC_SDP_OFFER;

    /* Build internal SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_OFFER;
    sdp.setup = RTC_SETUP_ACTPASS;
    int rc = peer_build_sdp(pc, &sdp);
    if (rc != RTC_OK) {
        rtc_sdp_close(&sdp);
        return rc;
    }

    rc = rtc_sdp_generate(&sdp);
    if (rc != RTC_OK)
        return rc;

    /* Copy to rtc_desc_t */
    memcpy(desc->sdp, sdp.raw, sdp.raw_len);
    desc->sdp_len = sdp.raw_len;

    /* Store internal SDP for later use (transfer ownership of candidates vec) */
    rtc_sdp_close(&pc->local_sdp);
    pc->local_sdp = sdp;

    return RTC_OK;
}

static int peer_create_answer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    if (pc->signaling_state != RTC_SIGNALING_HAVE_REMOTE_OFFER)
        return RTC_ERR_INVALID;
    memset(desc, 0, sizeof(*desc));
    desc->type = RTC_SDP_ANSWER;

    /* Build internal SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_ANSWER;
    sdp.setup = RTC_SETUP_ACTIVE;
    if (pc->runtime_connected) {
        rtc_dtls_parameters_t dtls;
        if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) != RTC_OK)
            return RTC_ERR_GENERIC;
        sdp.setup =
            dtls.role == RTC_TRANSPORT_DTLS_ROLE_CLIENT ? RTC_SETUP_ACTIVE : RTC_SETUP_PASSIVE;
    }
    int rc = peer_build_sdp(pc, &sdp);
    if (rc != RTC_OK) {
        rtc_sdp_close(&sdp);
        return rc;
    }

    rc = rtc_sdp_generate(&sdp);
    if (rc != RTC_OK)
        return rc;

    memcpy(desc->sdp, sdp.raw, sdp.raw_len);
    desc->sdp_len = sdp.raw_len;

    rtc_sdp_close(&pc->local_sdp);
    pc->local_sdp = sdp;

    return RTC_OK;
}

static int peer_set_local_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    if (desc->sdp_len == 0 || desc->sdp_len >= sizeof(desc->sdp) ||
        !((desc->type == RTC_SDP_OFFER && pc->signaling_state == RTC_SIGNALING_STABLE) ||
          (desc->type == RTC_SDP_ANSWER && pc->signaling_state == RTC_SIGNALING_HAVE_REMOTE_OFFER)))
        return RTC_ERR_INVALID;
    rtc_sdp_t parsed = {0};
    int rc = rtc_sdp_parse(&parsed, desc->sdp, desc->sdp_len);
    if (rc != RTC_OK) {
        rtc_sdp_close(&parsed);
        return rc;
    }
    rtc_ice_parameters_t local_ice;
    if (rtc_transport_get_ice_parameters(pc->runtime_transport, &local_ice) != RTC_OK ||
        strcmp(parsed.ice_ufrag, local_ice.username_fragment) != 0 ||
        strcmp(parsed.ice_pwd, local_ice.password) != 0 ||
        strcmp(parsed.fingerprint, pc->runtime_fingerprint) != 0) {
        rtc_sdp_close(&parsed);
        return RTC_ERR_SDP;
    }
    parsed.type = desc->type;
    rtc_sdp_close(&pc->local_sdp);
    pc->local_sdp = parsed;

    if (desc->type == RTC_SDP_OFFER) {
        pc->previous_local_desc = pc->local_desc;
        pc->had_previous_local_desc = pc->has_local_desc;
    }
    pc->local_desc = *desc;
    pc->has_local_desc = true;

    /* Update signaling state */
    if (desc->type == RTC_SDP_OFFER) {
        peer_set_signaling(pc, RTC_SIGNALING_HAVE_LOCAL_OFFER);
    } else if (desc->type == RTC_SDP_ANSWER) {
        if (pc->signaling_state == RTC_SIGNALING_HAVE_REMOTE_OFFER)
            peer_set_signaling(pc, RTC_SIGNALING_STABLE);
    }

    /* Fire ICE gathering callbacks for candidates already cached by the
     * shared listener. Later srflx candidates arrive through the listener
     * callback chain and are emitted as trickle candidates. */
    peer_set_ice_gathering(pc, RTC_ICE_GATHERING_GATHERING);
    peer_replay_listener_candidates(pc);
    size_t local_candidate_count = rtc_sdp_candidate_count(&pc->local_sdp);
    for (size_t i = 0; i < local_candidate_count; i++) {
        const rtc_ice_candidate_t *c = rtc_sdp_get_candidate(&pc->local_sdp, i);
        if (!c)
            continue;
        peer_emit_ice_candidate(pc, c);
    }
    if (rtc_listener_gathering_complete(peer_runtime_listener(pc)))
        peer_runtime_on_listener_done(pc);
    if (pc->relay_only && !pc->relay_candidate_emitted &&
        pc->relay_timer == RTC_WORKER_TIMER_INVALID) {
        pc->relay_deadline = rtc_time_ms() + 30000;
        peer_relay_timer(pc);
    }

    /* Try to start connection if both descriptions set */
    peer_try_connect(pc);

    return RTC_OK;
}

static bool peer_codec_name_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ac = (unsigned char)*a++, bc = (unsigned char)*b++;
        if (ac >= 'A' && ac <= 'Z')
            ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z')
            bc += 'a' - 'A';
        if (ac != bc)
            return false;
    }
    return *a == *b;
}

static bool peer_validate_remote_media(rtc_peer_connection_t *pc, const rtc_sdp_t *sdp,
                                       rtc_sdp_type_t type) {
    if ((type == RTC_SDP_ANSWER && sdp->media_count != pc->local_sdp.media_count) ||
        (type == RTC_SDP_OFFER && pc->has_remote_desc &&
         sdp->media_count < pc->remote_sdp.media_count))
        return false;
    unsigned applications = 0;
    for (int i = 0; i < sdp->media_count; i++) {
        const rtc_sdp_media_t *m = &sdp->media[i];
        for (int j = 0; j < i; j++)
            if (sdp->media[j].mid_index == m->mid_index ||
                (m->ssrc && sdp->media[j].ssrc == m->ssrc))
                return false;
        if (m->media_type == RTC_MEDIA_APPLICATION) {
            if (++applications > 1 || !m->sctp_port ||
                (type == RTC_SDP_ANSWER && m->mid_index != pc->application_mid) ||
                (pc->dc_manager.socket && m->sctp_port != pc->dc_manager.remote_port))
                return false;
            for (int j = 0; j < pc->transceiver_count; j++)
                if (pc->transceivers[j].mid_index == m->mid_index)
                    return false;
            continue;
        }
        bool found = false;
        if (m->payload_type < 0 || m->payload_type > 127 ||
            (m->payload_type >= 64 && m->payload_type <= 95))
            return false;
        if (!m->rejected && !m->rtcp_mux)
            return false;
        if (type == RTC_SDP_ANSWER &&
            (m->rtcp_feedback & (uint8_t)~pc->local_sdp.media[i].rtcp_feedback))
            return false;
        for (int j = 0; j < pc->transceiver_count; j++) {
            const struct rtc_rtp_transceiver *t = &pc->transceivers[j];
            if (t->mid_index != m->mid_index)
                continue;
            const char *name = strchr(t->sender.codec.mime_type, '/');
            if (!name || !peer_codec_name_equal(name + 1, m->codec_name) ||
                ((type != RTC_SDP_OFFER || pc->runtime_connected) &&
                 t->sender.codec.payload_type != m->payload_type) ||
                t->sender.codec.clock_rate != (uint32_t)m->clockrate ||
                (t->sender.kind == RTC_KIND_AUDIO) != (m->media_type == RTC_MEDIA_AUDIO))
                return false;
            found = true;
        }
        if (!found && type == RTC_SDP_ANSWER)
            return false;
    }
    return true;
}

static int peer_set_remote_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    /* Explicit impolite glare policy: reject competing offers without mutation. */
    if (desc->sdp_len == 0 || desc->sdp_len >= sizeof(desc->sdp) ||
        !((desc->type == RTC_SDP_OFFER && pc->signaling_state == RTC_SIGNALING_STABLE) ||
          (desc->type == RTC_SDP_ANSWER && pc->signaling_state == RTC_SIGNALING_HAVE_LOCAL_OFFER)))
        return RTC_ERR_INVALID;

    /* Parse the SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    int rc = rtc_sdp_parse(&sdp, desc->sdp, desc->sdp_len);
    if (rc != RTC_OK) {
        rtc_sdp_close(&sdp);
        return rc;
    }
    sdp.type = desc->type;
    if (!sdp.fingerprint[0] || !sdp.ice_ufrag[0] || !sdp.ice_pwd[0] ||
        (desc->type == RTC_SDP_ANSWER && sdp.setup == RTC_SETUP_ACTPASS) ||
        (pc->runtime_connected && strcmp(pc->remote_sdp.fingerprint, sdp.fingerprint) != 0) ||
        !peer_validate_remote_media(pc, &sdp, desc->type)) {
        rtc_sdp_close(&sdp);
        return RTC_ERR_SDP;
    }
    bool restart = pc->runtime_connected && (strcmp(pc->remote_sdp.ice_ufrag, sdp.ice_ufrag) != 0 ||
                                             strcmp(pc->remote_sdp.ice_pwd, sdp.ice_pwd) != 0);
    rc = peer_runtime_set_remote_desc(pc, &sdp);
    if (rc != RTC_OK) {
        rtc_sdp_close(&sdp);
        /* Protocol mutation failed: fail closed rather than retain ambiguous state. */
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return rc;
    }
    pc->remote_desc = *desc;
    pc->has_remote_desc = true;
    rtc_sdp_close(&pc->remote_sdp);
    pc->remote_sdp = sdp;

    if (restart) {
        pc->ice_restart_pending = true;
        if (pc->relay_only)
            pc->relay_candidate_emitted = false;
    }
    for (int i = 0; i < sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &sdp.media[i];
        if (m->mid_index >= pc->next_mid)
            pc->next_mid = m->mid_index + 1;
        if (m->media_type == RTC_MEDIA_APPLICATION) {
            pc->application_mid = m->mid_index;
            continue;
        }
        bool found = false;
        for (int j = 0; j < pc->transceiver_count; j++) {
            struct rtc_rtp_transceiver *t = &pc->transceivers[j];
            if (t->mid_index != m->mid_index)
                continue;
            found = true;
            if (!pc->runtime_connected && desc->type == RTC_SDP_OFFER) {
                if (rtc_rtp_send_stream_set_payload_type(t->sender.stream,
                                                         (uint8_t)m->payload_type) != RTC_OK ||
                    rtc_rtp_recv_stream_set_payload_type(t->receiver.stream,
                                                         (uint8_t)m->payload_type) != RTC_OK) {
                    peer_set_connection(pc, RTC_CONNECTION_FAILED);
                    return RTC_ERR_SDP;
                }
                t->sender.codec.payload_type = (uint8_t)m->payload_type;
                t->receiver.codec.payload_type = (uint8_t)m->payload_type;
            }
        }
        if (!found && desc->type == RTC_SDP_OFFER) {
            rtc_codec_t codec = {.payload_type = (uint8_t)m->payload_type,
                                 .clock_rate = (uint32_t)m->clockrate,
                                 .channels = m->channels};
            snprintf(codec.mime_type, sizeof(codec.mime_type), "%s/%s",
                     m->media_type == RTC_MEDIA_AUDIO ? "audio" : "video", m->codec_name);
            rtc_rtp_transceiver_init_t init = {.direction = RTC_DIR_RECVONLY};
            rtc_rtp_transceiver_t *t = peer_add_transceiver(
                pc, m->media_type == RTC_MEDIA_AUDIO ? RTC_KIND_AUDIO : RTC_KIND_VIDEO, &codec,
                &init);
            if (!t) {
                peer_set_connection(pc, RTC_CONNECTION_FAILED);
                return RTC_ERR_NOMEM;
            }
            t->mid_index = m->mid_index;
            snprintf(t->mid, sizeof(t->mid), "%d", m->mid_index);
        }
    }

    /* Update signaling state */
    if (desc->type == RTC_SDP_OFFER) {
        peer_set_signaling(pc, RTC_SIGNALING_HAVE_REMOTE_OFFER);
    } else if (desc->type == RTC_SDP_ANSWER) {
        if (pc->signaling_state == RTC_SIGNALING_HAVE_LOCAL_OFFER)
            peer_set_signaling(pc, RTC_SIGNALING_STABLE);
    }

    /* Eager SSRC → receive-stream binding from remote SDP. Walk parsed
     * m= sections; for each that carried a=ssrc, find the matching
     * transceiver by mid_index and bind its receive stream into the
     * transport RTP demux under that SSRC. The session payload-type
     * resolver remains as a defensive fallback for peers that omit
     * a=ssrc lines. */
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        for (int j = 0; j < pc->transceiver_count; j++) {
            struct rtc_rtp_transceiver *t = &pc->transceivers[j];
            if (t->mid_index == m->mid_index) {
                rtc_rtp_recv_stream_set_feedback(t->receiver.stream, m->rtcp_feedback);
                rtc_rtp_send_stream_set_remote_feedback(t->sender.stream, m->rtcp_feedback);
                if (m->ssrc)
                    rtc_media_session_bind_receiver(&pc->media_session, t->receiver.stream,
                                                    m->ssrc);
                break;
            }
        }
    }

    /* MID extmap negotiation: when the peer advertises the MID extension, route
     * bundled inbound RTP by MID (RFC 8843/8852). Tell each receive stream its
     * expected MID so the resolver can disambiguate same-payload-type streams. */
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        if (m->media_type != RTC_MEDIA_AUDIO && m->media_type != RTC_MEDIA_VIDEO)
            continue;
        uint8_t mid_id = rtc_sdp_media_find_extmap_id(m, RTC_EXT_URI_MID);
        if (mid_id != 0) {
            rtc_media_session_set_mid_ext_id(&pc->media_session, mid_id);
            break;
        }
    }
    for (int j = 0; j < pc->transceiver_count; j++) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[j];
        if (t->receiver.stream)
            rtc_rtp_recv_stream_set_mid(t->receiver.stream, t->mid);
    }

    /* Transport-CC extmap negotiation */
#ifdef MRTC_ENABLE_TWCC
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        if (m->media_type != RTC_MEDIA_AUDIO && m->media_type != RTC_MEDIA_VIDEO)
            continue;
        uint8_t id = (m->rtcp_feedback & RTC_SDP_FB_TRANSPORT_CC)
                         ? rtc_sdp_media_find_extmap_id(m, RTC_EXT_URI_TRANSPORT_CC)
                         : 0;
        if (id != 0) {
            uint32_t local_ssrc = 0;
            for (int k = 0; k < pc->transceiver_count; k++) {
                if (rtc_rtp_send_stream_is_active(pc->transceivers[k].sender.stream)) {
                    local_ssrc = rtc_rtp_send_stream_ssrc(pc->transceivers[k].sender.stream);
                    break;
                }
            }
            rc = rtc_transport_enable_twcc(pc->runtime_transport, id, local_ssrc);
            if (rc != RTC_OK) {
                peer_set_connection(pc, RTC_CONNECTION_FAILED);
                return rc;
            }
            pc->twcc_ext_id = id;
            RTC_LOG_INFO("Peer: transport-cc negotiated (ext id=%u)", (unsigned)id);
            break;
        }
    }
#endif

    /* Try to start connection if both descriptions set */
    peer_try_connect(pc);

    RTC_LOG_INFO("Remote description set (%zu candidates)",
                 rtc_sdp_candidate_count(&pc->remote_sdp));
    return RTC_OK;
}

const rtc_desc_t *rtc_peer_connection_local_desc(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_local_desc) ? &pc->local_desc : NULL;
}

const rtc_desc_t *rtc_peer_connection_remote_desc(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_remote_desc) ? &pc->remote_desc : NULL;
}

int rtc_peer_connection_add_ice_candidate(rtc_peer_connection_t *pc,
                                          const rtc_ice_candidate_desc_t *cand) {
    if (!pc)
        return RTC_ERR_INVALID;
    if (!cand || cand->candidate[0] == '\0')
        return RTC_OK;

    rtc_ice_candidate_t candidate;
    int rc = rtc_sdp_parse_candidate_line(cand->candidate, &candidate);
    if (rc != RTC_OK)
        return rc;

    return peer_runtime_add_remote_candidate(pc, &candidate);
}

static int peer_restart_ice(rtc_peer_connection_t *pc) {
    if (!pc)
        return RTC_ERR_INVALID;
    if (pc->signaling_state != RTC_SIGNALING_STABLE)
        return RTC_ERR_INVALID;
    if (!pc->runtime_transport)
        return RTC_ERR_INVALID;
    int rc = rtc_transport_restart_ice(pc->runtime_transport);
    if (rc == RTC_OK) {
        pc->ice_restart_pending = true;
        if (pc->relay_only)
            pc->relay_candidate_emitted = false;
    }
    return rc;
}

static int peer_rollback(rtc_peer_connection_t *pc) {
    if (pc->signaling_state != RTC_SIGNALING_HAVE_LOCAL_OFFER)
        return RTC_ERR_INVALID;
    rtc_sdp_t previous = {0};
    if (pc->had_previous_local_desc) {
        int rc =
            rtc_sdp_parse(&previous, pc->previous_local_desc.sdp, pc->previous_local_desc.sdp_len);
        if (rc != RTC_OK) {
            rtc_sdp_close(&previous);
            return rc;
        }
        previous.type = pc->previous_local_desc.type;
    }
    rtc_sdp_close(&pc->local_sdp);
    pc->local_sdp = previous;
    pc->local_desc = pc->previous_local_desc;
    pc->has_local_desc = pc->had_previous_local_desc;
    peer_set_signaling(pc, RTC_SIGNALING_STABLE);
    return RTC_OK;
}

/* Send callback for data channel manager (sends via DTLS) */
static int peer_dc_send(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->runtime_transport && pc->runtime_connected)
        return rtc_transport_send_data(pc->runtime_transport, data, len);
    return RTC_ERR_INVALID;
}

static rtc_data_channel_t *peer_create_data_channel(rtc_peer_connection_t *pc, const char *label,
                                                    const rtc_data_channel_init_t *opts) {
    if (!pc)
        return NULL;

    /* Initialize dc_manager if not already done */
    if (!pc->dc_manager.send_fn) {
        rtc_dc_manager_init(&pc->dc_manager, peer_dc_send, pc);
    }

    return rtc_dc_manager_create_channel(&pc->dc_manager, label, opts);
}

typedef struct {
    rtc_peer_connection_t *pc;
    rtc_kind_t kind;
    const rtc_codec_t *codec;
    const rtc_rtp_transceiver_init_t *init;
    rtc_rtp_sender_t *sender;
    rtc_desc_t *out;
    const rtc_desc_t *desc;
    const char *label;
    const rtc_data_channel_init_t *options;
    rtc_rtp_transceiver_t **transceivers;
    int *count;
    rtc_stats_report_t *report;
    void *result;
    int status;
} peer_call_t;

static void peer_call_add_track(void *arg) {
    peer_call_t *call = arg;
    call->result = peer_add_track(call->pc, call->kind, call->codec);
}
static void peer_call_add_transceiver(void *arg) {
    peer_call_t *call = arg;
    call->result = peer_add_transceiver(call->pc, call->kind, call->codec, call->init);
}
static void peer_call_remove_track(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_remove_track(call->pc, call->sender);
}
static void peer_call_offer(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_create_offer(call->pc, call->out);
}
static void peer_call_answer(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_create_answer(call->pc, call->out);
}
static void peer_call_local(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_set_local_desc(call->pc, call->desc);
}
static void peer_call_remote(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_set_remote_desc(call->pc, call->desc);
}
static void peer_call_restart(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_restart_ice(call->pc);
}
static void peer_call_rollback(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_rollback(call->pc);
}
static void peer_call_channel(void *arg) {
    peer_call_t *call = arg;
    call->result = peer_create_data_channel(call->pc, call->label, call->options);
}
static void peer_call_transceivers(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_get_transceivers(call->pc, call->transceivers, call->count);
}
static void peer_call_stats(void *arg) {
    peer_call_t *call = arg;
    call->status = peer_get_stats(call->pc, call->report);
}
static int peer_run(peer_call_t *call, void (*fn)(void *)) {
    if (!call->pc || call->pc->connection_state == RTC_CONNECTION_CLOSED ||
        call->pc->connection_state == RTC_CONNECTION_FAILED)
        return RTC_ERR_INVALID;
    int rc = peer_dispatch(call->pc, fn, call);
    return rc == RTC_OK ? call->status : rc;
}
rtc_rtp_sender_t *rtc_peer_connection_add_track(rtc_peer_connection_t *pc, rtc_kind_t kind,
                                                const rtc_codec_t *codec) {
    peer_call_t call = {.pc = pc, .kind = kind, .codec = codec};
    return peer_run(&call, peer_call_add_track) == RTC_OK ? call.result : NULL;
}
rtc_rtp_transceiver_t *rtc_peer_connection_add_transceiver(rtc_peer_connection_t *pc,
                                                           rtc_kind_t kind,
                                                           const rtc_codec_t *codec,
                                                           const rtc_rtp_transceiver_init_t *init) {
    peer_call_t call = {.pc = pc, .kind = kind, .codec = codec, .init = init};
    return peer_run(&call, peer_call_add_transceiver) == RTC_OK ? call.result : NULL;
}
int rtc_peer_connection_remove_track(rtc_peer_connection_t *pc, rtc_rtp_sender_t *sender) {
    peer_call_t call = {.pc = pc, .sender = sender};
    return peer_run(&call, peer_call_remove_track);
}
int rtc_peer_connection_create_offer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    peer_call_t call = {.pc = pc, .out = desc};
    return peer_run(&call, peer_call_offer);
}
int rtc_peer_connection_create_answer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    peer_call_t call = {.pc = pc, .out = desc};
    return peer_run(&call, peer_call_answer);
}
int rtc_peer_connection_set_local_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc) {
    peer_call_t call = {.pc = pc, .desc = desc};
    return peer_run(&call, peer_call_local);
}
int rtc_peer_connection_set_remote_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc) {
    peer_call_t call = {.pc = pc, .desc = desc};
    return peer_run(&call, peer_call_remote);
}
int rtc_peer_connection_restart_ice(rtc_peer_connection_t *pc) {
    peer_call_t call = {.pc = pc};
    return peer_run(&call, peer_call_restart);
}
int rtc_peer_connection_rollback(rtc_peer_connection_t *pc) {
    peer_call_t call = {.pc = pc};
    return peer_run(&call, peer_call_rollback);
}
rtc_data_channel_t *rtc_peer_connection_create_data_channel(
    rtc_peer_connection_t *pc, const char *label, const rtc_data_channel_init_t *options) {
    peer_call_t call = {.pc = pc, .label = label, .options = options};
    return peer_run(&call, peer_call_channel) == RTC_OK ? call.result : NULL;
}
int rtc_peer_connection_get_transceivers(rtc_peer_connection_t *pc, rtc_rtp_transceiver_t **out,
                                         int *count) {
    peer_call_t call = {.pc = pc, .transceivers = out, .count = count};
    return peer_run(&call, peer_call_transceivers);
}
int rtc_peer_connection_get_stats(const rtc_peer_connection_t *pc, rtc_stats_report_t *report) {
    peer_call_t call = {.pc = (rtc_peer_connection_t *)pc, .report = report};
    return peer_run(&call, peer_call_stats);
}

typedef struct {
    rtc_peer_connection_t *pc;
    int kind;
    void *user;
    rtc_on_signaling_state_fn signaling;
    rtc_on_ice_gathering_state_fn gathering;
    rtc_on_ice_connection_state_fn ice;
    rtc_on_connection_state_fn connection;
    rtc_on_ice_candidate_fn candidate;
    rtc_on_track_fn track;
    rtc_on_data_channel_fn channel;
} peer_callback_call_t;

static void peer_callback_apply(void *arg) {
    peer_callback_call_t *call = arg;
    rtc_peer_connection_t *pc = call->pc;
    switch (call->kind) {
        case 0:
            pc->on_signaling_state = call->signaling;
            pc->on_signaling_state_user = call->user;
            break;
        case 1:
            pc->on_ice_gathering_state = call->gathering;
            pc->on_ice_gathering_state_user = call->user;
            break;
        case 2:
            pc->on_ice_connection_state = call->ice;
            pc->on_ice_connection_state_user = call->user;
            break;
        case 3:
            pc->on_connection_state = call->connection;
            pc->on_connection_state_user = call->user;
            break;
        case 4:
            pc->on_ice_candidate = call->candidate;
            pc->on_ice_candidate_user = call->user;
            break;
        case 5:
            pc->on_track = call->track;
            pc->on_track_user = call->user;
            break;
        case 6:
            pc->on_data_channel = call->channel;
            pc->on_data_channel_user = call->user;
            rtc_dc_manager_on_channel(&pc->dc_manager, call->channel, call->user);
            break;
    }
}

static void peer_callback_dispatch(peer_callback_call_t *call) {
    if (call->pc && call->pc->connection_state != RTC_CONNECTION_CLOSED)
        (void)peer_dispatch(call->pc, peer_callback_apply, call);
}

void rtc_peer_connection_on_signaling_state(rtc_peer_connection_t *pc, rtc_on_signaling_state_fn fn,
                                            void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 0, .signaling = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_ice_gathering_state(rtc_peer_connection_t *pc,
                                                rtc_on_ice_gathering_state_fn fn, void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 1, .gathering = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_ice_connection_state(rtc_peer_connection_t *pc,
                                                 rtc_on_ice_connection_state_fn fn, void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 2, .ice = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_connection_state(rtc_peer_connection_t *pc,
                                             rtc_on_connection_state_fn fn, void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 3, .connection = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_ice_candidate(rtc_peer_connection_t *pc, rtc_on_ice_candidate_fn fn,
                                          void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 4, .candidate = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_track(rtc_peer_connection_t *pc, rtc_on_track_fn fn, void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 5, .track = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_data_channel(rtc_peer_connection_t *pc, rtc_on_data_channel_fn fn,
                                         void *user) {
    peer_callback_call_t call = {.pc = pc, .kind = 6, .channel = fn, .user = user};
    peer_callback_dispatch(&call);
}

void rtc_peer_connection_on_bitrate_estimate(rtc_peer_connection_t *pc,
                                             rtc_on_bitrate_estimate_fn fn, void *user) {
    if (!pc)
        return;
#ifdef MRTC_ENABLE_TWCC
    rtc_transport_on_bitrate_estimate(pc->runtime_transport, fn, user);
#else
    (void)fn;
    (void)user;
#endif
}

rtc_signaling_state_t rtc_peer_connection_signaling_state(const rtc_peer_connection_t *pc) {
    return pc ? pc->signaling_state : RTC_SIGNALING_CLOSED;
}

rtc_ice_gathering_state_t rtc_peer_connection_ice_gathering_state(const rtc_peer_connection_t *pc) {
    return pc ? pc->ice_gathering_state : RTC_ICE_GATHERING_NEW;
}

rtc_ice_connection_state_t rtc_peer_connection_ice_connection_state(
    const rtc_peer_connection_t *pc) {
    return pc ? pc->ice_connection_state : RTC_ICE_CONNECTION_CLOSED;
}

rtc_connection_state_t rtc_peer_connection_connection_state(const rtc_peer_connection_t *pc) {
    return pc ? pc->connection_state : RTC_CONNECTION_CLOSED;
}

const char *rtc_peer_connection_local_fingerprint(const rtc_peer_connection_t *pc) {
    if (pc && pc->runtime_fingerprint[0] != '\0')
        return pc->runtime_fingerprint;
    return "";
}

const char *rtc_peer_connection_remote_fingerprint(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_remote_desc) ? pc->remote_sdp.fingerprint : "";
}

bool rtc_peer_connection_can_trickle_ice_candidates(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_remote_desc) ? pc->remote_sdp.ice_options_trickle : false;
}

static int peer_get_stats(const rtc_peer_connection_t *pc, rtc_stats_report_t *report) {
    if (!pc || !report)
        return RTC_ERR_INVALID;
    memset(report, 0, sizeof(*report));
    report->transceiver_count = pc->transceiver_count;
    for (int i = 0; i < pc->transceiver_count; i++) {
        const struct rtc_rtp_transceiver *t = &pc->transceivers[i];
        rtc_transceiver_stats_t *s = &report->transceivers[i];

        size_t mlen = strnlen(t->mid, sizeof(s->mid) - 1);
        memcpy(s->mid, t->mid, mlen);
        s->mid[mlen] = '\0';
        s->kind = t->sender.kind;
        s->dir = t->direction;

        if (rtc_rtp_send_stream_is_active(t->sender.stream)) {
            const rtc_rtcp_stats_t *sender_stats = rtc_rtp_send_stream_stats(t->sender.stream);
            s->out_ssrc = rtc_rtp_send_stream_ssrc(t->sender.stream);
            s->out_packets_sent = sender_stats->packets_sent;
            s->out_bytes_sent = sender_stats->octets_sent;
        }
        if (rtc_rtp_recv_stream_is_active(t->receiver.stream)) {
            const rtc_rtcp_stats_t *receiver_stats = rtc_rtp_recv_stream_stats(t->receiver.stream);
            s->in_ssrc = rtc_rtp_recv_stream_ssrc(t->receiver.stream);
            s->in_source_generation = rtc_rtp_recv_stream_source_generation(t->receiver.stream);
            s->in_packets_received = receiver_stats->packets_received;
            s->in_packets_lost = receiver_stats->packets_lost;
            s->in_jitter_q16 = receiver_stats->jitter;
        }
    }
    return RTC_OK;
}
