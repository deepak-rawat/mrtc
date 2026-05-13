/*
 * rtc_peer.c - Peer Connection: WebRTC-style API.
 *
 * Opaque, heap-allocated struct. Owns a transport layer with a background
 * thread. All protocol state (ICE, DTLS, SRTP) is driven on the transport
 * thread — no mutexes on protocol state.
 *
 * Threading model:
 *   - Main thread: create, add_track, create_offer/answer,
 *     set_local/remote_desc, sender_send, close, destroy
 *   - Transport thread: ICE connect, DTLS handshake, SRTP setup,
 *     packet recv, callback delivery
 *
 * After both local and remote descriptions are set, a zero-delay timer
 * fires on the transport thread to run: ICE connect → DTLS → SRTP.
 * No explicit connect() call is needed.
 *
 * The SRTP send context is initialized on the transport thread but used
 * from the main thread (via rtc_rtp_sender_send). This is safe because
 * the main thread observes RTC_CONNECTION_CONNECTED before calling
 * sender_send, providing a happens-before guarantee via the _Atomic
 * connection_state (sequentially-consistent on plain read/write).
 */
#include "rtc/rtc_peer.h"
#include "rtc/rtc_u32_map.h"
#include "rtc_rtp.h"
#include "rtc_rtcp.h"
#include "rtc_rate_control.h"
#include "rtc_transport.h"
#include "rtc_ice.h"
#include "rtc_dtls.h"
#include "rtc_srtp.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ---- Internal transceiver struct (must match rtc_track.c layout) ---- */

struct rtc_rtp_sender {
    rtc_codec_t codec;
    rtc_kind_t kind;
    rtc_rtp_session_t rtp_session;
    rtc_srtp_ctx_t *srtp;
    void *transport;
    rtc_rtcp_stats_t rtcp_stats;
    rtc_rate_controller_t *rate_ctrl; /* borrowed from peer, set after CONNECTED */
    bool active;
};

struct rtc_rtp_receiver {
    rtc_codec_t codec;
    rtc_kind_t kind;
    rtc_on_frame_fn on_frame;
    void *on_frame_user;
    uint32_t ssrc;
    rtc_rtcp_stats_t rtcp_stats;
    bool active;
};

struct rtc_rtp_transceiver {
    struct rtc_rtp_sender sender;
    struct rtc_rtp_receiver receiver;
    rtc_direction_t direction;
    char mid[32];
    int mid_index;
    bool used;
};

/* ---- Peer connection internal struct ---- */

struct rtc_peer_connection {
    /* Transport (owns socket and I/O thread) */
    rtc_transport_t transport;

    /* Protocol components (touched only on transport thread after connect) */
    rtc_ice_agent_t ice;
    rtc_dtls_transport_t dtls;
    rtc_srtp_ctx_t srtp_send; /* after init: main thread only */
    rtc_srtp_ctx_t srtp_recv; /* transport thread only */

    /* Data channel manager */
    rtc_dc_manager_t dc_manager;

    /* Transceivers (main thread before connect, transport thread after) */
    struct rtc_rtp_transceiver transceivers[RTC_MAX_TRANSCEIVERS];
    int transceiver_count;

    /* Descriptions */
    rtc_desc_t local_desc;
    rtc_desc_t remote_desc;
    bool has_local_desc;
    bool has_remote_desc;
    rtc_sdp_t local_sdp; /* parsed internal SDP */
    rtc_sdp_t remote_sdp;

    /* States: _Atomic so cross-thread reads/writes carry full memory ordering.
     * Written on transport thread, read from any thread.  Plain C11 reads
     * and assignments on _Atomic types are seq_cst, which is sufficient to
     * publish protocol state (e.g. srtp_send) alongside CONNECTED. */
    _Atomic rtc_signaling_state_t signaling_state;
    _Atomic rtc_ice_gathering_state_t ice_gathering_state;
    _Atomic rtc_ice_connection_state_t ice_connection_state;
    _Atomic rtc_connection_state_t connection_state;

    /* Callbacks (set from main thread before connect, read on transport thread) */
    rtc_on_signaling_state_fn on_signaling_state;
    void *on_signaling_state_user;
    rtc_on_ice_gathering_state_fn on_ice_gathering_state;
    void *on_ice_gathering_state_user;
    rtc_on_ice_connection_state_fn on_ice_connection_state;
    void *on_ice_connection_state_user;
    rtc_on_connection_state_fn on_connection_state;
    void *on_connection_state_user;
    rtc_on_ice_candidate_fn on_ice_candidate;
    void *on_ice_candidate_user;
    rtc_on_track_fn on_track;
    void *on_track_user;
    rtc_on_data_channel_fn on_data_channel;
    void *on_data_channel_user;

    /* Config */
    char stun_server[64];
    uint16_t stun_port;

    /* Connection started flag */
    bool connect_started;

    /* RTCP send timer */
    rtc_timer_id_t rtcp_timer_id;

    /* Rate controller (created on connection, fed by RTCP RR) */
    rtc_rate_controller_t *rate_ctrl;

    /* Fast SSRC → receiver lookup (populated on first RTP packet per stream).
     * Hot path: O(1) average via rtc_u32_map (Fibonacci hash + linear probing). */
    rtc_u32_map_t recv_map;
};

/* ---- Forward declarations ---- */
static int peer_dc_send(const uint8_t *data, size_t len, void *user);
static void peer_rtcp_timer(void *user);

/* ---- RTCP periodic send timer (fires on transport thread every 5 seconds) ---- */

#define RTCP_INTERVAL_MS 5000

static void peer_rtcp_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->connection_state != RTC_CONNECTION_CONNECTED)
        return;

    /* Build and send RTCP for each active transceiver */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[i];

        /* Sender Report (if we're sending) */
        if (t->sender.active && t->sender.rtcp_stats.packets_sent > 0) {
            rtc_rtcp_packet_t pkt;
            if (rtc_rtcp_build_sr(&pkt, &t->sender.rtcp_stats) == RTC_OK) {
                uint8_t buf[RTCP_MAX_PACKET + 4 + SRTP_AUTH_TAG_LEN];
                memcpy(buf, pkt.buf, pkt.buf_len);
                size_t len = pkt.buf_len;
                if (rtc_srtp_protect_rtcp(&pc->srtp_send, buf, &len) == RTC_OK) {
                    rtc_transport_send_to_remote(&pc->transport, buf, len);
                }
            }
            t->sender.rtcp_stats.last_report_time = rtc_time_ms();
        }

        /* Receiver Report (if we're receiving) */
        if (t->receiver.active && t->receiver.rtcp_stats.packets_received > 0) {
            rtc_rtcp_packet_t pkt;
            if (rtc_rtcp_build_rr(&pkt, &t->receiver.rtcp_stats) == RTC_OK) {
                uint8_t buf[RTCP_MAX_PACKET + 4 + SRTP_AUTH_TAG_LEN];
                memcpy(buf, pkt.buf, pkt.buf_len);
                size_t len = pkt.buf_len;
                if (rtc_srtp_protect_rtcp(&pc->srtp_send, buf, &len) == RTC_OK) {
                    rtc_transport_send_to_remote(&pc->transport, buf, len);
                }
            }
            t->receiver.rtcp_stats.last_report_time = rtc_time_ms();
        }
    }

    /* Re-arm timer */
    pc->rtcp_timer_id = rtc_transport_add_timer(&pc->transport, rtc_time_ms() + RTCP_INTERVAL_MS,
                                                peer_rtcp_timer, pc);
}

/* ---- State helpers ---- */

static void peer_set_signaling(rtc_peer_connection_t *pc, rtc_signaling_state_t s) {
    pc->signaling_state = s;
    if (pc->on_signaling_state)
        pc->on_signaling_state(s, pc->on_signaling_state_user);
}

static void peer_set_ice_gathering(rtc_peer_connection_t *pc, rtc_ice_gathering_state_t s) {
    pc->ice_gathering_state = s;
    if (pc->on_ice_gathering_state)
        pc->on_ice_gathering_state(s, pc->on_ice_gathering_state_user);
}

static void peer_set_ice_connection(rtc_peer_connection_t *pc, rtc_ice_connection_state_t s) {
    pc->ice_connection_state = s;
    if (pc->on_ice_connection_state)
        pc->on_ice_connection_state(s, pc->on_ice_connection_state_user);
}

static void peer_set_connection(rtc_peer_connection_t *pc, rtc_connection_state_t s) {
    pc->connection_state = s;
    if (pc->on_connection_state)
        pc->on_connection_state(s, pc->on_connection_state_user);
}

/* ---- DTLS send callback ---- */

static int peer_dtls_send(const uint8_t *data, size_t len, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    return rtc_transport_send_to_remote(&pc->transport, data, len);
}

/* ---- Complete connection (called on transport thread after DTLS connects) ---- */

static void peer_complete_connection(rtc_peer_connection_t *pc) {
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

    /* Wire up senders to SRTP + transport */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_sender *s = &pc->transceivers[i].sender;
        if (s->active) {
            s->srtp = &pc->srtp_send;
            s->transport = &pc->transport;
        }
    }

    /* Create rate controller */
    rtc_rate_control_config_t rc_cfg = {
        .target_bitrate_kbps = 500,
        .min_bitrate_kbps = 100,
        .max_bitrate_kbps = 2500,
    };
    pc->rate_ctrl = rtc_rate_control_create(&rc_cfg);

    /* Wire rate controller to video senders */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_sender *s = &pc->transceivers[i].sender;
        if (s->active && s->kind == RTC_KIND_VIDEO)
            s->rate_ctrl = pc->rate_ctrl;
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

static void peer_connect_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;

    peer_set_connection(pc, RTC_CONNECTION_CONNECTING);
    peer_set_ice_connection(pc, RTC_ICE_CONNECTION_CHECKING);

    /* Step 1: ICE connectivity check (blocks transport thread with own select loop) */
    RTC_LOG_INFO("Peer: starting ICE connectivity checks...");
    int rc = rtc_ice_connect(&pc->ice);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("Peer: ICE connect failed (rc=%d)", rc);
        peer_set_ice_connection(pc, RTC_ICE_CONNECTION_FAILED);
        peer_set_connection(pc, RTC_CONNECTION_FAILED);
        return;
    }

    /* Step 2: Start DTLS handshake
     * Only the DTLS client initiates. The server waits for ClientHello
     * which arrives via the transport recv callback. */
    RTC_LOG_INFO("Peer: starting DTLS handshake (role=%s)...",
                 pc->dtls.role == RTC_DTLS_ROLE_CLIENT ? "client" : "server");
    if (pc->dtls.role == RTC_DTLS_ROLE_CLIENT) {
        rc = rtc_dtls_handshake(&pc->dtls);
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

    /* DTLS completion is handled in the recv callback when DTLS packets arrive.
     * The transport thread resumes its normal poller loop now. */
}

/* ---- Transport recv callback (fires on transport thread) ---- */

static void peer_transport_recv(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                const rtc_addr_t *from, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    (void)from;

    switch (type) {
        case RTC_PKT_STUN:
            rtc_ice_handle_stun(&pc->ice, data, len, from);
            break;

        case RTC_PKT_DTLS:
            /* Only process DTLS after connection has started (both desc set) */
            if (!pc->connect_started)
                break;
            rtc_dtls_recv(&pc->dtls, data, len);
            /* Check if DTLS just completed the handshake */
            if (pc->dtls.state == RTC_DTLS_STATE_CONNECTED &&
                pc->connection_state == RTC_CONNECTION_CONNECTING) {
                peer_complete_connection(pc);
            }
            /* Check for DTLS application data (data channel messages) */
            if (pc->dtls.state == RTC_DTLS_STATE_CONNECTED) {
                uint8_t app_buf[2048];
                int app_len;
                while ((app_len = SSL_read(pc->dtls.ssl, app_buf, sizeof(app_buf))) > 0) {
                    rtc_dc_manager_recv(&pc->dc_manager, app_buf, (size_t)app_len);
                }
            }
            break;

        case RTC_PKT_RTP:
            if (pc->connection_state == RTC_CONNECTION_CONNECTED && len > 12) {
                uint8_t buf[2048];
                if (len > sizeof(buf))
                    break;
                memcpy(buf, data, len);

                /* SRTP unprotect (recv ctx only touched on transport thread) */
                size_t pkt_len = len;
                int rc = rtc_srtp_unprotect(&pc->srtp_recv, buf, &pkt_len);
                if (rc != RTC_OK) {
                    RTC_LOG_WARN("Peer: SRTP unprotect failed");
                    break;
                }

                /* Parse RTP header */
                rtc_rtp_packet_t pkt;
                rc = rtc_rtp_parse(&pkt, buf, pkt_len);
                if (rc != RTC_OK)
                    break;

                /* Fast path: O(1) SSRC → receiver lookup */
                struct rtc_rtp_receiver *r =
                    (struct rtc_rtp_receiver *)rtc_u32_map_get(&pc->recv_map, pkt.header.ssrc);

                /* Slow path: match by payload type on first packet per SSRC */
                if (!r) {
                    for (int i = 0; i < pc->transceiver_count; i++) {
                        struct rtc_rtp_receiver *rx = &pc->transceivers[i].receiver;
                        if (rx->active && rx->on_frame &&
                            rx->codec.payload_type == pkt.header.payload_type) {
                            r = rx;
                            r->ssrc = pkt.header.ssrc;
                            rtc_u32_map_set(&pc->recv_map, pkt.header.ssrc, r);
                            break;
                        }
                    }
                }

                if (r) {
                    rtc_rtcp_stats_on_rtp_recv(&r->rtcp_stats, pkt.header.seq, pkt.header.timestamp,
                                               pkt.header.ssrc, r->codec.clock_rate);
                    r->on_frame(pkt.payload, pkt.payload_len, pkt.header.seq, pkt.header.timestamp,
                                pkt.header.ssrc, pkt.header.marker, r->on_frame_user);
                }
            }
            break;

        case RTC_PKT_RTCP:
            if (pc->connection_state == RTC_CONNECTION_CONNECTED && len > 8) {
                uint8_t buf[2048];
                if (len > sizeof(buf))
                    break;
                memcpy(buf, data, len);

                /* SRTCP unprotect */
                size_t pkt_len = len;
                int rc = rtc_srtp_unprotect_rtcp(&pc->srtp_recv, buf, &pkt_len);
                if (rc != RTC_OK) {
                    RTC_LOG_WARN("Peer: SRTCP unprotect failed");
                    break;
                }

                /* Parse RTCP packet */
                rtc_rtcp_packet_t rtcp;
                rc = rtc_rtcp_parse(&rtcp, buf, pkt_len);
                if (rc != RTC_OK)
                    break;

                if (rtcp.header.packet_type == RTCP_PT_SR) {
                    /* Store last SR NTP for DLSR calculation in the matching receiver */
                    uint32_t lsr = (rtcp.sr.ntp_sec & 0xFFFF) << 16 | (rtcp.sr.ntp_frac >> 16);
                    struct rtc_rtp_receiver *r =
                        (struct rtc_rtp_receiver *)rtc_u32_map_get(&pc->recv_map, rtcp.sender_ssrc);
                    if (r) {
                        r->rtcp_stats.last_sr_ntp = lsr;
                        r->rtcp_stats.last_sr_recv_time = rtc_time_ms();
                    }
                } else if (rtcp.header.packet_type == RTCP_PT_RR) {
                    /* Extract feedback and notify callback */
                    for (int i = 0; i < rtcp.report_count; i++) {
                        const rtc_rtcp_rr_block_t *rr = &rtcp.reports[i];

                        /* Compute RTT from delay_since_sr */
                        int rtt_ms = 0;
                        if (rr->last_sr != 0) {
                            /* Get current NTP compact (middle 32 bits) */
                            uint64_t now_ms = rtc_time_ms();
                            uint32_t now_sec = (uint32_t)(now_ms / 1000);
                            uint32_t now_frac = (uint32_t)((now_ms % 1000) * 4294967ULL);
                            uint32_t now_compact = (now_sec & 0xFFFF) << 16 | (now_frac >> 16);
                            /* RTT = now - last_sr - delay_since_sr (all in 1/65536 sec) */
                            uint32_t rtt_ntp = now_compact - rr->last_sr - rr->delay_since_sr;
                            rtt_ms = (int)((rtt_ntp * 1000ULL) >> 16);
                            if (rtt_ms < 0)
                                rtt_ms = 0;
                        }

                        /* TODO: RTCP RR demux — rr->ssrc identifies which of
                         * our senders is being reported on, but we feed a single
                         * shared rate controller regardless. With audio+video
                         * senders, an audio RR (low jitter, no loss) incorrectly
                         * inflates the rate estimate used for video. Fix: match
                         * rr->ssrc to the corresponding sender's rtcp_stats.ssrc
                         * and give each sender (or at least each video sender)
                         * its own rate controller. */
                        if (pc->rate_ctrl) {
                            rtc_rate_control_on_rtcp_rr(pc->rate_ctrl, rr->fraction_lost, rtt_ms,
                                                        (int)rr->jitter);
                        }
                    }
                }
            }
            break;

        default:
            break;
    }
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
        struct rtc_rtp_transceiver *t = &pc->transceivers[i];
        rtc_sdp_media_t *m = &sdp->media[sdp->media_count];
        memset(m, 0, sizeof(*m));

        const char *mime = t->sender.codec.mime_type;
        if (strncmp(mime, "audio/", 6) == 0) {
            m->media_type = RTC_MEDIA_AUDIO;
            size_t clen = strlen(mime + 6);
            if (clen >= sizeof(m->codec_name))
                clen = sizeof(m->codec_name) - 1;
            memcpy(m->codec_name, mime + 6, clen);
            m->codec_name[clen] = '\0';
        } else if (strncmp(mime, "video/", 6) == 0) {
            m->media_type = RTC_MEDIA_VIDEO;
            size_t clen = strlen(mime + 6);
            if (clen >= sizeof(m->codec_name))
                clen = sizeof(m->codec_name) - 1;
            memcpy(m->codec_name, mime + 6, clen);
            m->codec_name[clen] = '\0';
        } else {
            m->media_type = RTC_MEDIA_APPLICATION;
        }

        m->payload_type = t->sender.codec.payload_type;
        m->clockrate = (int)t->sender.codec.clock_rate;
        m->channels = t->sender.codec.channels;
        m->mid_index = t->mid_index;

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

    /* Extract STUN server from config */
    if (config && config->ice_server_count > 0 && config->ice_servers[0].url_count > 0 &&
        config->ice_servers[0].urls[0]) {
        parse_stun_url(config->ice_servers[0].urls[0], pc->stun_server, sizeof(pc->stun_server),
                       &pc->stun_port);
    }

    /* Initialize transport (socket + thread) */
    int rc = rtc_transport_init(&pc->transport);
    if (rc != RTC_OK) {
        free(pc);
        return NULL;
    }

    /* Initialize SSRC → receiver lookup map */
    if (rtc_u32_map_init(&pc->recv_map) != RTC_OK) {
        rtc_transport_close(&pc->transport);
        free(pc);
        return NULL;
    }

    /* Register recv callback */
    rtc_transport_set_recv_callback(&pc->transport, peer_transport_recv, pc);

    /* Initialize ICE agent (borrows transport) */
    rc = rtc_ice_init(&pc->ice, &pc->transport, pc->stun_server[0] ? pc->stun_server : NULL,
                      pc->stun_port);
    if (rc != RTC_OK) {
        rtc_transport_close(&pc->transport);
        free(pc);
        return NULL;
    }

    /* Generate DTLS certificate (needed for fingerprint in SDP) */
    rc = rtc_dtls_init(&pc->dtls, RTC_DTLS_ROLE_CLIENT, peer_dtls_send, pc);
    if (rc != RTC_OK) {
        rtc_transport_close(&pc->transport);
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

    rtc_dc_manager_close(&pc->dc_manager);
    rtc_srtp_close(&pc->srtp_send);
    rtc_srtp_close(&pc->srtp_recv);
    rtc_dtls_close(&pc->dtls);
    rtc_ice_close(&pc->ice);
    rtc_transport_close(&pc->transport);
    rtc_u32_map_free(&pc->recv_map);
    rtc_sdp_close(&pc->local_sdp);
    rtc_sdp_close(&pc->remote_sdp);
    if (pc->rate_ctrl) {
        rtc_rate_control_destroy(pc->rate_ctrl);
        pc->rate_ctrl = NULL;
    }

    pc->connection_state = RTC_CONNECTION_CLOSED;
    pc->signaling_state = RTC_SIGNALING_CLOSED;

    RTC_LOG_INFO("Peer connection closed");
}

void rtc_peer_connection_destroy(rtc_peer_connection_t *pc) {
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
    memset(t, 0, sizeof(*t));

    t->used = true;
    t->direction = RTC_DIR_SENDRECV;
    t->mid_index = pc->transceiver_count;
    snprintf(t->mid, sizeof(t->mid), "%d", t->mid_index);

    /* Sender */
    t->sender.codec = *codec;
    t->sender.kind = kind;
    t->sender.active = true;
    rtc_rtp_session_init(&t->sender.rtp_session, codec->payload_type, codec->clock_rate);
    rtc_rtcp_stats_init(&t->sender.rtcp_stats, t->sender.rtp_session.ssrc);

    /* Receiver (activated when remote description arrives) */
    t->receiver.codec = *codec;
    t->receiver.kind = kind;
    t->receiver.active = false;
    rtc_rtcp_stats_init(&t->receiver.rtcp_stats, t->sender.rtp_session.ssrc);

    pc->transceiver_count++;
    return &t->sender;
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

    pc->local_desc = *desc;
    pc->has_local_desc = true;

    /* Update signaling state */
    if (desc->type == RTC_SDP_OFFER) {
        peer_set_signaling(pc, RTC_SIGNALING_HAVE_LOCAL_OFFER);
    } else if (desc->type == RTC_SDP_ANSWER) {
        if (pc->signaling_state == RTC_SIGNALING_HAVE_REMOTE_OFFER)
            peer_set_signaling(pc, RTC_SIGNALING_STABLE);
    }

    /* Fire ICE gathering callbacks for already-gathered candidates */
    peer_set_ice_gathering(pc, RTC_ICE_GATHERING_GATHERING);
    for (int i = 0; i < pc->ice.local_candidate_count; i++) {
        if (pc->on_ice_candidate) {
            rtc_ice_candidate_desc_t cand_desc;
            memset(&cand_desc, 0, sizeof(cand_desc));

            rtc_ice_candidate_t *c = &pc->ice.local_candidates[i];
            char ip[64];
            uint16_t port;
            rtc_addr_to_string(&c->addr, ip, sizeof(ip), &port);

            const char *ctype = "host";
            if (c->type == ICE_CANDIDATE_SRFLX)
                ctype = "srflx";

            snprintf(cand_desc.candidate, sizeof(cand_desc.candidate),
                     "candidate:%s 1 udp %u %s %u typ %s", c->foundation, c->priority, ip, port,
                     ctype);
            snprintf(cand_desc.mid, sizeof(cand_desc.mid), "0");
            cand_desc.mid_index = 0;

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
            r->active = true;
            if (pc->on_track)
                pc->on_track((rtc_rtp_receiver_t *)r, pc->on_track_user);
        }
    }

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
