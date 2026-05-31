/*
 * rtc_peer_packets.c - Peer Connection: transport-thread packet I/O.
 *
 * Owns everything that fires on the transport thread:
 *   - peer_transport_recv: classify and dispatch STUN / DTLS / RTP / RTCP.
 *   - peer_rtcp_timer:     periodic SR / RR emission.
 *   - peer_twcc_fb_timer:  periodic transport-cc feedback emission.
 *   - peer_on_bwe_bitrate: trampoline from BWE → user callback.
 * Feature-gated sections: TWCC arrival recording, BWE feedback, rate control.
 */
#include "rtc_peer_internal.h"

#include <string.h>

/* Scratch buffer size for inbound RTP/RTCP unprotect. Large enough for any
 * SRTP/SRTCP packet that fits in a single UDP datagram. */
#define PEER_RECV_BUF_SIZE 2048

/* ---- DTLS receive path ---- */

static void handle_dtls(rtc_peer_connection_t *pc, const uint8_t *data, size_t len) {
    /* Only process DTLS after connection has started (both desc set) */
    if (!pc->connect_started)
        return;
    rtc_dtls_recv(&pc->dtls, data, len);
    /* Check if DTLS just completed the handshake */
    if (pc->dtls.state == RTC_DTLS_STATE_CONNECTED &&
        pc->connection_state == RTC_CONNECTION_CONNECTING) {
        peer_complete_connection(pc);
    }
    /* Check for DTLS application data (data channel messages) */
    if (pc->dtls.state == RTC_DTLS_STATE_CONNECTED) {
        int app_len;
        while ((app_len = SSL_read(pc->dtls.ssl, pc->app_buf, (int)pc->app_buf_cap)) > 0) {
            rtc_dc_manager_recv(&pc->dc_manager, pc->app_buf, (size_t)app_len);
        }
    }
}

/* ---- RTP receive path ---- */

/* Find the receiver matching a parsed RTP packet. Fast path: O(1) lookup by
 * SSRC. Slow path on the first packet for a new SSRC: match by payload type
 * and populate the map. */
static struct rtc_rtp_receiver *demux_rtp_receiver(rtc_peer_connection_t *pc,
                                                   const rtc_rtp_packet_t *pkt) {
    struct rtc_rtp_receiver *r =
        (struct rtc_rtp_receiver *)rtc_u32_map_get(&pc->recv_map, pkt->header.ssrc);
    if (r)
        return r;

    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_receiver *rx = &pc->transceivers[i].receiver;
        if (rx->active && rx->on_frame && rx->codec.payload_type == pkt->header.payload_type) {
            rx->ssrc = pkt->header.ssrc;
            rtc_u32_map_set(&pc->recv_map, pkt->header.ssrc, rx);
            return rx;
        }
    }
    return NULL;
}

#ifdef MRTC_ENABLE_TWCC
/* Record the transport-cc sequence number from an inbound RTP packet so the
 * TWCC feedback timer can report it back to the sender. */
static void record_twcc_arrival(rtc_peer_connection_t *pc, const rtc_rtp_packet_t *pkt) {
    if (pc->twcc_ext_id_recv == 0 || !pkt->header.extension || !pkt->ext_data || pkt->ext_len == 0)
        return;
    rtc_rtp_ext_t exts[RTC_RTP_EXT_MAX_ENTRIES];
    size_t cnt = RTC_RTP_EXT_MAX_ENTRIES;
    if (rtc_rtp_ext_parse_body(pkt->ext_data, pkt->ext_len, exts, &cnt) != RTC_OK)
        return;
    const rtc_rtp_ext_t *e = rtc_rtp_ext_find(exts, cnt, pc->twcc_ext_id_recv);
    if (!e)
        return;
    uint16_t tseq = rtc_rtp_ext_read_transport_cc(e);
    rtc_twcc_receiver_on_packet(&pc->twcc_receiver, tseq, rtc_time_us());
    if (pc->twcc_remote_ssrc == 0)
        pc->twcc_remote_ssrc = pkt->header.ssrc;
    pc->twcc_have_packets = true;
}
#endif

static void handle_rtp(rtc_peer_connection_t *pc, const uint8_t *data, size_t len) {
    if (pc->connection_state != RTC_CONNECTION_CONNECTED || len <= 12)
        return;

    uint8_t buf[PEER_RECV_BUF_SIZE];
    if (len > sizeof(buf))
        return;
    memcpy(buf, data, len);

    /* SRTP unprotect (recv ctx only touched on transport thread) */
    size_t pkt_len = len;
    if (rtc_srtp_unprotect(&pc->srtp_recv, buf, &pkt_len) != RTC_OK) {
        RTC_LOG_WARN("Peer: SRTP unprotect failed");
        return;
    }

    rtc_rtp_packet_t pkt;
    if (rtc_rtp_parse(&pkt, buf, pkt_len) != RTC_OK)
        return;

    struct rtc_rtp_receiver *r = demux_rtp_receiver(pc, &pkt);
    if (r) {
        rtc_rtcp_stats_on_rtp_recv(&r->rtcp_stats, pkt.header.seq, pkt.header.timestamp,
                                   pkt.header.ssrc, r->codec.clock_rate);
        r->on_frame(pkt.payload, pkt.payload_len, pkt.header.seq, pkt.header.timestamp,
                    pkt.header.ssrc, pkt.header.marker, r->on_frame_user);
    }

#ifdef MRTC_ENABLE_TWCC
    record_twcc_arrival(pc, &pkt);
#endif
}

/* ---- RTCP receive path ---- */

/* RTCP SR: remember last SR NTP timestamp on the matched receiver so the
 * next RR can compute DLSR (RFC 3550 §6.4.1). */
static void handle_rtcp_sr(rtc_peer_connection_t *pc, const uint8_t *buf, size_t pkt_len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, pkt_len) != RTC_OK)
        return;
    uint32_t lsr = (rtcp.sr.ntp_sec & 0xFFFF) << 16 | (rtcp.sr.ntp_frac >> 16);
    struct rtc_rtp_receiver *r =
        (struct rtc_rtp_receiver *)rtc_u32_map_get(&pc->recv_map, rtcp.sender_ssrc);
    if (r) {
        r->rtcp_stats.last_sr_ntp = lsr;
        r->rtcp_stats.last_sr_recv_time = rtc_time_ms();
    }
}

/* Compact-NTP RTT from RFC 3550 §6.4.1: RTT = now - last_sr - delay_since_sr,
 * all in the 16.16 compact NTP format. */
static int rtt_from_rr(const rtc_rtcp_rr_block_t *rr) {
    if (rr->last_sr == 0)
        return 0;
    uint64_t now_ms = rtc_time_ms();
    uint32_t now_sec = (uint32_t)(now_ms / 1000);
    uint32_t now_frac = (uint32_t)((now_ms % 1000) * 4294967ULL);
    uint32_t now_compact = (now_sec & 0xFFFF) << 16 | (now_frac >> 16);
    uint32_t rtt_ntp = now_compact - rr->last_sr - rr->delay_since_sr;
    int rtt_ms = (int)((rtt_ntp * 1000ULL) >> 16);
    return rtt_ms < 0 ? 0 : rtt_ms;
}

static void handle_rtcp_rr(rtc_peer_connection_t *pc, const uint8_t *buf, size_t pkt_len) {
    rtc_rtcp_packet_t rtcp;
    if (rtc_rtcp_parse(&rtcp, buf, pkt_len) != RTC_OK)
        return;

#ifdef MRTC_ENABLE_TWCC
    if (pc->bwe && rtcp.report_count > 0)
        rtc_bwe_on_loss(pc->bwe, rtcp.reports[0].fraction_lost);
#endif

    for (int i = 0; i < rtcp.report_count; i++) {
        const rtc_rtcp_rr_block_t *rr = &rtcp.reports[i];
        int rtt_ms = rtt_from_rr(rr);

#ifdef MRTC_ENABLE_RATE_CONTROL
        /* Route to per-sender rate controller via send_map */
        struct rtc_rtp_sender *sender =
            (struct rtc_rtp_sender *)rtc_u32_map_get(&pc->send_map, rr->ssrc);
        if (sender && sender->rate_ctrl) {
            rtc_rate_control_on_rtcp_rr(sender->rate_ctrl, rr->fraction_lost, rtt_ms,
                                        (int)rr->jitter);
        } else if (pc->rate_ctrl) {
            rtc_rate_control_on_rtcp_rr(pc->rate_ctrl, rr->fraction_lost, rtt_ms, (int)rr->jitter);
        }
#else
        (void)rtt_ms;
#endif
    }
}

static void handle_rtcp_nack(rtc_peer_connection_t *pc, const uint8_t *buf, size_t pkt_len) {
    rtc_rtcp_nack_t nack;
    if (rtc_rtcp_parse_nack(&nack, buf, pkt_len) != RTC_OK)
        return;
    struct rtc_rtp_sender *sender =
        (struct rtc_rtp_sender *)rtc_u32_map_get(&pc->send_map, nack.media_ssrc);
    if (sender)
        rtc_rtp_sender_handle_nack(sender, nack.lost_seqs, nack.lost_count);
}

#ifdef MRTC_ENABLE_TWCC
static void handle_rtcp_twcc(rtc_peer_connection_t *pc, const uint8_t *buf, size_t pkt_len) {
    rtc_rtcp_twcc_t tw;
    if (rtc_rtcp_parse_twcc(&tw, buf, pkt_len) != RTC_OK)
        return;
    RTC_LOG_DBG("Peer: TWCC feedback base=%u count=%d fb_pkt=%u", (unsigned)tw.base_seq,
                tw.item_count, (unsigned)tw.fb_pkt_count);
    if (!pc->bwe)
        return;
    for (int i = 0; i < tw.item_count; i++) {
        const rtc_twcc_sent_pkt_t *s = rtc_twcc_sender_lookup(&pc->twcc_sender, tw.items[i].seq);
        if (!s)
            continue;
        uint64_t recv_us = tw.items[i].received ? tw.items[i].recv_time_us : 0;
        rtc_bwe_on_packet_feedback(pc->bwe, s->send_time_us, recv_us, s->size);
    }
}
#endif

/* PLI (RFC 4585) and FIR (RFC 5104) both ask the sender for a keyframe and
 * the only field consumed is media_ssrc, so they share a handler. */
static void handle_rtcp_psfb(rtc_peer_connection_t *pc, uint8_t fmt, const uint8_t *buf,
                             size_t pkt_len) {
    uint32_t media_ssrc;
    if (fmt == RTCP_FMT_PLI) {
        rtc_rtcp_pli_t pli;
        if (rtc_rtcp_parse_pli(&pli, buf, pkt_len) != RTC_OK)
            return;
        media_ssrc = pli.media_ssrc;
    } else if (fmt == RTCP_FMT_FIR) {
        rtc_rtcp_fir_t fir;
        if (rtc_rtcp_parse_fir(&fir, buf, pkt_len) != RTC_OK)
            return;
        media_ssrc = fir.media_ssrc;
    } else {
        return;
    }

    struct rtc_rtp_sender *sender =
        (struct rtc_rtp_sender *)rtc_u32_map_get(&pc->send_map, media_ssrc);
    if (sender)
        rtc_rtp_sender_handle_pli(sender);
}

static void handle_rtcp(rtc_peer_connection_t *pc, const uint8_t *data, size_t len) {
    if (pc->connection_state != RTC_CONNECTION_CONNECTED || len <= 8)
        return;

    uint8_t buf[PEER_RECV_BUF_SIZE];
    if (len > sizeof(buf))
        return;
    memcpy(buf, data, len);

    /* SRTCP unprotect */
    size_t pkt_len = len;
    if (rtc_srtp_unprotect_rtcp(&pc->srtp_recv, buf, &pkt_len) != RTC_OK) {
        RTC_LOG_WARN("Peer: SRTCP unprotect failed");
        return;
    }

    uint8_t pt, fmt;
    if (!rtc_rtcp_get_pt_fmt(buf, pkt_len, &pt, &fmt))
        return;

    switch (pt) {
        case RTCP_PT_SR:
            handle_rtcp_sr(pc, buf, pkt_len);
            break;
        case RTCP_PT_RR:
            handle_rtcp_rr(pc, buf, pkt_len);
            break;
        case RTCP_PT_RTPFB:
            if (fmt == RTCP_FMT_NACK)
                handle_rtcp_nack(pc, buf, pkt_len);
#ifdef MRTC_ENABLE_TWCC
            else if (fmt == 15) /* RFC 8888 transport-cc */
                handle_rtcp_twcc(pc, buf, pkt_len);
#endif
            break;
        case RTCP_PT_PSFB:
            handle_rtcp_psfb(pc, fmt, buf, pkt_len);
            break;
        default:
            break;
    }
}

/* ---- Transport recv callback (fires on transport thread) ---- */

void peer_transport_recv(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                         const rtc_addr_t *from, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;

    switch (type) {
        case RTC_PKT_STUN:
            rtc_ice_handle_stun(&pc->ice, data, len, from);
            break;
        case RTC_PKT_DTLS:
            handle_dtls(pc, data, len);
            break;
        case RTC_PKT_RTP:
            handle_rtp(pc, data, len);
            break;
        case RTC_PKT_RTCP:
            handle_rtcp(pc, data, len);
            break;
        default:
            break;
    }
}

/* ---- RTCP periodic send timer (fires on transport thread every 5 seconds) ---- */

void peer_rtcp_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->connection_state != RTC_CONNECTION_CONNECTED)
        return;

    /* Build and send RTCP for each active transceiver */
    for (int i = 0; i < pc->transceiver_count; i++) {
        struct rtc_rtp_transceiver *t = &pc->transceivers[i];
        rtc_rtp_sender_emit_sr(&t->sender, &pc->srtp_send, &pc->transport);
        rtc_rtp_receiver_emit_rr(&t->receiver, &pc->srtp_send, &pc->transport);
    }

    /* Re-arm timer */
    pc->rtcp_timer_id = rtc_transport_add_timer(&pc->transport, rtc_time_ms() + RTCP_INTERVAL_MS,
                                                peer_rtcp_timer, pc);
}

/* ---- TWCC feedback timer (fires every 100ms on transport thread) ---- */

#ifdef MRTC_ENABLE_TWCC

void peer_twcc_fb_timer(void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->connection_state != RTC_CONNECTION_CONNECTED) {
        pc->twcc_fb_timer_id = rtc_transport_add_timer(
            &pc->transport, rtc_time_ms() + TWCC_FB_INTERVAL_MS, peer_twcc_fb_timer, pc);
        return;
    }

    if (pc->twcc_have_packets && pc->twcc_receiver.have_base) {
        uint8_t fb[RTCP_MAX_PACKET];
        size_t fb_len = 0;
        if (rtc_twcc_receiver_build_feedback(&pc->twcc_receiver, pc->twcc_local_ssrc,
                                             pc->twcc_remote_ssrc, fb, sizeof(fb),
                                             &fb_len) == RTC_OK) {
            uint8_t buf[RTCP_MAX_PACKET + 4 + SRTP_AUTH_TAG_LEN];
            if (fb_len <= sizeof(buf)) {
                memcpy(buf, fb, fb_len);
                size_t out_len = fb_len;
                if (rtc_srtp_protect_rtcp(&pc->srtp_send, buf, &out_len, sizeof(buf)) == RTC_OK)
                    rtc_transport_send_to_remote(&pc->transport, buf, out_len);
            }
        }
        pc->twcc_have_packets = false;
    }

    pc->twcc_fb_timer_id = rtc_transport_add_timer(
        &pc->transport, rtc_time_ms() + TWCC_FB_INTERVAL_MS, peer_twcc_fb_timer, pc);
}

/* ---- BWE bitrate-change trampoline (fires on transport thread) ---- */

void peer_on_bwe_bitrate(uint32_t bitrate_bps, void *user) {
    rtc_peer_connection_t *pc = (rtc_peer_connection_t *)user;
    if (pc->on_bitrate_estimate)
        pc->on_bitrate_estimate(bitrate_bps, pc->on_bitrate_estimate_user);
}

#endif /* MRTC_ENABLE_TWCC */
