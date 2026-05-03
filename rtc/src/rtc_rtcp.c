/*
 * rtc_rtcp.c - RTCP (RFC 3550) Sender/Receiver Reports.
 */
#include "rtc_rtcp.h"

#include <string.h>
#include <stdio.h>

/* ---------- Helpers ---------- */

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* Get NTP timestamp (seconds since 1900-01-01). We approximate from ms clock. */
static void get_ntp_time(uint32_t *sec, uint32_t *frac) {
    uint64_t ms = rtc_time_ms();
    /* NTP epoch offset from Unix: 2208988800 seconds.
     * We use monotonic clock which is not wall-clock but sufficient for RTCP. */
    *sec = (uint32_t)(ms / 1000);
    uint32_t ms_frac = (uint32_t)(ms % 1000);
    /* Convert ms fraction to NTP fraction (2^32 / 1000) */
    *frac = (uint32_t)((uint64_t)ms_frac * 4294967ULL);
}

/* ---------- Statistics ---------- */

int rtc_rtcp_stats_init(rtc_rtcp_stats_t *stats, uint32_t ssrc) {
    if (!stats)
        return RTC_ERR_INVALID;
    memset(stats, 0, sizeof(*stats));
    stats->ssrc = ssrc;
    stats->first_packet = true;
    stats->last_report_time = rtc_time_ms();
    return RTC_OK;
}

void rtc_rtcp_stats_on_rtp_recv(rtc_rtcp_stats_t *stats, uint16_t seq, uint32_t timestamp,
                                uint32_t ssrc, uint32_t clock_rate) {
    if (!stats)
        return;

    if (stats->first_packet) {
        stats->remote_ssrc = ssrc;
        stats->highest_seq = seq;
        stats->first_packet = false;
        stats->packets_received = 1;
        stats->packets_expected = 1;
        return;
    }

    /* Only track the configured remote SSRC */
    if (ssrc != stats->remote_ssrc)
        return;

    stats->packets_received++;

    /* Extended highest sequence number */
    uint32_t seq32 = (stats->highest_seq & 0xFFFF0000u) | seq;
    if (seq < (stats->highest_seq & 0xFFFF)) {
        /* Possible wraparound */
        if ((stats->highest_seq & 0xFFFF) - seq > 0x8000)
            seq32 += 0x10000; /* sequence number wrapped */
    }
    if (seq32 > stats->highest_seq)
        stats->highest_seq = seq32;

    /* Expected packets = highest_seq - first_seq + 1 (simplified) */
    stats->packets_expected = stats->highest_seq + 1;

    /* Packet loss */
    if (stats->packets_expected > stats->packets_received)
        stats->packets_lost = stats->packets_expected - stats->packets_received;
    else
        stats->packets_lost = 0;

    /* Interarrival jitter (RFC 3550 A.8) */
    if (clock_rate > 0) {
        uint64_t arrival_ms = rtc_time_ms();
        uint32_t arrival_ts = (uint32_t)((arrival_ms * clock_rate) / 1000);
        uint32_t transit = arrival_ts - timestamp;

        if (stats->last_transit != 0) {
            int32_t d = (int32_t)(transit - stats->last_transit);
            if (d < 0)
                d = -d;
            /* jitter += (1/16) * (|d| - jitter) */
            stats->jitter += ((uint32_t)d - stats->jitter + 8) >> 4;
        }
        stats->last_transit = transit;
    }
}

void rtc_rtcp_stats_on_rtp_send(rtc_rtcp_stats_t *stats, uint32_t timestamp, size_t payload_len) {
    if (!stats)
        return;
    stats->packets_sent++;
    stats->octets_sent += (uint32_t)payload_len;
    stats->rtp_timestamp = timestamp;
}

/* ---------- Build Sender Report ---------- */

int rtc_rtcp_build_sr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats) {
    if (!pkt || !stats)
        return RTC_ERR_INVALID;

    memset(pkt, 0, sizeof(*pkt));
    pkt->header.version = 2;
    pkt->header.padding = false;
    pkt->header.packet_type = RTCP_PT_SR;

    int rc_count = (stats->remote_ssrc != 0 && !stats->first_packet) ? 1 : 0;
    pkt->header.count = (uint8_t)rc_count;
    pkt->report_count = rc_count;
    pkt->sender_ssrc = stats->ssrc;

    /* Sender info */
    pkt->sr.ssrc = stats->ssrc;
    get_ntp_time(&pkt->sr.ntp_sec, &pkt->sr.ntp_frac);
    pkt->sr.rtp_timestamp = stats->rtp_timestamp;
    pkt->sr.sender_pkt_count = stats->packets_sent;
    pkt->sr.sender_oct_count = stats->octets_sent;

    /* Build reception report block if we have a remote source */
    if (rc_count > 0) {
        rtc_rtcp_rr_block_t *rr = &pkt->reports[0];
        rr->ssrc = stats->remote_ssrc;

        if (stats->packets_expected > 0) {
            uint32_t lost = stats->packets_lost;
            uint32_t expected = stats->packets_expected;
            rr->fraction_lost = (uint8_t)((lost * 256) / expected);
        }
        rr->cumulative_lost = (int32_t)stats->packets_lost;
        rr->highest_seq = stats->highest_seq;
        rr->jitter = stats->jitter;
        rr->last_sr = stats->last_sr_ntp;
        if (stats->last_sr_recv_time > 0) {
            uint64_t delay_ms = rtc_time_ms() - stats->last_sr_recv_time;
            rr->delay_since_sr = (uint32_t)((delay_ms * 65536) / 1000);
        }
    }

    /* Serialize */
    uint8_t *b = pkt->buf;
    size_t offset = 0;

    /* Header */
    b[0] = (2 << 6) | ((uint8_t)rc_count & 0x1F);
    b[1] = RTCP_PT_SR;
    offset = 4; /* length filled later */

    /* SSRC */
    write_u32(b + offset, stats->ssrc);
    offset += 4;

    /* NTP timestamp */
    write_u32(b + offset, pkt->sr.ntp_sec);
    offset += 4;
    write_u32(b + offset, pkt->sr.ntp_frac);
    offset += 4;

    /* RTP timestamp */
    write_u32(b + offset, stats->rtp_timestamp);
    offset += 4;

    /* Sender packet count */
    write_u32(b + offset, stats->packets_sent);
    offset += 4;

    /* Sender octet count */
    write_u32(b + offset, stats->octets_sent);
    offset += 4;

    /* Reception report blocks */
    for (int i = 0; i < rc_count; i++) {
        const rtc_rtcp_rr_block_t *rr = &pkt->reports[i];
        write_u32(b + offset, rr->ssrc);
        offset += 4;

        /* Fraction lost (8 bits) + cumulative lost (24 bits) */
        uint32_t lost_field =
            ((uint32_t)rr->fraction_lost << 24) | ((uint32_t)rr->cumulative_lost & 0x00FFFFFF);
        write_u32(b + offset, lost_field);
        offset += 4;

        write_u32(b + offset, rr->highest_seq);
        offset += 4;
        write_u32(b + offset, rr->jitter);
        offset += 4;
        write_u32(b + offset, rr->last_sr);
        offset += 4;
        write_u32(b + offset, rr->delay_since_sr);
        offset += 4;
    }

    /* Fill in length (32-bit words minus 1, not counting header word) */
    uint16_t length_words = (uint16_t)((offset / 4) - 1);
    pkt->header.length = length_words;
    write_u16(b + 2, length_words);

    pkt->buf_len = offset;
    return RTC_OK;
}

/* ---------- Build Receiver Report ---------- */

int rtc_rtcp_build_rr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats) {
    if (!pkt || !stats)
        return RTC_ERR_INVALID;

    memset(pkt, 0, sizeof(*pkt));
    pkt->header.version = 2;
    pkt->header.padding = false;
    pkt->header.packet_type = RTCP_PT_RR;

    int rc_count = (stats->remote_ssrc != 0 && !stats->first_packet) ? 1 : 0;
    pkt->header.count = (uint8_t)rc_count;
    pkt->report_count = rc_count;
    pkt->sender_ssrc = stats->ssrc;

    /* Build reception report block */
    if (rc_count > 0) {
        rtc_rtcp_rr_block_t *rr = &pkt->reports[0];
        rr->ssrc = stats->remote_ssrc;

        if (stats->packets_expected > 0) {
            uint32_t lost = stats->packets_lost;
            uint32_t expected = stats->packets_expected;
            rr->fraction_lost = (uint8_t)((lost * 256) / expected);
        }
        rr->cumulative_lost = (int32_t)stats->packets_lost;
        rr->highest_seq = stats->highest_seq;
        rr->jitter = stats->jitter;
        rr->last_sr = stats->last_sr_ntp;
        if (stats->last_sr_recv_time > 0) {
            uint64_t delay_ms = rtc_time_ms() - stats->last_sr_recv_time;
            rr->delay_since_sr = (uint32_t)((delay_ms * 65536) / 1000);
        }
    }

    /* Serialize */
    uint8_t *b = pkt->buf;
    size_t offset = 0;

    b[0] = (2 << 6) | ((uint8_t)rc_count & 0x1F);
    b[1] = RTCP_PT_RR;
    offset = 4;

    /* SSRC of sender */
    write_u32(b + offset, stats->ssrc);
    offset += 4;

    /* Reception report blocks */
    for (int i = 0; i < rc_count; i++) {
        const rtc_rtcp_rr_block_t *rr = &pkt->reports[i];
        write_u32(b + offset, rr->ssrc);
        offset += 4;

        uint32_t lost_field =
            ((uint32_t)rr->fraction_lost << 24) | ((uint32_t)rr->cumulative_lost & 0x00FFFFFF);
        write_u32(b + offset, lost_field);
        offset += 4;

        write_u32(b + offset, rr->highest_seq);
        offset += 4;
        write_u32(b + offset, rr->jitter);
        offset += 4;
        write_u32(b + offset, rr->last_sr);
        offset += 4;
        write_u32(b + offset, rr->delay_since_sr);
        offset += 4;
    }

    uint16_t length_words = (uint16_t)((offset / 4) - 1);
    pkt->header.length = length_words;
    write_u16(b + 2, length_words);

    pkt->buf_len = offset;
    return RTC_OK;
}

/* ---------- Parse ---------- */

int rtc_rtcp_parse(rtc_rtcp_packet_t *pkt, const uint8_t *data, size_t len) {
    if (!pkt || !data)
        return RTC_ERR_INVALID;
    if (len < 8)
        return RTC_ERR_INVALID; /* minimum: header + SSRC */

    memset(pkt, 0, sizeof(*pkt));

    /* Common header */
    pkt->header.version = (data[0] >> 6) & 0x03;
    if (pkt->header.version != 2)
        return RTC_ERR_INVALID;

    pkt->header.padding = (data[0] >> 5) & 0x01;
    pkt->header.count = data[0] & 0x1F;
    pkt->header.packet_type = data[1];
    pkt->header.length = read_u16(data + 2);

    /* Verify length */
    size_t expected_len = ((size_t)pkt->header.length + 1) * 4;
    if (expected_len > len)
        return RTC_ERR_INVALID;

    /* SSRC of sender */
    pkt->sender_ssrc = read_u32(data + 4);

    size_t offset = 8;

    if (pkt->header.packet_type == RTCP_PT_SR) {
        if (offset + RTCP_SR_SIZE > len)
            return RTC_ERR_INVALID;

        pkt->sr.ssrc = pkt->sender_ssrc;
        pkt->sr.ntp_sec = read_u32(data + offset);
        offset += 4;
        pkt->sr.ntp_frac = read_u32(data + offset);
        offset += 4;
        pkt->sr.rtp_timestamp = read_u32(data + offset);
        offset += 4;
        pkt->sr.sender_pkt_count = read_u32(data + offset);
        offset += 4;
        pkt->sr.sender_oct_count = read_u32(data + offset);
        offset += 4;
    }

    /* Parse reception report blocks */
    int rc = (int)pkt->header.count;
    pkt->report_count = 0;
    for (int i = 0; i < rc && i < 31; i++) {
        if (offset + RTCP_RR_BLOCK_SIZE > len)
            break;

        rtc_rtcp_rr_block_t *rr = &pkt->reports[i];
        rr->ssrc = read_u32(data + offset);
        offset += 4;

        uint32_t lost_field = read_u32(data + offset);
        offset += 4;
        rr->fraction_lost = (uint8_t)(lost_field >> 24);
        rr->cumulative_lost = (int32_t)(lost_field & 0x00FFFFFF);
        /* Sign-extend 24-bit value */
        if (rr->cumulative_lost & 0x800000)
            rr->cumulative_lost |= (int32_t)0xFF000000;

        rr->highest_seq = read_u32(data + offset);
        offset += 4;
        rr->jitter = read_u32(data + offset);
        offset += 4;
        rr->last_sr = read_u32(data + offset);
        offset += 4;
        rr->delay_since_sr = read_u32(data + offset);
        offset += 4;

        pkt->report_count++;
    }

    /* Copy raw data */
    if (len <= sizeof(pkt->buf)) {
        memcpy(pkt->buf, data, len);
        pkt->buf_len = len;
    }

    return RTC_OK;
}

/* ---------- RTCP detection ---------- */

bool rtc_rtcp_is_rtcp(const uint8_t *data, size_t len) {
    if (len < 4)
        return false;

    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
        return false;

    uint8_t pt = data[1];
    /* RTCP payload types: 200-204 (SR, RR, SDES, BYE, APP) */
    return (pt >= 200 && pt <= 204);
}
