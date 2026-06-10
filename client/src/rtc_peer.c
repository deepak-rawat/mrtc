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
static void peer_runtime_connect_timer(void *user);
static void peer_runtime_on_rtp(const rtc_rtp_packet_t *pkt, void *user);
static void peer_runtime_on_rtcp(const uint8_t *data, size_t len, void *user);
static void peer_runtime_on_data(const uint8_t *data, size_t len, void *user);

static rtc_worker_t *peer_runtime_worker(rtc_peer_connection_t *pc) {
    return rtc_client_runtime_worker(pc->runtime);
}

static rtc_listener_t *peer_runtime_listener(rtc_peer_connection_t *pc) {
    return rtc_client_runtime_listener(pc->runtime);
}

static int peer_runtime_init(rtc_peer_connection_t *pc) {
    pc->runtime = rtc_client_runtime_acquire();
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
                                                         });
    if (!pc->runtime_transport) {
        peer_runtime_close(pc);
        return RTC_ERR_GENERIC;
    }
    rtc_transport_on_rtp(pc->runtime_transport, peer_runtime_on_rtp, pc);
    rtc_transport_on_rtcp(pc->runtime_transport, peer_runtime_on_rtcp, pc);
    rtc_transport_on_data(pc->runtime_transport, peer_runtime_on_data, pc);
    pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    pc->runtime_rtcp_timer = RTC_WORKER_TIMER_INVALID;
#ifdef MRTC_ENABLE_TWCC
    pc->runtime_twcc_fb_timer = RTC_WORKER_TIMER_INVALID;
#endif

    rtc_dtls_parameters_t dtls;
    if (rtc_transport_get_dtls_parameters(pc->runtime_transport, &dtls) == RTC_OK)
        memcpy(pc->runtime_fingerprint, dtls.fingerprint, sizeof(pc->runtime_fingerprint));

    return RTC_OK;
}

static void peer_runtime_close(rtc_peer_connection_t *pc) {
    rtc_worker_t *worker = peer_runtime_worker(pc);
    if (worker && pc->runtime_connect_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_connect_timer);
        pc->runtime_connect_timer = RTC_WORKER_TIMER_INVALID;
    }
    if (worker && pc->runtime_rtcp_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_rtcp_timer);
        pc->runtime_rtcp_timer = RTC_WORKER_TIMER_INVALID;
    }
#ifdef MRTC_ENABLE_TWCC
    if (worker && pc->runtime_twcc_fb_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(worker, pc->runtime_twcc_fb_timer);
        pc->runtime_twcc_fb_timer = RTC_WORKER_TIMER_INVALID;
    }
#endif
    if (pc->runtime_transport) {
        rtc_transport_destroy(pc->runtime_transport);
        pc->runtime_transport = NULL;
    }
    if (pc->runtime) {
        rtc_client_runtime_release(pc->runtime);
        pc->runtime = NULL;
    }
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

    rtc_transport_candidate_t candidates[ICE_MAX_CANDIDATES];
    int count = ICE_MAX_CANDIDATES;
    rc = rtc_listener_get_candidates(listener, candidates, &count);
    if (rc != RTC_OK)
        return rc;

    for (int i = 0; i < count; i++) {
        rtc_ice_candidate_t c;
        memset(&c, 0, sizeof(c));
        c.type = ICE_CANDIDATE_HOST;
        c.component = 1;
        c.priority = 2130706431u;
        size_t flen = strlen(candidates[i].foundation);
        if (flen >= sizeof(c.foundation))
            flen = sizeof(c.foundation) - 1;
        memcpy(c.foundation, candidates[i].foundation, flen);
        c.foundation[flen] = '\0';
        rc = rtc_addr_from_string(&c.addr, candidates[i].address, candidates[i].port);
        if (rc != RTC_OK)
            return rc;
        rtc_sdp_add_candidate(sdp, &c);
    }
    return RTC_OK;
}

static int peer_runtime_add_remote_candidate(rtc_peer_connection_t *pc,
                                             const rtc_ice_candidate_t *candidate) {
    if (!pc->runtime_transport || !candidate)
        return RTC_ERR_INVALID;

    char ip[64];
    uint16_t port = 0;
    if (rtc_addr_to_string(&candidate->addr, ip, sizeof(ip), &port) != RTC_OK)
        return RTC_ERR_INVALID;

    rtc_transport_candidate_t tc;
    memset(&tc, 0, sizeof(tc));
    size_t flen = strlen(candidate->foundation);
    if (flen >= sizeof(tc.foundation))
        flen = sizeof(tc.foundation) - 1;
    memcpy(tc.foundation, candidate->foundation, flen);
    tc.foundation[flen] = '\0';
    size_t ilen = strlen(ip);
    if (ilen >= sizeof(tc.address))
        ilen = sizeof(tc.address) - 1;
    memcpy(tc.address, ip, ilen);
    tc.address[ilen] = '\0';
    memcpy(tc.protocol, "udp", sizeof("udp"));
    tc.port = port;
    tc.type = RTC_TRANSPORT_CANDIDATE_HOST;
    if (candidate->type == ICE_CANDIDATE_SRFLX)
        tc.type = RTC_TRANSPORT_CANDIDATE_SRFLX;
    else if (candidate->type == ICE_CANDIDATE_RELAY)
        tc.type = RTC_TRANSPORT_CANDIDATE_RELAY;
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
        if (!s->active)
            continue;
        rtc_rtp_sender_attach_logical(s, pc->runtime_transport);
#ifdef MRTC_ENABLE_TWCC
        if (pc->twcc_ext_id_send != 0) {
            rtc_rtp_sender_attach_twcc(s, &pc->twcc_sender, pc->twcc_ext_id_send);
            if (pc->twcc_local_ssrc == 0)
                pc->twcc_local_ssrc = s->rtp_session.ssrc;
        }
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
#ifdef MRTC_ENABLE_TWCC
    if (pc->twcc_ext_id_recv != 0) {
        pc->runtime_twcc_fb_timer = rtc_worker_add_timer(
            peer_runtime_worker(pc), rtc_time_ms() + TWCC_FB_INTERVAL_MS, peer_twcc_fb_timer, pc);
    }
#endif

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

static void peer_runtime_on_rtp(const rtc_rtp_packet_t *pkt, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    peer_handle_plain_rtp(pc, pkt);
}

static void peer_runtime_on_rtcp(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    peer_handle_plain_rtcp(pc, data, len);
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

    rc = peer_runtime_init(pc);
    if (rc != RTC_OK) {
        free(pc);
        return NULL;
    }

    /* Initialize SSRC → receiver lookup map */
    if (rtc_u32_map_init(&pc->recv_map) != RTC_OK) {
        peer_runtime_close(pc);
        free(pc);
        return NULL;
    }

    /* Initialize SSRC → sender lookup map */
    if (rtc_u32_map_init(&pc->send_map) != RTC_OK) {
        rtc_u32_map_free(&pc->recv_map);
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
    rtc_u32_map_free(&pc->recv_map);
    rtc_u32_map_free(&pc->send_map);
    rtc_sdp_close(&pc->local_sdp);
    rtc_sdp_close(&pc->remote_sdp);
#ifdef MRTC_ENABLE_RATE_CONTROL
    if (pc->rate_ctrl) {
        rtc_rate_control_destroy(pc->rate_ctrl);
        pc->rate_ctrl = NULL;
    }
#endif
    /* Destroy per-sender resources */
    for (int i = 0; i < pc->transceiver_count; i++) {
        rtc_rtp_transceiver_close_resources(&pc->transceivers[i]);
    }

#ifdef MRTC_ENABLE_TWCC
    if (pc->bwe) {
        rtc_bwe_destroy(pc->bwe);
        pc->bwe = NULL;
    }
#endif

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

    /* Eager SSRC → sender map: SSRC is known now, register so RTCP RR
     * report blocks naming this SSRC can be demuxed back to this sender. */
    rtc_u32_map_set(&pc->send_map, t->sender.rtp_session.ssrc, &t->sender);

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
        t->sender.active = false;
    rtc_u32_map_set(&pc->send_map, t->sender.rtp_session.ssrc, &t->sender);

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
        t->sender.active = false;
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

    /* Fire ICE gathering callbacks for already-gathered candidates.
     * Each candidate must be associated with an m-line via its mid. With
     * BUNDLE the remote will dedupe to a single ICE transport; without
     * BUNDLE each m-line needs its own copy. Emit one event per
     * (candidate × transceiver). If there are no transceivers (e.g.,
     * data-channel-only), emit once with the default mid "0". */
    peer_set_ice_gathering(pc, RTC_ICE_GATHERING_GATHERING);
    size_t local_candidate_count = rtc_sdp_candidate_count(&pc->local_sdp);
    for (size_t i = 0; i < local_candidate_count; i++) {
        if (!pc->on_ice_candidate)
            continue;

        const rtc_ice_candidate_t *c = rtc_sdp_get_candidate(&pc->local_sdp, i);
        if (!c)
            continue;
        char ip[64];
        uint16_t port;
        rtc_addr_to_string(&c->addr, ip, sizeof(ip), &port);

        const char *ctype = "host";
        if (c->type == ICE_CANDIDATE_SRFLX)
            ctype = "srflx";

        char cand_line[sizeof(((rtc_ice_candidate_desc_t *)0)->candidate)];
        snprintf(cand_line, sizeof(cand_line), "candidate:%s 1 udp %u %s %u typ %s", c->foundation,
                 c->priority, ip, port, ctype);

        int n_emit = pc->transceiver_count > 0 ? pc->transceiver_count : 1;
        for (int j = 0; j < n_emit; j++) {
            rtc_ice_candidate_desc_t cand_desc;
            memset(&cand_desc, 0, sizeof(cand_desc));
            memcpy(cand_desc.candidate, cand_line, sizeof(cand_desc.candidate));

            if (pc->transceiver_count > 0) {
                const struct rtc_rtp_transceiver *t = &pc->transceivers[j];
                size_t mlen = strnlen(t->mid, sizeof(cand_desc.mid) - 1);
                memcpy(cand_desc.mid, t->mid, mlen);
                cand_desc.mid[mlen] = '\0';
                cand_desc.mid_index = t->mid_index;
            } else {
                cand_desc.mid[0] = '0';
                cand_desc.mid[1] = '\0';
                cand_desc.mid_index = 0;
            }

            pc->on_ice_candidate(&cand_desc, pc->on_ice_candidate_user);
        }
    }
    /* End-of-candidates */
    peer_set_ice_gathering(pc, RTC_ICE_GATHERING_COMPLETE);
    if (pc->on_ice_candidate)
        pc->on_ice_candidate(NULL, pc->on_ice_candidate_user);

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
        if (!r->active) {
            rtc_rtp_receiver_activate(r);
            if (pc->on_track)
                pc->on_track((rtc_rtp_receiver_t *)r, pc->on_track_user);
        }
    }

    /* Eager SSRC → receiver map population from remote SDP. Walk parsed
     * m= sections; for each that carried a=ssrc, find the matching
     * transceiver by mid_index and register its receiver under that SSRC.
     * Lazy first-packet population in peer_handle_plain_rtp remains as a
     * defensive fallback for peers that omit a=ssrc lines. */
    for (int i = 0; i < pc->remote_sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &pc->remote_sdp.media[i];
        if (m->ssrc == 0)
            continue;
        for (int j = 0; j < pc->transceiver_count; j++) {
            struct rtc_rtp_transceiver *t = &pc->transceivers[j];
            if (t->mid_index == m->mid_index) {
                t->receiver.ssrc = m->ssrc;
                rtc_u32_map_set(&pc->recv_map, m->ssrc, &t->receiver);
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
            pc->twcc_ext_id_send = id;
            pc->twcc_ext_id_recv = id;
            if (pc->twcc_remote_ssrc == 0)
                pc->twcc_remote_ssrc = m->ssrc;
            rtc_twcc_sender_init(&pc->twcc_sender);
            rtc_twcc_receiver_init(&pc->twcc_receiver);
            if (!pc->bwe) {
                rtc_bwe_config_t bcfg = {
                    .initial_bps = 500000,
                    .min_bps = 100000,
                    .max_bps = 4000000,
                };
                pc->bwe = rtc_bwe_create(&bcfg);
            }
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
    pc->on_bitrate_estimate = fn;
    pc->on_bitrate_estimate_user = user;
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

        if (t->sender.active) {
            s->out_ssrc = t->sender.rtp_session.ssrc;
            s->out_packets_sent = t->sender.rtcp_stats.packets_sent;
            s->out_bytes_sent = t->sender.rtcp_stats.octets_sent;
        }
        if (t->receiver.active) {
            s->in_ssrc = t->receiver.ssrc;
            s->in_packets_received = t->receiver.rtcp_stats.packets_received;
            s->in_packets_lost = t->receiver.rtcp_stats.packets_lost;
            s->in_jitter_q16 = t->receiver.rtcp_stats.jitter;
        }
    }
    return RTC_OK;
}
