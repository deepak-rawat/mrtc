/*
 * RTCP (RFC 3550) Sender/Receiver Reports.
 */
#include "rtc/rtc_rtcp.h"

#include <string.h>
#include <stdio.h>

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
        stats->base_seq = seq;
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

    /* Expected packets (RFC 3550 §A.3): highest_seq - base_seq + 1.
     * The base_seq itself is an extended 32-bit value (the upper 16 bits are
     * the rollover counter, which is 0 at session start). */
    stats->packets_expected = stats->highest_seq - stats->base_seq + 1;

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

bool rtc_rtcp_is_rtcp(const uint8_t *data, size_t len) {
    if (len < 4)
        return false;

    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
        return false;

    uint8_t pt = data[1];
    /* RTCP payload types: 200-204 (SR, RR, SDES, BYE, APP) + 205-206 (RTPFB, PSFB) */
    return (pt >= 200 && pt <= 206);
}

bool rtc_rtcp_get_pt_fmt(const uint8_t *data, size_t len, uint8_t *pt, uint8_t *fmt) {
    if (!data || len < 4)
        return false;
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
        return false;
    *pt = data[1];
    *fmt = data[0] & 0x1F;
    return true;
}

/* NACK build (RFC 4585 §6.2.1). */

/* Sort helper for uint16_t */
static int cmp_u16(const void *a, const void *b) {
    uint16_t va = *(const uint16_t *)a;
    uint16_t vb = *(const uint16_t *)b;
    return (va > vb) - (va < vb);
}

int rtc_rtcp_build_nack(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t sender_ssrc,
                        uint32_t media_ssrc, const uint16_t *lost_seqs, int count) {
    if (!buf || !out_len || !lost_seqs || count <= 0)
        return RTC_ERR_INVALID;

    /* Sort the lost sequence numbers to coalesce into PID+BLP */
    uint16_t sorted[RTCP_NACK_MAX_SEQS];
    if (count > RTCP_NACK_MAX_SEQS)
        count = RTCP_NACK_MAX_SEQS;
    memcpy(sorted, lost_seqs, (size_t)count * sizeof(uint16_t));
    qsort(sorted, (size_t)count, sizeof(uint16_t), cmp_u16);

    /* Build FCI entries: each PID + 16-bit BLP bitmask */
    typedef struct {
        uint16_t pid;
        uint16_t blp;
    } nack_fci_t;
    nack_fci_t fcis[RTCP_NACK_MAX_FCIS];
    int fci_count = 0;

    int i = 0;
    while (i < count && fci_count < RTCP_NACK_MAX_FCIS) {
        uint16_t pid = sorted[i++];
        uint16_t blp = 0;
        while (i < count) {
            int diff = (int)sorted[i] - (int)pid;
            if (diff >= 1 && diff <= 16) {
                blp |= (uint16_t)(1 << (diff - 1));
                i++;
            } else {
                break;
            }
        }
        fcis[fci_count].pid = pid;
        fcis[fci_count].blp = blp;
        fci_count++;
    }

    /* Packet size: header(4) + sender_ssrc(4) + media_ssrc(4) + fcis(4 each) */
    size_t pkt_len = 12 + (size_t)fci_count * 4;
    if (buf_cap < pkt_len)
        return RTC_ERR_INVALID;

    /* Header: V=2, P=0, FMT=1, PT=205 */
    buf[0] = (2 << 6) | RTCP_FMT_NACK;
    buf[1] = RTCP_PT_RTPFB;
    uint16_t length_words = (uint16_t)(pkt_len / 4 - 1);
    write_u16(buf + 2, length_words);

    write_u32(buf + 4, sender_ssrc);
    write_u32(buf + 8, media_ssrc);

    for (int j = 0; j < fci_count; j++) {
        size_t off = 12 + (size_t)j * 4;
        write_u16(buf + off, fcis[j].pid);
        write_u16(buf + off + 2, fcis[j].blp);
    }

    *out_len = pkt_len;
    return RTC_OK;
}

int rtc_rtcp_parse_nack(rtc_rtcp_nack_t *out, const uint8_t *data, size_t len) {
    if (!out || !data)
        return RTC_ERR_INVALID;
    if (len < 12)
        return RTC_ERR_INVALID;

    memset(out, 0, sizeof(*out));

    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
        return RTC_ERR_INVALID;

    out->sender_ssrc = read_u32(data + 4);
    out->media_ssrc = read_u32(data + 8);

    /* Parse FCI entries starting at offset 12 */
    size_t offset = 12;
    out->lost_count = 0;
    while (offset + 4 <= len && out->lost_count < RTCP_NACK_MAX_SEQS) {
        uint16_t pid = read_u16(data + offset);
        uint16_t blp = read_u16(data + offset + 2);
        offset += 4;

        out->lost_seqs[out->lost_count++] = pid;
        for (int bit = 0; bit < 16 && out->lost_count < RTCP_NACK_MAX_SEQS; bit++) {
            if (blp & (1 << bit))
                out->lost_seqs[out->lost_count++] = (uint16_t)(pid + bit + 1);
        }
    }

    return RTC_OK;
}

/* PLI build (RFC 4585 §6.3.1). */
int rtc_rtcp_build_pli(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t sender_ssrc,
                       uint32_t media_ssrc) {
    if (!buf || !out_len)
        return RTC_ERR_INVALID;
    if (buf_cap < 12)
        return RTC_ERR_INVALID;

    /* Fixed 12 bytes: header(4) + sender_ssrc(4) + media_ssrc(4) */
    buf[0] = (2 << 6) | RTCP_FMT_PLI;
    buf[1] = RTCP_PT_PSFB;
    write_u16(buf + 2, 2); /* length = 2 (12 bytes / 4 - 1) */
    write_u32(buf + 4, sender_ssrc);
    write_u32(buf + 8, media_ssrc);

    *out_len = 12;
    return RTC_OK;
}

int rtc_rtcp_parse_pli(rtc_rtcp_pli_t *out, const uint8_t *data, size_t len) {
    if (!out || !data)
        return RTC_ERR_INVALID;
    if (len < 12)
        return RTC_ERR_INVALID;

    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
        return RTC_ERR_INVALID;

    out->sender_ssrc = read_u32(data + 4);
    out->media_ssrc = read_u32(data + 8);
    return RTC_OK;
}

/* FIR build (RFC 5104 §4.3.1). */
int rtc_rtcp_build_fir(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t sender_ssrc,
                       uint32_t media_ssrc, uint8_t seq_nr) {
    if (!buf || !out_len)
        return RTC_ERR_INVALID;
    /* header(4) + sender_ssrc(4) + media_ssrc(4, unused=0) + FCI(8) = 20 bytes */
    if (buf_cap < 20)
        return RTC_ERR_INVALID;

    buf[0] = (2 << 6) | RTCP_FMT_FIR;
    buf[1] = RTCP_PT_PSFB;
    write_u16(buf + 2, 4); /* length = 4 (20 bytes / 4 - 1) */
    write_u32(buf + 4, sender_ssrc);
    write_u32(buf + 8, 0); /* media source SSRC unused in FIR, set to 0 */

    /* FCI: SSRC(4) + Seq Nr(1) + Reserved(3) */
    write_u32(buf + 12, media_ssrc);
    buf[16] = seq_nr;
    buf[17] = 0;
    buf[18] = 0;
    buf[19] = 0;

    *out_len = 20;
    return RTC_OK;
}

int rtc_rtcp_parse_fir(rtc_rtcp_fir_t *out, const uint8_t *data, size_t len) {
    if (!out || !data)
        return RTC_ERR_INVALID;
    if (len < 20)
        return RTC_ERR_INVALID;

    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
        return RTC_ERR_INVALID;

    out->sender_ssrc = read_u32(data + 4);
    /* FCI starts at offset 12 */
    out->media_ssrc = read_u32(data + 12);
    out->seq_nr = data[16];
    return RTC_OK;
}

#define TWCC_STATUS_NOT_RECV    0
#define TWCC_STATUS_SMALL_DELTA 1
#define TWCC_STATUS_LARGE_DELTA 2
#define TWCC_TICK_US            250

int rtc_rtcp_parse_twcc(rtc_rtcp_twcc_t *out, const uint8_t *data, size_t len) {
    if (!out || !data)
        return RTC_ERR_INVALID;
    if (len < 20)
        return RTC_ERR_INVALID;
    if (((data[0] >> 6) & 0x3) != 2)
        return RTC_ERR_INVALID;

    memset(out, 0, sizeof(*out));
    out->sender_ssrc = read_u32(data + 4);
    out->media_ssrc = read_u32(data + 8);
    out->base_seq = read_u16(data + 12);
    uint16_t status_count = read_u16(data + 14);
    uint32_t ref_time_64ms = ((uint32_t)data[16] << 16) | ((uint32_t)data[17] << 8) | data[18];
    out->fb_pkt_count = data[19];
    out->reference_time_us = (uint64_t)ref_time_64ms * 64000ULL;

    if (status_count == 0 || status_count > RTC_TWCC_PARSE_MAX)
        return RTC_ERR_INVALID;
    out->item_count = status_count;
    for (int i = 0; i < status_count; i++)
        out->items[i].seq = (uint16_t)(out->base_seq + i);

    /* Walk chunks to assign per-packet status into a scratch buffer. */
    uint8_t status[RTC_TWCC_PARSE_MAX];
    size_t p = 20;
    int filled = 0;
    while (filled < status_count) {
        if (p + 2 > len)
            return RTC_ERR_INVALID;
        uint16_t chunk = read_u16(data + p);
        p += 2;
        if ((chunk & 0x8000) == 0) {
            /* Run-length: T=0, S=2 bits, L=13 bits */
            uint8_t s = (chunk >> 13) & 0x3;
            int run = chunk & 0x1FFF;
            for (int k = 0; k < run && filled < status_count; k++)
                status[filled++] = s;
        } else {
            /* Status-vector chunk */
            int sbit = (chunk >> 14) & 0x1;
            if (sbit == 0) {
                /* 14 × 1-bit symbols (0 = not recv, 1 = small delta) */
                for (int k = 0; k < 14 && filled < status_count; k++) {
                    int v = (chunk >> (13 - k)) & 0x1;
                    status[filled++] = v ? TWCC_STATUS_SMALL_DELTA : TWCC_STATUS_NOT_RECV;
                }
            } else {
                /* 7 × 2-bit symbols */
                for (int k = 0; k < 7 && filled < status_count; k++) {
                    uint8_t v = (chunk >> (12 - 2 * k)) & 0x3;
                    status[filled++] = v;
                }
            }
        }
    }

    /* Read per-packet receive deltas (250µs units), accumulating absolute time. */
    uint64_t cur_us = out->reference_time_us;
    for (int i = 0; i < status_count; i++) {
        if (status[i] == TWCC_STATUS_NOT_RECV) {
            out->items[i].received = false;
            continue;
        }
        if (status[i] == TWCC_STATUS_SMALL_DELTA) {
            if (p + 1 > len)
                return RTC_ERR_INVALID;
            int8_t d = (int8_t)data[p++];
            cur_us = (uint64_t)((int64_t)cur_us + (int64_t)d * TWCC_TICK_US);
        } else if (status[i] == TWCC_STATUS_LARGE_DELTA) {
            if (p + 2 > len)
                return RTC_ERR_INVALID;
            int16_t d = (int16_t)read_u16(data + p);
            p += 2;
            cur_us = (uint64_t)((int64_t)cur_us + (int64_t)d * TWCC_TICK_US);
        } else {
            return RTC_ERR_INVALID;
        }
        out->items[i].received = true;
        out->items[i].recv_time_us = cur_us;
    }

    return RTC_OK;
}
