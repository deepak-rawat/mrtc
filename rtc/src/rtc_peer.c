/*
 * rtc_peer.c - Peer Connection: control plane.
 *
 * Lifecycle, SDP, connect choreography, timers, public API.
 * Packet handling (RTP/RTCP dispatch) lives in rtc_peer_packets.c.
 */
#include "rtc_peer_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ---- Forward declarations ---- */
static int peer_dc_send(const uint8_t *data, size_t len, void *user);

/* ---- BWE bitrate callback ---- */

/* ---- DTLS send callback ---- */

static int peer_dtls_send(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    return rtc_transport_send_to_remote(&pc->transport, data, len);
}

/* ---- Complete connection (called on transport thread after DTLS connects) ---- */

void peer_complete_connection(rtc_peer_connection_t *pc) {
    /* Export SRTP keys */
    int rc = rtc_dtls_export_srtp_keys(&pc->dtls);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("Peer: SRTP key export failed");
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    /* Initialize SRTP contexts */
    if (pc->dtls.role == RTC_DTLS_ROLE_CLIENT) {
        rc = rtc_srtp_init(&pc->srtp_send, pc->dtls.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                           pc->dtls.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN);
        if (rc != RTC_OK) {
            peer_set_connection(pc, RTC_CONNECTION_FAILED);
            return;
        }
        rc = rtc_srtp_init(&pc->srtp_recv, pc->dtls.srtp_server_key, RTC_SRTP_MASTER_KEY_LEN,
                           pc->dtls.srtp_server_salt, RTC_SRTP_MASTER_SALT_LEN);
    } else {
        rc = rtc_srtp_init(&pc->srtp_send, pc->dtls.srtp_server_key, RTC_SRTP_MASTER_KEY_LEN,
                           pc->dtls.srtp_server_salt, RTC_SRTP_MASTER_SALT_LEN);
        if (rc != RTC_OK) {
            peer_set_connection(pc, RTC_CONNECTION_FAILED);
            return;
        }
        rc = rtc_srtp_init(&pc->srtp_recv, pc->dtls.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                           pc->dtls.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN);
    }
    if (rc != RTC_OK) {
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    /* Wire up senders to SRTP + transport (+ TWCC if negotiated), then arm
     * per-sender video resources (NACK buffer + rate controller). */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_sender *s = &pc->transceivers[i].sender;
        if (!s->active)
            continue;
        rtc_rtp_sender_attach(s, &pc->srtp_send, &pc->transport);
#ifdef MRTC_ENABLE_TWCC
        if (pc->twcc_ext_id_send != 0) {
            rtc_rtp_sender_attach_twcc(s, &pc->twcc_sender, pc->twcc_ext_id_send);
            if (pc->twcc_local_ssrc == 0)
                pc->twcc_local_ssrc = s->rtp_session.ssrc;
        }
#endif
        rtc_rtp_sender_arm_video(s);
    }

    /* Initialize data channel manager (if not already) and notify DTLS connected */
    if (!pc->dc_manager.send_fn) {
        rtc_dc_manager_init(&pc->dc_manager, peer_dc_send, pc);
    }
    rtc_dc_manager_on_dtls_connected(&pc->dc_manager);

    /* Wire up the on_data_channel callback */
    if (pc->on_data_channel) {
        rtc_dc_manager_on_channel(&pc->dc_manager, pc->on_data_channel, pc->on_data_channel_user);
    }

    peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CONNECTED);
    peer_set_connection(pc, RTC_CONNECTION_CONNECTED);
    RTC_LOG_INFO("Peer: connected! Ready to send/receive media.");

    /* Start periodic RTCP send timer */
    pc->rtcp_timer_id = rtc_transport_add_timer(&pc->transport, rtc_time_ms() + RTCP_INTERVAL_MS,
                                                peer_rtcp_timer, pc);

    /* Start TWCC feedback timer if negotiated for inbound. */
#ifdef MRTC_ENABLE_TWCC
    if (pc->twcc_ext_id_recv != 0) {
        rtc_twcc_receiver_init(&pc->twcc_receiver);
        pc->twcc_fb_timer_id =
            rtc_transport_add_timer(&pc->transport, rtc_time_ms() + 100, peer_twcc_fb_timer, pc);
    }

    /* Bind the BWE bitrate callback through a per-peer trampoline. */
    if (pc->bwe)
        rtc_bwe_on_bitrate_change(pc->bwe, peer_on_bwe_bitrate, pc);
#endif
}

/* ---- DTLS retransmission timer (fires on transport thread) ---- */

static void peer_dtls_retransmit_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->dtls.state != RTC_DTLS_STATE_CONNECTING)
        return;

    rtc_dtls_retransmit(&pc->dtls);

    /* Re-schedule until handshake completes */
    rtc_transport_add_timer(&pc->transport, rtc_time_ms() + 1000, peer_dtls_retransmit_timer, pc);
}

/* ---- Connect timer (fires on transport thread) ---- */

#define PEER_ICE_CHECK_INTERVAL_MS 250

/* Forward decl: timer that polls ICE state until connected/failed and then
 * advances to DTLS. Replaces the old synchronous select+recvfrom in
 * rtc_ice_connect that stole non-STUN packets from the transport classifier. */
static void peer_ice_check_timer(void *user);

/* Kick off DTLS handshake — extracted from the old peer_connect_timer body. */
static void peer_start_dtls(rtc_peer_connection_t *pc) {
    RTC_LOG_INFO("Peer: starting DTLS handshake (role=%s)...",
                 pc->dtls.role == RTC_DTLS_ROLE_CLIENT ? "client" : "server");
    if (pc->dtls.role == RTC_DTLS_ROLE_CLIENT) {
        int rc = rtc_dtls_handshake(&pc->dtls);
        if (rc != RTC_OK && pc->dtls.state == RTC_DTLS_STATE_FAILED) {
            peer_set_connection(pc, RTC_CONNECTION_FAILED);
            return;
        }
    } else {
        /* Server: just set state to CONNECTING and wait for client's ClientHello */
        pc->dtls.state = RTC_DTLS_STATE_CONNECTING;
    }

    /* Schedule DTLS retransmission timer (fires every 1s until handshake completes) */
    rtc_transport_add_timer(&pc->transport, rtc_time_ms() + 1000, peer_dtls_retransmit_timer, pc);
}

static void peer_ice_check_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;

    if (pc->connection_state == RTC_CONNECTION_CLOSED ||
        pc->connection_state == RTC_CONNECTION_FAILED)
        return;

    /* Success path — ICE has flipped to CONNECTED via rtc_ice_handle_stun
     * on the transport thread (same thread, no race). */
    if (pc->ice.state == ICE_STATE_CONNECTED) {
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CONNECTED);
        peer_start_dtls(pc);
        return;
    }

    /* Failure path */
    if (pc->ice.state == ICE_STATE_FAILED || rtc_ice_check_deadline_passed(&pc->ice)) {
        pc->ice.state = ICE_STATE_FAILED;
        RTC_LOG_ERR("Peer: ICE connectivity checks failed");
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_FAILED);
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    /* Still checking — send another binding request and re-arm. */
    rtc_ice_send_check(&pc->ice);
    rtc_transport_add_timer(&pc->transport, rtc_time_ms() + PEER_ICE_CHECK_INTERVAL_MS,
                            peer_ice_check_timer, pc);
}

static void peer_connect_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;

    peer_set_connection(pc, RTC_CONNECTION_CONNECTING);
    peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CHECKING);

    /* Kick off ICE connectivity checks asynchronously. rtc_ice_connect sends
     * the first STUN binding request and returns; binding responses arrive
     * via the transport's recv callback (which routes to rtc_ice_handle_stun),
     * so the transport thread can resume normal packet classification. */
    RTC_LOG_INFO("Peer: starting ICE connectivity checks...");
    int rc = rtc_ice_connect(&pc->ice);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("Peer: ICE connect failed to start (rc=%d)", rc);
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_FAILED);
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    /* Poll ICE state every PEER_ICE_CHECK_INTERVAL_MS until CONNECTED or timeout. */
    rtc_transport_add_timer(&pc->transport, rtc_time_ms() + PEER_ICE_CHECK_INTERVAL_MS,
                            peer_ice_check_timer, pc);
}

/* ---- Helper: parse STUN server URL like "stun:host:port" or just IP ---- */

static void parse_stun_url(const char *url, char *host, size_t host_len, uint16_t *port) {
    *port = 3478;
    const char *p = url;
    if (strncmp(p, "stun:", 5) == 0)
        p += 5;
    if (strncmp(p, "//", 2) == 0)
        p += 2;

    const char *colon = strchr(p, ':');
    if (colon) {
        size_t len = (size_t)(colon - p);
        if (len >= host_len)
            len = host_len - 1;
        memcpy(host, p, len);
        host[len] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        size_t len = strlen(p);
        if (len >= host_len)
            len = host_len - 1;
        memcpy(host, p, len);
        host[len] = '\0';
    }
}

/* ---- Helper: fill rtc_sdp_t from transceivers + ICE + DTLS ---- */

static int peer_build_sdp(rtc_peer_connection_t *pc, rtc_sdp_t *sdp) {
    /* ICE credentials */
    memcpy(sdp->ice_ufrag, pc->ice.ufrag, ICE_UFRAG_LEN);
    memcpy(sdp->ice_pwd, pc->ice.pwd, ICE_PWD_LEN);

    /* DTLS fingerprint */
    memcpy(sdp->fingerprint, pc->dtls.local_fingerprint, sizeof(sdp->fingerprint));

    /* Candidates */
    for (int i = 0; i < pc->ice.local_candidate_count; i++) {
        rtc_sdp_add_candidate(sdp, &pc->ice.local_candidates[i]);
    }

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

/* ---- Helper: try to start connection ---- */

static void peer_try_connect(rtc_peer_connection_t *pc) {
    if (!pc->has_local_desc || !pc->has_remote_desc)
        return;
    if (pc->connect_started)
        return;
    if (pc->signaling_state != RTC_SIGNALING_STABLE)
        return;

    pc->connect_started = true;

    /* Schedule connection on transport thread (0-delay timer) */
    rtc_transport_add_timer(&pc->transport, rtc_time_ms(), peer_connect_timer, pc);
}

/* ---- Public API ---- */

rtc_peer_connection_t *rtc_peer_connection_create(const rtc_config_t *config) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)calloc(1, sizeof(*pc));
    if (!pc)
        return NULL;

    pc->signaling_state = RTC_SIGNALING_STABLE;
    pc->ice_gathering_state = RTC_ICE_GATHERING_NEW;
    pc->ice_connection_state = RTC_ICE_CONNECTION_NEW;
    pc->connection_state = RTC_CONNECTION_NEW;

    /* Allocate DTLS app-data scratch buffer (64 KiB = max DTLS record payload). */
    pc->app_buf_cap = 65536;
    pc->app_buf = (uint8_t *)malloc(pc->app_buf_cap);
    if (!pc->app_buf) {
        free(pc);
        return NULL;
    }

    /* Extract STUN server from config */
    if (config && config->ice_server_count > 0 && config->ice_servers[0].url_count > 0 &&
        config->ice_servers[0].urls[0]) {
        parse_stun_url(config->ice_servers[0].urls[0], pc->stun_server, sizeof(pc->stun_server),
                       &pc->stun_port);
    }

    /* Initialize transport (socket + thread). */
    int rc = rtc_transport_init(&pc->transport, peer_transport_recv, pc);
    if (rc != RTC_OK) {
        free(pc->app_buf);
        free(pc);
        return NULL;
    }

    /* Initialize SSRC → receiver lookup map */
    if (rtc_u32_map_init(&pc->recv_map) != RTC_OK) {
        rtc_transport_close(&pc->transport);
        free(pc->app_buf);
        free(pc);
        return NULL;
    }

    /* Initialize SSRC → sender lookup map */
    if (rtc_u32_map_init(&pc->send_map) != RTC_OK) {
        rtc_u32_map_free(&pc->recv_map);
        rtc_transport_close(&pc->transport);
        free(pc->app_buf);
        free(pc);
        return NULL;
    }

    /* Initialize ICE agent (borrows transport) */
    rc = rtc_ice_init(&pc->ice, &pc->transport, pc->stun_server[0] ? pc->stun_server : NULL,
                      pc->stun_port);
    if (rc != RTC_OK) {
        rtc_transport_close(&pc->transport);
        free(pc->app_buf);
        free(pc);
        return NULL;
    }

    /* Generate DTLS certificate (needed for fingerprint in SDP) */
    rc = rtc_dtls_init(&pc->dtls, RTC_DTLS_ROLE_CLIENT, peer_dtls_send, pc);
    if (rc != RTC_OK) {
        rtc_transport_close(&pc->transport);
        free(pc->app_buf);
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

    /* Close the transport FIRST. This joins the I/O thread, after which no
     * recv callback or timer can fire. Everything below this point is
     * therefore safe to tear down without racing the transport thread. */
    rtc_transport_close(&pc->transport);

    rtc_dc_manager_close(&pc->dc_manager);
    rtc_srtp_close(&pc->srtp_send);
    rtc_srtp_close(&pc->srtp_recv);
    rtc_dtls_close(&pc->dtls);
    rtc_ice_close(&pc->ice);
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
    free(pc->app_buf);
    free(pc);
}

/* ---- Track management ---- */

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

/* ---- SDP offer/answer ---- */

int rtc_peer_connection_create_offer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    memset(desc, 0, sizeof(*desc));
    desc->type = RTC_SDP_OFFER;

    /* Gather ICE candidates (synchronous) */
    int rc = rtc_ice_gather(&pc->ice);
    if (rc != RTC_OK)
        return rc;

    /* Build internal SDP */
    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_OFFER;
    sdp.setup = RTC_SETUP_ACTPASS;
    peer_build_sdp(pc, &sdp);

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

int rtc_peer_connection_create_answer(rtc_peer_connection_t *pc, rtc_desc_t *desc) {
    if (!pc || !desc)
        return RTC_ERR_INVALID;
    memset(desc, 0, sizeof(*desc));
    desc->type = RTC_SDP_ANSWER;

    /* Gather if not already done */
    if (pc->ice.local_candidate_count == 0) {
        int rc = rtc_ice_gather(&pc->ice);
        if (rc != RTC_OK)
            return rc;
    }

    /* Answerer is ICE-controlled */
    pc->ice.controlling = false;

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
     * transport thread owns pc->ice / pc->dtls / pc->transceivers, and any
     * further mutation from the app thread would race. */
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
    for (int i = 0; i < pc->ice.local_candidate_count; i++) {
        if (!pc->on_ice_candidate)
            continue;

        rtc_ice_candidate_t *c = &pc->ice.local_candidates[i];
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

    /* Set remote ICE credentials */
    rc = rtc_ice_set_remote_credentials(&pc->ice, sdp.ice_ufrag, sdp.ice_pwd);
    if (rc != RTC_OK)
        return rc;

    /* Add remote candidates */
    size_t ncand = rtc_sdp_candidate_count(&sdp);
    for (size_t i = 0; i < ncand; i++) {
        const rtc_ice_candidate_t *c = rtc_sdp_get_candidate(&sdp, i);
        rc = rtc_ice_add_remote_candidate(&pc->ice, c);
        if (rc != RTC_OK)
            return rc;
    }

    /* DTLS role negotiation */
    if (pc->local_sdp.type == RTC_SDP_OFFER && sdp.setup == RTC_SETUP_ACTIVE) {
        rtc_dtls_close(&pc->dtls);
        rc = rtc_dtls_init(&pc->dtls, RTC_DTLS_ROLE_SERVER, peer_dtls_send, pc);
        if (rc != RTC_OK)
            return rc;
        RTC_LOG_INFO("Peer: DTLS role → server (remote chose active)");
    }

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
     * Lazy first-packet population in peer_transport_recv remains as a
     * defensive fallback for peers that omit a=ssrc lines. */
    for (int i = 0; i < sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &sdp.media[i];
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
    for (int i = 0; i < sdp.media_count; i++) {
        const rtc_sdp_media_t *m = &sdp.media[i];
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

    RTC_LOG_INFO("Remote description set (%zu candidates)", rtc_sdp_candidate_count(&sdp));
    return RTC_OK;
}

const rtc_desc_t *rtc_peer_connection_local_desc(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_local_desc) ? &pc->local_desc : NULL;
}

const rtc_desc_t *rtc_peer_connection_remote_desc(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_remote_desc) ? &pc->remote_desc : NULL;
}

/* ---- Trickle ICE ---- */

int rtc_peer_connection_add_ice_candidate(rtc_peer_connection_t *pc,
                                          const rtc_ice_candidate_desc_t *cand) {
    (void)pc;
    (void)cand;
    /* TODO: parse candidate string and add to ICE agent */
    return RTC_ERR_GENERIC;
}

int rtc_peer_connection_restart_ice(rtc_peer_connection_t *pc) {
    if (!pc)
        return RTC_ERR_INVALID;
    if (pc->connect_started)
        return RTC_ERR_INVALID;
    return rtc_ice_restart_credentials(&pc->ice);
}

/* ---- Data channels ---- */

/* Send callback for data channel manager (sends via DTLS) */
static int peer_dc_send(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->dtls.state != RTC_DTLS_STATE_CONNECTED)
        return RTC_ERR_INVALID;

    /* Write data into SSL for DTLS encryption, then flush output BIO */
    int written = SSL_write(pc->dtls.ssl, data, (int)len);
    if (written <= 0)
        return RTC_ERR_SSL;

    /* Flush the output BIO */
    uint8_t buf[2048];
    int pending;
    while ((pending = BIO_read(pc->dtls.wbio, buf, sizeof(buf))) > 0) {
        rtc_transport_send_to_remote(&pc->transport, buf, (size_t)pending);
    }

    return RTC_OK;
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

/* ---- Event callbacks ---- */

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

/* ---- State getters ---- */

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

/* ---- Identity / capability getters ---- */

const char *rtc_peer_connection_local_fingerprint(const rtc_peer_connection_t *pc) {
    return pc ? pc->dtls.local_fingerprint : "";
}

const char *rtc_peer_connection_remote_fingerprint(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_remote_desc) ? pc->remote_sdp.fingerprint : "";
}

bool rtc_peer_connection_can_trickle_ice_candidates(const rtc_peer_connection_t *pc) {
    return (pc && pc->has_remote_desc) ? pc->remote_sdp.ice_options_trickle : false;
}

/* ---- Stats (getStats) ---- */

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
