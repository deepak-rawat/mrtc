/*
 * Peer Connection: control plane.
 *
 * Lifecycle, SDP, connect choreography, timers, public API.
 * Packet handling (RTP/RTCP dispatch) lives in rtc_peer_packets.c.
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
static void peer_runtime_on_rtcp(const uint8_t *data, size_t len, void *user);
static void peer_runtime_on_data(const uint8_t *data, size_t len, void *user);

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
    else if (strncmp(host, "stuns:", 6) == 0)
        host += 6;
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

    int rc = rtc_client_runtime_register_peer(pc->runtime, peer_runtime_on_listener_candidate,
                                              peer_runtime_on_listener_done, pc);
    if (rc != RTC_OK) {
        peer_runtime_close(pc);
        return rc;
    }
    pc->runtime_registered = true;

    pc->runtime_transport = rtc_transport_create(worker, &(rtc_transport_config_t){
                                                             .listener = listener,
                                                             .ice_mode = RTC_ICE_MODE_FULL,
                                                         });
    if (!pc->runtime_transport) {
        peer_runtime_close(pc);
        return RTC_ERR_GENERIC;
    }
    rtc_transport_set_rtp_router(pc->runtime_transport, peer_rtp_sink, peer_rtp_resolve, pc);
    rtc_transport_on_rtcp(pc->runtime_transport, peer_runtime_on_rtcp, pc);
    rtc_transport_on_data(pc->runtime_transport, peer_runtime_on_data, pc);
    pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    pc->runtime_rtcp_timer = RTC_WORKER_TIMER_INVALID;

    rtc_dtls_parameters_t dtls;
    if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) == RTC_OK)
        memcpy(pc->runtime_fingerprint, dtls.fingerprint, sizeof(pc->runtime_fingerprint));

    return RTC_OK;
}

static void peer_runtime_close(rtc_peer_connection_t *pc) {
    rtc_worker_t *worker = pc->runtime ? peer_runtime_worker(pc) : NULL;
    if (pc->runtime && pc->runtime_registered) {
        rtc_client_runtime_unregister_peer(pc->runtime, pc);
        pc->runtime_registered = false;
    }
    if (worker && pc->runtime_connect_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_connect_timer);
        pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (worker && pc->runtime_rtcp_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_rtcp_timer);
        pc->runtime_rtcp_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (pc->runtime_transport) {
        rtc_transport_destroy(pc->runtime_transport);
        pc->runtime_transport = NULL;
    }
    if (pc->runtime) {
        rtc_client_runtime_release(pc->runtime);
        pc->runtime = NULL;
    }
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
    if (!pc || !cand || !pc->has_local_desc || pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    (void)peer_add_local_transport_candidate(pc, cand, 65535, true);
}

static void peer_runtime_on_listener_done(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (!pc || !pc->has_local_desc || pc->connection_state == RTC_CONNECTION_CLOSED)
        return;
    if (pc->ice_gathering_state == RTC_ICE_GATHERING_COMPLETE)
        return;
    peer_set_ice_gathering(pc, RTC_ICE_GATHERING_COMPLETE);
    if (pc->on_ice_candidate)
        pc->on_ice_candidate(NULL, pc->on_ice_candidate_user);
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

static void peer_runtime_set_remote_desc(rtc_peer_connection_t *pc, const rtc_sdp_t *sdp) {
    if (!pc->runtime_transport || !sdp)
        return;

    rtc_ice_parameters_t ice;
    memset(&ice, 0, sizeof(ice));
    memcpy(ice.username_fragment, sdp->ice_ufrag, sizeof(ice.username_fragment));
    memcpy(ice.password, sdp->ice_pwd, sizeof(ice.password));
    ice.mode = RTC_ICE_MODE_FULL;
    (void)rtc_transport_set_remote_ice_parameters(pc->runtime_transport, &ice);

    size_t ncand = rtc_sdp_candidate_count(sdp);
    for (size_t i = 0; i < ncand; i++) {
        (void)peer_runtime_add_remote_candidate(pc, rtc_sdp_get_candidate(sdp, i));
    }

    if (pc->local_sdp.type == RTC_SDP_OFFER && sdp->setup == RTC_SETUP_ACTIVE) {
        (void)rtc_transport_set_dtls_role(pc->runtime_transport, RTC_TRANSPORT_DTLS_ROLE_SERVER);
        rtc_dtls_parameters_t dtls;
        if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) == RTC_OK)
            memcpy(pc->runtime_fingerprint, dtls.fingerprint, sizeof(pc->runtime_fingerprint));
    } else if (pc->local_sdp.type == RTC_SDP_ANSWER || sdp->setup == RTC_SETUP_ACTPASS) {
        (void)rtc_transport_set_dtls_role(pc->runtime_transport, RTC_TRANSPORT_DTLS_ROLE_CLIENT);
        rtc_dtls_parameters_t dtls;
        if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) == RTC_OK)
            memcpy(pc->runtime_fingerprint, dtls.fingerprint, sizeof(pc->runtime_fingerprint));
    }
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
            rtc_rtp_sender_attach_twcc(s, rtc_transport_twcc_sender(pc->runtime_transport),
                                       pc->twcc_ext_id);
#endif
        rtc_rtp_sender_arm_video(s);
    }

    if (!pc->dc_manager.send_fn)
        rtc_dc_manager_init(&pc->dc_manager, peer_dc_send, pc);
    rtc_dc_manager_on_dtls_connected(&pc->dc_manager);
    if (pc->on_data_channel)
        rtc_dc_manager_on_channel(&pc->dc_manager, pc->on_data_channel, pc->on_data_channel_user);

    pc->runtime_rtcp_timer = rtc_worker_add_timer(
        peer_runtime_worker(pc), rtc_time_ms() + RTCP_INTERVAL_MS, peer_rtcp_timer, pc);

    peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CONNECTED);
    peer_set_connection(pc, RTC_CONNECTION_CONNECTED);
}

static void peer_runtime_connect_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    if (pc->connection_state == RTC_CONNECTION_CLOSED ||
        pc->connection_state == RTC_CONNECTION_FAILED || pc->runtime_connected)
        return;

    rtc_transport_stats_t stats;
    if (rtc_transport_get_stats(pc->runtime_transport, &stats) != RTC_OK) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    if (!stats.selected_tuple_valid) {
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
        peer_runtime_complete_connection(pc);
        return;
    }

    pc->runtime_connect_timer = rtc_worker_add_timer(peer_runtime_worker(pc), rtc_time_ms() + 50,
                                                     peer_runtime_connect_timer, pc);
}

static void peer_runtime_on_rtcp(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    rtc_rtcp_router_handle(&pc->rtcp_router, data, len);
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
        sdp->media_count++;
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

static void peer_try_connect(rtc_peer_connection_t *pc) {
    if (!pc->has_local_desc || !pc->has_remote_desc)
        return;
    if (pc->connect_started)
        return;
    if (pc->signaling_state != RTC_SIGNALING_STABLE)
        return;

    pc->connect_started = true;

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

    (void)config;

    int rc = RTC_OK;

    rc = peer_runtime_init(pc, config);
    if (rc != RTC_OK) {
        free(pc);
        return NULL;
    }

    /* Initialize the RTCP feedback router (resolves SR recipients via the
     * transport RTP demux; routes RR/NACK/PLI to send streams). */
    if (rtc_rtcp_router_init(&pc->rtcp_router, pc->runtime_transport) != RTC_OK) {
        peer_runtime_close(pc);
        free(pc);
        return NULL;
    }
    RTC_LOG_INFO("Peer connection created");
    return pc;
}

void rtc_peer_connection_close(rtc_peer_connection_t *pc) {
    if (!pc)
        return;
    if (pc->connection_state == RTC_CONNECTION_CLOSED)
        return;

    peer_runtime_close(pc);

    rtc_dc_manager_close(&pc->dc_manager);
    rtc_rtcp_router_close(&pc->rtcp_router);
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
    free(pc);
}

rtc_rtp_sender_t *rtc_peer_connection_add_track(rtc_peer_connection_t *pc, rtc_kind_t kind,
                                                const rtc_codec_t *codec) {
    if (!pc || !codec)
        return NULL;
    if (pc->transceiver_count >= RTC_MAX_TRANSCEIVERS)
        return NULL;

    struct rtc_rtp_transceiver *t = &pc->transceivers[pc->transceiver_count];
    rtc_rtp_transceiver_init_slot(t, pc->transceiver_count, kind, codec);

    /* SSRC is fixed at creation, so register for RTCP feedback now. */
    rtc_rtcp_router_add_sender(&pc->rtcp_router, rtc_rtp_send_stream_ssrc(t->sender.stream),
                               t->sender.stream);

    pc->transceiver_count++;
    return &t->sender;
}

rtc_rtp_transceiver_t *rtc_peer_connection_add_transceiver(rtc_peer_connection_t *pc,
                                                           rtc_kind_t kind,
                                                           const rtc_codec_t *codec,
                                                           const rtc_rtp_transceiver_init_t *init) {
    if (!pc || !codec)
        return NULL;
    if (pc->transceiver_count >= RTC_MAX_TRANSCEIVERS)
        return NULL;
    if (pc->connect_started)
        return NULL;

    struct rtc_rtp_transceiver *t = &pc->transceivers[pc->transceiver_count];
    rtc_rtp_transceiver_init_slot(t, pc->transceiver_count, kind, codec);
    if (init)
        t->direction = init->direction;
    /* Sender active only for sendrecv/sendonly. */
    if (t->direction == RTC_DIR_RECVONLY || t->direction == RTC_DIR_INACTIVE)
        rtc_rtp_send_stream_set_active(t->sender.stream, false);
    rtc_rtcp_router_add_sender(&pc->rtcp_router, rtc_rtp_send_stream_ssrc(t->sender.stream),
                               t->sender.stream);

    pc->transceiver_count++;
    return (rtc_rtp_transceiver_t *)t;
}

int rtc_peer_connection_remove_track(rtc_peer_connection_t *pc, rtc_rtp_sender_t *sender) {
    if (!pc || !sender)
        return RTC_ERR_INVALID;
    if (pc->connect_started)
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

int rtc_peer_connection_get_transceivers(rtc_peer_connection_t *pc, rtc_rtp_transceiver_t **out,
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

int rtc_peer_connection_create_offer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    memset(desc, 0, sizeof(*desc));
    desc->type = RTC_SDP_OFFER;

    /* Build internal SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_OFFER;
    sdp.setup = RTC_SETUP_ACTPASS;
    peer_build_sdp(pc, &sdp);

    int rc = rtc_sdp_generate(&sdp);
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

int rtc_peer_connection_create_answer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    memset(desc, 0, sizeof(*desc));
    desc->type = RTC_SDP_ANSWER;

    /* Build internal SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_ANSWER;
    sdp.setup = RTC_SETUP_ACTIVE;
    peer_build_sdp(pc, &sdp);

    int rc = rtc_sdp_generate(&sdp);
    if (rc != RTC_OK)
        return rc;

    memcpy(desc->sdp, sdp.raw, sdp.raw_len);
    desc->sdp_len = sdp.raw_len;

    rtc_sdp_close(&pc->local_sdp);
    pc->local_sdp = sdp;

    return RTC_OK;
}

int rtc_peer_connection_set_local_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    /* Renegotiation is not supported: once the connection has started, the
     * runtime owns transport state and transceivers, and any further mutation
     * from the app thread would race. */
    if (pc->connect_started)
        return RTC_ERR_INVALID;

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

    /* Try to start connection if both descriptions set */
    peer_try_connect(pc);

    return RTC_OK;
}

int rtc_peer_connection_set_remote_desc(rtc_peer_connection_t *pc, const rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    /* Renegotiation is not supported — see rtc_peer_connection_set_local_desc. */
    if (pc->connect_started)
        return RTC_ERR_INVALID;

    pc->remote_desc = *desc;
    pc->has_remote_desc = true;

    /* Parse the SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    int rc = rtc_sdp_parse(&sdp, desc->sdp, desc->sdp_len);
    if (rc != RTC_OK) {
        rtc_sdp_close(&sdp);
        return rc;
    }
    rtc_sdp_close(&pc->remote_sdp);
    pc->remote_sdp = sdp;

    peer_runtime_set_remote_desc(pc, &pc->remote_sdp);

    /* Update signaling state */
    if (desc->type == RTC_SDP_OFFER) {
        peer_set_signaling(pc, RTC_SIGNALING_HAVE_REMOTE_OFFER);
    } else if (desc->type == RTC_SDP_ANSWER) {
        if (pc->signaling_state == RTC_SIGNALING_HAVE_LOCAL_OFFER)
            peer_set_signaling(pc, RTC_SIGNALING_STABLE);
    }

    /* Activate receivers and fire on_track */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_receiver *r = &pc->transceivers[i].receiver;
        if (!rtc_rtp_recv_stream_is_active(r->stream)) {
            rtc_rtp_receiver_activate(r);
            if (pc->on_track)
                pc->on_track((rtc_rtp_receiver_t *)r, pc->on_track_user);
        }
    }

    /* Eager SSRC → receive-stream binding from remote SDP. Walk parsed
     * m= sections; for each that carried a=ssrc, find the matching
     * transceiver by mid_index and bind its receive stream into the
     * transport RTP demux under that SSRC. The payload-type resolver
     * (peer_rtp_resolve) remains as a defensive fallback for peers that
     * omit a=ssrc lines. */
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        if (m->ssrc == 0)
            continue;
        for (int j = 0; j < pc->transceiver_count; j++) {
            struct rtc_rtp_transceiver *t = &pc->transceivers[j];
            if (t->mid_index == m->mid_index) {
                rtc_rtp_recv_stream_set_ssrc(t->receiver.stream, m->ssrc);
                rtc_transport_bind_rtp(pc->runtime_transport, m->ssrc, t->receiver.stream);
                break;
            }
        }
    }

    /* Transport-CC extmap negotiation */
#ifdef MRTC_ENABLE_TWCC
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        if (m->media_type != RTC_MEDIA_AUDIO && m->media_type != RTC_MEDIA_VIDEO)
            continue;
        uint8_t id = rtc_sdp_media_find_extmap_id(m, RTC_EXT_URI_TRANSPORT_CC);
        if (id != 0) {
            pc->twcc_ext_id = id;
            uint32_t local_ssrc = 0;
            for (int k = 0; k < pc->transceiver_count; k++) {
                if (rtc_rtp_send_stream_is_active(pc->transceivers[k].sender.stream)) {
                    local_ssrc = rtc_rtp_send_stream_ssrc(pc->transceivers[k].sender.stream);
                    break;
                }
            }
            rtc_transport_enable_twcc(pc->runtime_transport, id, local_ssrc, NULL);
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

int rtc_peer_connection_restart_ice(rtc_peer_connection_t *pc) {
    if (!pc)
        return RTC_ERR_INVALID;
    if (pc->connect_started)
        return RTC_ERR_INVALID;
    if (!pc->runtime_transport)
        return RTC_ERR_INVALID;
    return rtc_transport_restart_ice(pc->runtime_transport);
}

/* Send callback for data channel manager (sends via DTLS) */
static int peer_dc_send(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->runtime_transport && pc->runtime_connected)
        return rtc_transport_send_data(pc->runtime_transport, data, len);
    return RTC_ERR_INVALID;
}

rtc_data_channel_t *rtc_peer_connection_create_data_channel(rtc_peer_connection_t *pc,
                                                            const char *label,
                                                            const rtc_data_channel_init_t *opts) {
    if (!pc)
        return NULL;

    /* Initialize dc_manager if not already done */
    if (!pc->dc_manager.send_fn) {
        rtc_dc_manager_init(&pc->dc_manager, peer_dc_send, pc);
    }

    return rtc_dc_manager_create_channel(&pc->dc_manager, label, opts);
}

void rtc_peer_connection_on_signaling_state(rtc_peer_connection_t *pc, rtc_on_signaling_state_fn fn,
                                            void *user) {
    if (!pc)
        return;
    pc->on_signaling_state = fn;
    pc->on_signaling_state_user = user;
}

void rtc_peer_connection_on_ice_gathering_state(rtc_peer_connection_t *pc,
                                                rtc_on_ice_gathering_state_fn fn, void *user) {
    if (!pc)
        return;
    pc->on_ice_gathering_state = fn;
    pc->on_ice_gathering_state_user = user;
}

void rtc_peer_connection_on_ice_connection_state(rtc_peer_connection_t *pc,
                                                 rtc_on_ice_connection_state_fn fn, void *user) {
    if (!pc)
        return;
    pc->on_ice_connection_state = fn;
    pc->on_ice_connection_state_user = user;
}

void rtc_peer_connection_on_connection_state(rtc_peer_connection_t *pc,
                                             rtc_on_connection_state_fn fn, void *user) {
    if (!pc)
        return;
    pc->on_connection_state = fn;
    pc->on_connection_state_user = user;
}

void rtc_peer_connection_on_ice_candidate(rtc_peer_connection_t *pc, rtc_on_ice_candidate_fn fn,
                                          void *user) {
    if (!pc)
        return;
    pc->on_ice_candidate = fn;
    pc->on_ice_candidate_user = user;
}

void rtc_peer_connection_on_track(rtc_peer_connection_t *pc, rtc_on_track_fn fn, void *user) {
    if (!pc)
        return;
    pc->on_track = fn;
    pc->on_track_user = user;
}

void rtc_peer_connection_on_data_channel(rtc_peer_connection_t *pc, rtc_on_data_channel_fn fn,
                                         void *user) {
    if (!pc)
        return;
    pc->on_data_channel = fn;
    pc->on_data_channel_user = user;
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

int rtc_peer_connection_get_stats(const rtc_peer_connection_t *pc, rtc_stats_report_t *report) {
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
            s->in_packets_received = receiver_stats->packets_received;
            s->in_packets_lost = receiver_stats->packets_lost;
            s->in_jitter_q16 = receiver_stats->jitter;
        }
    }
    return RTC_OK;
}
