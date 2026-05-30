/*
 * rtc_peer_packets.c - Peer Connection: data plane.
 *
 * Transport recv callback dispatching STUN, DTLS, RTP, and RTCP packets.
 * Feature-gated sections: TWCC arrival recording, BWE feedback, rate control.
 */
#include "rtc_peer_internal.h"

#include <string.h>

/* ---- Transport recv callback (fires on transport thread) ---- */

void peer_transport_recv(rtc_pkt_type_t type, const uint8_t *data, size_t len,
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
                int app_len;
                while ((app_len = SSL_read(pc->dtls.ssl, pc->app_buf, (int)pc->app_buf_cap)) > 0) {
                    rtc_dc_manager_recv(&pc->dc_manager, pc->app_buf, (size_t)app_len);
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

#ifdef MRTC_ENABLE_TWCC
                /* Transport-CC arrival recording */
                if (pc->twcc_ext_id_recv != 0 && pkt.header.extension && pkt.ext_data &&
                    pkt.ext_len > 0) {
                    rtc_rtp_ext_t exts[RTC_RTP_EXT_MAX_ENTRIES];
                    size_t cnt = RTC_RTP_EXT_MAX_ENTRIES;
                    if (rtc_rtp_ext_parse_body(pkt.ext_data, pkt.ext_len, exts, &cnt) == RTC_OK) {
                        const rtc_rtp_ext_t *e = rtc_rtp_ext_find(exts, cnt, pc->twcc_ext_id_recv);
                        if (e) {
                            uint16_t tseq = rtc_rtp_ext_read_transport_cc(e);
                            rtc_twcc_receiver_on_packet(&pc->twcc_receiver, tseq, rtc_time_us());
                            if (pc->twcc_remote_ssrc == 0)
                                pc->twcc_remote_ssrc = pkt.header.ssrc;
                            pc->twcc_have_packets = true;
                        }
                    }
                }
#endif /* MRTC_ENABLE_TWCC */
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

                /* Extract PT and FMT for dispatch */
                uint8_t pt, fmt;
                if (!rtc_rtcp_get_pt_fmt(buf, pkt_len, &pt, &fmt))
                    break;

                if (pt == RTCP_PT_SR) {
                    /* Parse SR and store last SR NTP for DLSR calc */
                    rtc_rtcp_packet_t rtcp;
                    rc = rtc_rtcp_parse(&rtcp, buf, pkt_len);
                    if (rc != RTC_OK)
                        break;
                    uint32_t lsr = (rtcp.sr.ntp_sec & 0xFFFF) << 16 | (rtcp.sr.ntp_frac >> 16);
                    struct rtc_rtp_receiver *r =
                        (struct rtc_rtp_receiver *)rtc_u32_map_get(&pc->recv_map, rtcp.sender_ssrc);
                    if (r) {
                        r->rtcp_stats.last_sr_ntp = lsr;
                        r->rtcp_stats.last_sr_recv_time = rtc_time_ms();
                    }
                } else if (pt == RTCP_PT_RR) {
                    /* Extract feedback and route to rate controller / BWE */
                    rtc_rtcp_packet_t rtcp;
                    rc = rtc_rtcp_parse(&rtcp, buf, pkt_len);
                    if (rc != RTC_OK)
                        break;
#ifdef MRTC_ENABLE_TWCC
                    if (pc->bwe && rtcp.report_count > 0)
                        rtc_bwe_on_loss(pc->bwe, rtcp.reports[0].fraction_lost);
#endif
                    for (int i = 0; i < rtcp.report_count; i++) {
                        const rtc_rtcp_rr_block_t *rr = &rtcp.reports[i];

                        /* Compute RTT from delay_since_sr */
                        int rtt_ms = 0;
                        if (rr->last_sr != 0) {
                            uint64_t now_ms = rtc_time_ms();
                            uint32_t now_sec = (uint32_t)(now_ms / 1000);
                            uint32_t now_frac = (uint32_t)((now_ms % 1000) * 4294967ULL);
                            uint32_t now_compact = (now_sec & 0xFFFF) << 16 | (now_frac >> 16);
                            uint32_t rtt_ntp = now_compact - rr->last_sr - rr->delay_since_sr;
                            rtt_ms = (int)((rtt_ntp * 1000ULL) >> 16);
                            if (rtt_ms < 0)
                                rtt_ms = 0;
                        }

#ifdef MRTC_ENABLE_RATE_CONTROL
                        /* Route to per-sender rate controller via send_map */
                        struct rtc_rtp_sender *sender =
                            (struct rtc_rtp_sender *)rtc_u32_map_get(&pc->send_map, rr->ssrc);
                        if (sender && sender->rate_ctrl) {
                            rtc_rate_control_on_rtcp_rr(sender->rate_ctrl, rr->fraction_lost,
                                                        rtt_ms, (int)rr->jitter);
                        } else if (pc->rate_ctrl) {
                            rtc_rate_control_on_rtcp_rr(pc->rate_ctrl, rr->fraction_lost, rtt_ms,
                                                        (int)rr->jitter);
                        }
#else
                        (void)rtt_ms;
#endif
                    }
                } else if (pt == RTCP_PT_RTPFB && fmt == RTCP_FMT_NACK) {
                    /* NACK — retransmit lost packets */
                    rtc_rtcp_nack_t nack;
                    if (rtc_rtcp_parse_nack(&nack, buf, pkt_len) == RTC_OK) {
                        struct rtc_rtp_sender *sender = (struct rtc_rtp_sender *)rtc_u32_map_get(
                            &pc->send_map, nack.media_ssrc);
                        if (sender)
                            rtc_rtp_sender_handle_nack(sender, nack.lost_seqs, nack.lost_count);
                    }
#ifdef MRTC_ENABLE_TWCC
                } else if (pt == RTCP_PT_RTPFB && fmt == 15) {
                    /* Transport-CC feedback — feed each reported packet to BWE. */
                    rtc_rtcp_twcc_t tw;
                    if (rtc_rtcp_parse_twcc(&tw, buf, pkt_len) == RTC_OK) {
                        RTC_LOG_DBG("Peer: TWCC feedback base=%u count=%d fb_pkt=%u",
                                    (unsigned)tw.base_seq, tw.item_count,
                                    (unsigned)tw.fb_pkt_count);
                        if (pc->bwe) {
                            for (int i = 0; i < tw.item_count; i++) {
                                const rtc_twcc_sent_pkt_t *s =
                                    rtc_twcc_sender_lookup(&pc->twcc_sender, tw.items[i].seq);
                                if (!s)
                                    continue;
                                uint64_t recv_us =
                                    tw.items[i].received ? tw.items[i].recv_time_us : 0;
                                rtc_bwe_on_packet_feedback(pc->bwe, s->send_time_us, recv_us,
                                                           s->size);
                            }
                        }
                    }
#endif /* MRTC_ENABLE_TWCC */
                } else if (pt == RTCP_PT_PSFB && fmt == RTCP_FMT_PLI) {
                    /* PLI — request keyframe */
                    rtc_rtcp_pli_t pli;
                    if (rtc_rtcp_parse_pli(&pli, buf, pkt_len) == RTC_OK) {
                        struct rtc_rtp_sender *sender =
                            (struct rtc_rtp_sender *)rtc_u32_map_get(&pc->send_map, pli.media_ssrc);
                        if (sender)
                            rtc_rtp_sender_handle_pli(sender);
                    }
                } else if (pt == RTCP_PT_PSFB && fmt == RTCP_FMT_FIR) {
                    /* FIR — request keyframe (same effect as PLI) */
                    rtc_rtcp_fir_t fir;
                    if (rtc_rtcp_parse_fir(&fir, buf, pkt_len) == RTC_OK) {
                        struct rtc_rtp_sender *sender =
                            (struct rtc_rtp_sender *)rtc_u32_map_get(&pc->send_map, fir.media_ssrc);
                        if (sender)
                            rtc_rtp_sender_handle_pli(sender);
                    }
                }
            }
            break;

        default:
            break;
    }
}
