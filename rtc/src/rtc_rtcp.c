/*
 * RTCP (RFC 3550) Sender/Receiver Reports.
 */
#include "rtc/rtc_rtcp.h"

#include <string.h>
#include <stdio.h>
#include <openssl/crypto.h>

static CRYPTO_ONCE cname_once = CRYPTO_ONCE_STATIC_INIT;
static char local_cname[33];

static void init_cname(void) {
    if (rtc_random_string(local_cname, sizeof(local_cname)) != RTC_OK) {
        local_cname[0] = '\0';
        RTC_LOG_ERR("RTCP CNAME generation failed");
    }
}

const char *rtc_rtcp_cname(void) {
    if (!CRYPTO_THREAD_run_once(&cname_once, init_cname) || !local_cname[0])
        return NULL;
    return local_cname;
}

static uint8_t fraction_lost(const rtc_rtcp_stats_t *stats) {
    uint32_t expected = stats->packets_expected - stats->expected_prior;
    uint32_t received = stats->packets_received - stats->received_prior;
    if (!expected || received >= expected)
        return 0;
    uint64_t fraction = ((uint64_t)(expected - received) << 8) / expected;
    return (uint8_t)(fraction > 255 ? 255 : fraction);
}

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

/* NTP timestamp (seconds since 1900-01-01) from the wall clock. */
static void get_ntp_time(uint64_t ms, uint32_t *sec, uint32_t *frac) {
    /* The NTP epoch (1900) precedes the Unix epoch (1970) by 2208988800 s. */
    *sec = (uint32_t)(ms / 1000) + 2208988800u;
    uint32_t ms_frac = (uint32_t)(ms % 1000);
    /* Convert ms fraction to NTP fraction (2^32 / 1000) */
    *frac = (uint32_t)(((uint64_t)ms_frac << 32) / 1000);
}

int rtc_rtcp_stats_init(rtc_rtcp_stats_t *stats, uint32_t ssrc) {
    if (!stats)
        return RTC_ERR_INVALID;
    memset(stats, 0, sizeof(*stats));
    stats->ssrc = ssrc;
    stats->first_packet = true;
    stats->report_members = 1;
    stats->report_initial = true;
    stats->last_report_time = rtc_time_ms();
    return RTC_OK;
}

int rtc_rtcp_stats_reset_source(rtc_rtcp_stats_t *stats) {
    if (!stats)
        return RTC_ERR_INVALID;
    rtc_rtcp_stats_t reset = {
        .ssrc = stats->ssrc,
        .packets_sent = stats->packets_sent,
        .octets_sent = stats->octets_sent,
        .rtp_timestamp = stats->rtp_timestamp,
        .send_clock_rate = stats->send_clock_rate,
        .send_time_ms = stats->send_time_ms,
        .send_clock_valid = stats->send_clock_valid,
        .last_report_time = stats->last_report_time,
        .next_report_time = stats->next_report_time,
        .report_target_bps = stats->report_target_bps,
        .last_report_size = stats->last_report_size,
        .report_members = stats->report_members,
        .report_senders = stats->report_senders,
        .report_pmembers = stats->report_pmembers,
        .report_we_sent = stats->report_we_sent,
        .report_initial = stats->report_initial,
        .report_previous_ms = stats->report_previous_ms,
        .report_average_size = stats->report_average_size,
        .first_packet = true,
    };
    *stats = reset;
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
    int16_t delta = (int16_t)(seq - (uint16_t)stats->highest_seq);
    if (delta > 0)
        stats->highest_seq += (uint16_t)delta;

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
            int64_t d = (int32_t)(transit - stats->last_transit);
            if (d < 0)
                d = -d;
            /* jitter += (1/16) * (|d| - jitter) */
            stats->jitter = (uint32_t)((int64_t)stats->jitter + (d - stats->jitter) / 16);
        }
        stats->last_transit = transit;
    }
}

void rtc_rtcp_stats_on_rtp_send(rtc_rtcp_stats_t *stats, uint32_t timestamp, size_t payload_len) {
    rtc_rtcp_stats_on_rtp_send_at(stats, timestamp, payload_len, rtc_time_ms());
}

void rtc_rtcp_stats_on_rtp_send_at(rtc_rtcp_stats_t *stats, uint32_t timestamp, size_t payload_len,
                                   uint64_t monotonic_ms) {
    if (!stats)
        return;
    stats->packets_sent++;
    stats->octets_sent += (uint32_t)payload_len;
    if (!stats->send_clock_valid || (int32_t)(timestamp - stats->rtp_timestamp) > 0) {
        stats->rtp_timestamp = timestamp;
        stats->send_time_ms = monotonic_ms;
        stats->send_clock_valid = true;
    }
    stats->report_we_sent = true;
    if (!stats->report_senders)
        stats->report_senders = 1;
}

void rtc_rtcp_stats_on_sr_recv(rtc_rtcp_stats_t *stats, const rtc_rtcp_sr_t *sr) {
    if (!stats || !sr)
        return;
    stats->last_sr_ntp = (sr->ntp_sec & 0xFFFF) << 16 | (sr->ntp_frac >> 16);
    stats->last_sr_recv_time = rtc_time_ms();
}

int rtc_rtcp_rtt_from_rr(const rtc_rtcp_rr_block_t *rr) {
    if (!rr || rr->last_sr == 0)
        return 0;
    /* last_sr echoes the middle 32 bits of an SR NTP we emitted, so compute the
     * current compact NTP on the same wall clock used by get_ntp_time(). */
    uint64_t now_ms = rtc_time_unix_ms();
    uint32_t now_sec = (uint32_t)(now_ms / 1000) + 2208988800u;
    uint32_t now_frac = (uint32_t)(((now_ms % 1000) << 32) / 1000);
    uint32_t now_compact = (now_sec & 0xFFFF) << 16 | (now_frac >> 16);
    uint32_t rtt_ntp = now_compact - rr->last_sr - rr->delay_since_sr;
    int rtt_ms = (int)((rtt_ntp * 1000ULL) >> 16);
    return rtt_ms < 0 ? 0 : rtt_ms;
}

int rtc_rtcp_build_sr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats) {
    uint64_t monotonic_ms = rtc_time_ms();
    return rtc_rtcp_build_sr_at(pkt, stats, monotonic_ms, rtc_time_unix_ms());
}

int rtc_rtcp_build_sr_at(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats,
                         uint64_t monotonic_ms, uint64_t unix_ms) {
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
    get_ntp_time(unix_ms, &pkt->sr.ntp_sec, &pkt->sr.ntp_frac);
    pkt->sr.rtp_timestamp = stats->rtp_timestamp;
    if (stats->send_clock_valid && stats->send_clock_rate && monotonic_ms >= stats->send_time_ms) {
        uint64_t elapsed = monotonic_ms - stats->send_time_ms;
        uint32_t seconds = (uint32_t)(elapsed / 1000);
        pkt->sr.rtp_timestamp += (uint32_t)((uint64_t)seconds * stats->send_clock_rate +
                                            (elapsed % 1000) * stats->send_clock_rate / 1000);
    }
    pkt->sr.sender_pkt_count = stats->packets_sent;
    pkt->sr.sender_oct_count = stats->octets_sent;

    /* Build reception report block if we have a remote source */
    if (rc_count > 0) {
        rtc_rtcp_rr_block_t *rr = &pkt->reports[0];
        rr->ssrc = stats->remote_ssrc;

        rr->fraction_lost = fraction_lost(stats);
        rr->cumulative_lost =
            (int32_t)(stats->packets_lost > 0x7fffff ? 0x7fffff : stats->packets_lost);
        rr->highest_seq = stats->highest_seq;
        rr->jitter = stats->jitter;
        rr->last_sr = stats->last_sr_ntp;
        if (stats->last_sr_recv_time > 0 && monotonic_ms >= stats->last_sr_recv_time) {
            uint64_t delay_ms = monotonic_ms - stats->last_sr_recv_time;
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
    write_u32(b + offset, pkt->sr.rtp_timestamp);
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

        rr->fraction_lost = fraction_lost(stats);
        rr->cumulative_lost =
            (int32_t)(stats->packets_lost > 0x7fffff ? 0x7fffff : stats->packets_lost);
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

int rtc_rtcp_build_report(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats, bool sender) {
    const char *cname = rtc_rtcp_cname();
    if (!cname)
        return RTC_ERR_SSL;
    int rc = sender ? rtc_rtcp_build_sr(pkt, stats) : rtc_rtcp_build_rr(pkt, stats);
    if (rc != RTC_OK)
        return rc;
    size_t name_len = strlen(cname);
    size_t chunk_len = (4 + 4 + 2 + name_len + 1 + 3) & ~(size_t)3;
    if (chunk_len > sizeof(pkt->buf) - pkt->buf_len)
        return RTC_ERR_NOMEM;
    uint8_t *chunk = pkt->buf + pkt->buf_len;
    memset(chunk, 0, chunk_len);
    chunk[0] = 0x81;
    chunk[1] = RTCP_PT_SDES;
    write_u16(chunk + 2, (uint16_t)(chunk_len / 4 - 1));
    write_u32(chunk + 4, stats->ssrc);
    chunk[8] = 1;
    chunk[9] = (uint8_t)name_len;
    memcpy(chunk + 10, cname, name_len);
    pkt->buf_len += chunk_len;
    return RTC_OK;
}

static double deterministic_interval_ms(uint32_t bandwidth_bps, size_t average_packet_bytes,
                                        unsigned members, unsigned senders, bool sending,
                                        bool initial) {
    if (!bandwidth_bps || !average_packet_bytes || !members || senders > members ||
        (sending && !senders)) {
        RTC_LOG_ERR("Invalid RTCP interval parameters");
        return 0;
    }
    double bandwidth = bandwidth_bps / 8.0;
    double sources = members;
    if (senders <= members * 0.25) {
        if (sending) {
            bandwidth *= 0.25;
            sources = senders;
        } else {
            bandwidth *= 0.75;
            sources = members - senders;
        }
    }
    double interval = (double)average_packet_bytes * sources * 1000.0 / bandwidth;
    double minimum = initial ? 2500.0 : 5000.0;
    if (interval < minimum)
        interval = minimum;
    return interval;
}

uint32_t rtc_rtcp_interval_ms(uint32_t bandwidth_bps, size_t average_packet_bytes, unsigned members,
                              unsigned senders, bool sending, bool initial, uint32_t random_value) {
    double interval = deterministic_interval_ms(bandwidth_bps, average_packet_bytes, members,
                                                senders, sending, initial);
    interval *= (0.5 + (double)random_value / UINT32_MAX) / 1.21828;
    return interval >= UINT32_MAX ? UINT32_MAX : (uint32_t)(interval + 0.5);
}

uint64_t rtc_rtcp_source_timeout_ms(const rtc_rtcp_stats_t *stats) {
    uint32_t target = stats && stats->report_target_bps ? stats->report_target_bps : 500000;
    size_t size = stats && stats->last_report_size ? stats->last_report_size : 128;
    if (stats && stats->report_average_size > 0)
        size = (size_t)(stats->report_average_size + 0.5);
    uint32_t budget = target / 20;
    unsigned members = stats && stats->report_members ? stats->report_members : 1;
    unsigned senders = stats ? stats->report_senders : 0;
    double interval =
        deterministic_interval_ms(budget ? budget : 1, size, members, senders, false, false);
    double timeout = interval * 5.0;
    return timeout >= 0x1p64 ? UINT64_MAX : (uint64_t)(timeout + 0.5);
}

static uint32_t report_interval(const rtc_rtcp_stats_t *stats, size_t packet_size,
                                uint32_t random_value) {
    uint32_t target = stats->report_target_bps ? stats->report_target_bps : 500000;
    uint32_t budget = target / 20;
    size_t average =
        stats->report_average_size > 0 ? (size_t)(stats->report_average_size + 0.5) : packet_size;
    return rtc_rtcp_interval_ms(budget ? budget : 1, average, stats->report_members,
                                stats->report_senders, stats->report_we_sent, stats->report_initial,
                                random_value);
}

static uint64_t report_deadline(double value) {
    return value >= 0x1p64 ? UINT64_MAX : value > 0 ? (uint64_t)value : 0;
}

int rtc_rtcp_report_due_at(rtc_rtcp_stats_t *stats, uint32_t media_target_bps, size_t report_size,
                           uint64_t now_ms, uint32_t random_value) {
    if (!stats || !report_size)
        return RTC_ERR_INVALID;
    stats->report_target_bps = media_target_bps;
    if (!stats->next_report_time) {
        stats->report_previous_ms = (double)now_ms;
        stats->report_pmembers = stats->report_members;
        uint32_t interval = report_interval(stats, report_size, random_value);
        if (!interval)
            return RTC_ERR_INVALID;
        stats->next_report_time = report_deadline((double)now_ms + interval);
        return RTC_ERR_TIMEOUT;
    }
    if (now_ms < stats->next_report_time)
        return RTC_ERR_TIMEOUT;
    uint32_t interval = report_interval(stats, report_size, random_value);
    if (!interval)
        return RTC_ERR_INVALID;
    stats->report_pmembers = stats->report_members;
    uint64_t next = report_deadline(stats->report_previous_ms + interval);
    if (next > now_ms) {
        stats->next_report_time = next;
        return RTC_ERR_TIMEOUT;
    }
    return RTC_OK;
}

int rtc_rtcp_report_due(rtc_rtcp_stats_t *stats, uint32_t media_target_bps, size_t report_size,
                        uint64_t now_ms) {
    if (!stats || !report_size)
        return RTC_ERR_INVALID;
    uint32_t random_value = 0;
    if ((!stats->next_report_time || now_ms >= stats->next_report_time) &&
        rtc_random_bytes((uint8_t *)&random_value, sizeof(random_value)) != RTC_OK) {
        RTC_LOG_ERR("RTCP report timer randomization failed");
        return RTC_ERR_SSL;
    }
    return rtc_rtcp_report_due_at(stats, media_target_bps, report_size, now_ms, random_value);
}

int rtc_rtcp_stats_report_sent(rtc_rtcp_stats_t *stats) {
    if (!stats)
        return RTC_ERR_INVALID;
    stats->expected_prior = stats->packets_expected;
    stats->received_prior = stats->packets_received;
    stats->last_report_time = rtc_time_ms();
    stats->report_previous_ms = (double)stats->last_report_time;
    stats->report_initial = false;
    stats->next_report_time = 0;
    int rc = rtc_rtcp_report_due(stats, stats->report_target_bps,
                                 stats->last_report_size ? stats->last_report_size : 128,
                                 stats->last_report_time);
    return rc == RTC_ERR_TIMEOUT ? RTC_OK : rc;
}

static int visit_sdes(rtc_rtcp_membership_t *group, const uint8_t *data, size_t len, bool local,
                      uint64_t now_ms) {
    size_t pos = 4;
    for (unsigned chunk = 0; chunk < (data[0] & 31); chunk++) {
        if (len - pos < 4)
            return RTC_ERR_INVALID;
        uint32_t ssrc = read_u32(data + pos);
        pos += 4;
        bool ended = false;
        while (pos < len) {
            uint8_t type = data[pos++];
            if (!type) {
                ended = true;
                break;
            }
            if (pos == len || data[pos] > len - pos - 1)
                return RTC_ERR_INVALID;
            pos += (size_t)data[pos] + 1;
        }
        if (!ended)
            return RTC_ERR_INVALID;
        while (pos % 4) {
            if (pos == len || data[pos++] != 0)
                return RTC_ERR_INVALID;
        }
        if (group) {
            int rc = rtc_rtcp_membership_touch(group, ssrc, local, false, now_ms);
            if (rc != RTC_OK)
                return rc;
        }
    }
    return pos == len ? RTC_OK : RTC_ERR_INVALID;
}

bool rtc_rtcp_membership_can_add(const rtc_rtcp_membership_t *group, uint32_t ssrc, bool local) {
    if (!group)
        return false;
    if (group->members < RTC_RTCP_MAX_MEMBERS)
        return true;
    for (size_t i = 0; i < RTC_RTCP_MAX_MEMBERS; i++)
        if (group->entries[i].used && group->entries[i].ssrc == ssrc &&
            group->entries[i].local == local)
            return true;
    return false;
}

int rtc_rtcp_membership_touch(rtc_rtcp_membership_t *group, uint32_t ssrc, bool local, bool sender,
                              uint64_t now_ms) {
    if (!group)
        return RTC_ERR_INVALID;
    rtc_rtcp_member_t *slot = NULL;
    for (size_t i = 0; i < RTC_RTCP_MAX_MEMBERS; i++) {
        rtc_rtcp_member_t *entry = &group->entries[i];
        if (entry->used && entry->ssrc == ssrc && entry->local == local) {
            slot = entry;
            break;
        }
        if (!entry->used && !slot)
            slot = entry;
    }
    if (!slot) {
        RTC_LOG_ERR("RTCP membership capacity exhausted");
        return RTC_ERR_NOMEM;
    }
    if (!slot->used) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->ssrc = ssrc;
        slot->local = local;
        group->members++;
    }
    slot->activity_ms = now_ms;
    if (sender) {
        if (!slot->sender)
            group->senders++;
        slot->sender = true;
        slot->rtp_ms = now_ms;
    }
    return RTC_OK;
}

static void membership_remove(rtc_rtcp_membership_t *group, uint32_t ssrc, bool local) {
    for (size_t i = 0; i < RTC_RTCP_MAX_MEMBERS; i++) {
        rtc_rtcp_member_t *entry = &group->entries[i];
        if (entry->used && entry->ssrc == ssrc && entry->local == local) {
            group->members--;
            if (entry->sender)
                group->senders--;
            memset(entry, 0, sizeof(*entry));
            return;
        }
    }
}

int rtc_rtcp_membership_on_rtcp(rtc_rtcp_membership_t *group, const uint8_t *data, size_t len,
                                bool local, size_t wire_size, uint64_t now_ms) {
    if (!group || wire_size < len || wire_size > 65583 ||
        rtc_rtcp_validate_compound(data, len) != RTC_OK)
        return RTC_ERR_INVALID;
    rtc_rtcp_membership_t updated = *group;
    for (size_t offset = 0; offset < len;) {
        const uint8_t *packet = data + offset;
        size_t size = ((size_t)read_u16(packet + 2) + 1) * 4;
        size_t body = size - ((packet[0] & 0x20) ? packet[size - 1] : 0);
        int rc = RTC_OK;
        if (packet[1] == RTCP_PT_BYE) {
            rtc_rtcp_bye_t bye;
            rc = rtc_rtcp_parse_bye(&bye, packet, size);
            if (rc == RTC_OK)
                for (unsigned i = 0; i < bye.count; i++)
                    membership_remove(&updated, bye.ssrcs[i], local);
        } else if (packet[1] == RTCP_PT_SDES) {
            rc = visit_sdes(&updated, packet, body, local, now_ms);
        } else if (packet[1] == RTCP_PT_SR || packet[1] == RTCP_PT_RR ||
                   packet[1] == RTCP_PT_RTPFB || packet[1] == RTCP_PT_PSFB) {
            rc = rtc_rtcp_membership_touch(&updated, read_u32(packet + 4), local, false, now_ms);
        }
        if (rc != RTC_OK)
            return rc;
        offset += size;
    }
    updated.average_size =
        updated.average_size > 0
            ? updated.average_size + ((double)wire_size - updated.average_size) / 16
            : (double)wire_size;
    *group = updated;
    return RTC_OK;
}

void rtc_rtcp_membership_expire(rtc_rtcp_membership_t *group, uint32_t media_target_bps,
                                uint64_t now_ms) {
    if (!group || !group->members)
        return;
    uint32_t budget = (media_target_bps ? media_target_bps : 500000) / 20;
    size_t size = group->average_size > 0 ? (size_t)(group->average_size + 0.5) : 128;
    double interval = deterministic_interval_ms(budget ? budget : 1, size, group->members,
                                                group->senders, false, false);
    uint64_t member_timeout = report_deadline(interval * 5);
    double sender_interval =
        group->senders ? deterministic_interval_ms(budget ? budget : 1, size, group->members,
                                                   group->senders, true, false)
                       : interval;
    uint64_t sender_timeout = report_deadline(sender_interval * 2);
    for (size_t i = 0; i < RTC_RTCP_MAX_MEMBERS; i++) {
        rtc_rtcp_member_t *entry = &group->entries[i];
        if (!entry->used)
            continue;
        if (now_ms >= entry->activity_ms && now_ms - entry->activity_ms >= member_timeout) {
            membership_remove(group, entry->ssrc, entry->local);
        } else if (entry->sender && now_ms >= entry->rtp_ms &&
                   now_ms - entry->rtp_ms >= sender_timeout) {
            entry->sender = false;
            group->senders--;
        }
    }
}

void rtc_rtcp_stats_set_membership(rtc_rtcp_stats_t *stats, const rtc_rtcp_membership_t *group,
                                   uint64_t now_ms) {
    if (!stats || !group)
        return;
    stats->report_members = group->members ? group->members : 1;
    stats->report_senders = group->senders;
    stats->report_we_sent = false;
    stats->report_average_size = group->average_size;
    for (size_t i = 0; i < RTC_RTCP_MAX_MEMBERS; i++)
        if (group->entries[i].used && group->entries[i].local &&
            group->entries[i].ssrc == stats->ssrc) {
            stats->report_we_sent = group->entries[i].sender;
            break;
        }
    if (stats->next_report_time && stats->report_members < stats->report_pmembers) {
        double ratio = (double)stats->report_members / stats->report_pmembers;
        stats->next_report_time = report_deadline(
            (double)now_ms + ratio * ((double)stats->next_report_time - (double)now_ms));
        stats->report_previous_ms =
            (double)now_ms - ratio * ((double)now_ms - stats->report_previous_ms);
        stats->report_pmembers = stats->report_members;
    }
}

int rtc_rtcp_build_bye(uint8_t *buf, size_t capacity, size_t *len, uint32_t ssrc) {
    if (!buf || !len || !ssrc)
        return RTC_ERR_INVALID;
    if (capacity < 8)
        return RTC_ERR_NOMEM;
    buf[0] = 0x81;
    buf[1] = RTCP_PT_BYE;
    write_u16(buf + 2, 1);
    write_u32(buf + 4, ssrc);
    *len = 8;
    return RTC_OK;
}

int rtc_rtcp_parse_bye(rtc_rtcp_bye_t *bye, const uint8_t *data, size_t len) {
    if (!bye || !data || len < 4 || (data[0] >> 6) != 2 || data[1] != RTCP_PT_BYE)
        return RTC_ERR_INVALID;
    size_t size = ((size_t)read_u16(data + 2) + 1) * 4;
    if (size > len)
        return RTC_ERR_INVALID;
    size_t end = size;
    if (data[0] & 0x20) {
        uint8_t padding = data[size - 1];
        if (!padding || padding > size - 4)
            return RTC_ERR_INVALID;
        end -= padding;
    }
    unsigned count = data[0] & 31;
    size_t sources_end = 4 + (size_t)count * 4;
    if (sources_end > end)
        return RTC_ERR_INVALID;
    if (sources_end < end) {
        size_t reason_end = sources_end + 1 + data[sources_end];
        if (reason_end > end || ((reason_end + 3) & ~(size_t)3) != end)
            return RTC_ERR_INVALID;
        for (size_t i = reason_end; i < end; i++)
            if (data[i])
                return RTC_ERR_INVALID;
    }
    rtc_rtcp_bye_t parsed = {.count = count};
    for (unsigned i = 0; i < count; i++)
        parsed.ssrcs[i] = read_u32(data + 4 + i * 4);
    *bye = parsed;
    return RTC_OK;
}

int rtc_rtcp_validate_compound(const uint8_t *data, size_t len) {
    if (!data || len < 4 || len % 4)
        return RTC_ERR_INVALID;
    size_t offset = 0;
    while (offset < len) {
        if (len - offset < 4 || (data[offset] >> 6) != 2 || data[offset + 1] < 192 ||
            data[offset + 1] > 223)
            return RTC_ERR_INVALID;
        size_t size = ((size_t)read_u16(data + offset + 2) + 1) * 4;
        if (size > len - offset)
            return RTC_ERR_INVALID;
        size_t body = size;
        if (data[offset] & 0x20) {
            uint8_t padding = data[offset + size - 1];
            if (offset + size != len || !padding || padding > size - 4)
                return RTC_ERR_INVALID;
            body -= padding;
        }
        if (data[offset + 1] == RTCP_PT_BYE) {
            rtc_rtcp_bye_t bye;
            if (rtc_rtcp_parse_bye(&bye, data + offset, size) != RTC_OK)
                return RTC_ERR_INVALID;
        } else if (data[offset + 1] == RTCP_PT_SR || data[offset + 1] == RTCP_PT_RR) {
            rtc_rtcp_packet_t report;
            if (rtc_rtcp_parse(&report, data + offset, size) != RTC_OK)
                return RTC_ERR_INVALID;
        } else if (data[offset + 1] == RTCP_PT_SDES) {
            if (visit_sdes(NULL, data + offset, body, false, 0) != RTC_OK)
                return RTC_ERR_INVALID;
        } else if ((data[offset + 1] == RTCP_PT_RTPFB || data[offset + 1] == RTCP_PT_PSFB) &&
                   body < 12) {
            return RTC_ERR_INVALID;
        }
        offset += size;
    }
    return RTC_OK;
}

int rtc_rtcp_compound_feedback(uint8_t *buf, size_t *len, size_t capacity) {
    if (!buf || !len || *len < 8 || *len > capacity || (buf[0] >> 6) != 2)
        return RTC_ERR_INVALID;
    if (buf[1] == RTCP_PT_SR || buf[1] == RTCP_PT_RR)
        return RTC_OK;
    if (buf[1] != RTCP_PT_RTPFB && buf[1] != RTCP_PT_PSFB && buf[1] != RTCP_PT_BYE)
        return RTC_ERR_INVALID;
    size_t offset = 0;
    while (offset < *len) {
        if (*len - offset < 4 || (buf[offset] >> 6) != 2)
            return RTC_ERR_INVALID;
        size_t size = ((size_t)read_u16(buf + offset + 2) + 1) * 4;
        if (size > *len - offset)
            return RTC_ERR_INVALID;
        offset += size;
    }
    rtc_rtcp_stats_t stats = {.ssrc = read_u32(buf + 4), .first_packet = true};
    rtc_rtcp_packet_t prefix;
    int rc = rtc_rtcp_build_report(&prefix, &stats, false);
    if (rc != RTC_OK)
        return rc;
    if (prefix.buf_len > capacity - *len || prefix.buf_len + *len > RTCP_MAX_PACKET)
        return RTC_ERR_NOMEM;
    memmove(buf + prefix.buf_len, buf, *len);
    memcpy(buf, prefix.buf, prefix.buf_len);
    *len += prefix.buf_len;
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
    if (expected_len < 8 || expected_len > len ||
        (pkt->header.packet_type != RTCP_PT_SR && pkt->header.packet_type != RTCP_PT_RR))
        return RTC_ERR_INVALID;
    size_t content_len = expected_len;
    if (pkt->header.padding) {
        uint8_t padding = data[expected_len - 1];
        if (!padding || padding > expected_len - 8)
            return RTC_ERR_INVALID;
        content_len -= padding;
    }
    size_t required = 8 + (pkt->header.packet_type == RTCP_PT_SR ? RTCP_SR_SIZE : 0) +
                      (size_t)pkt->header.count * RTCP_RR_BLOCK_SIZE;
    if (required != content_len)
        return RTC_ERR_INVALID;
    len = content_len;

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
    if (expected_len <= sizeof(pkt->buf)) {
        memcpy(pkt->buf, data, expected_len);
        pkt->buf_len = expected_len;
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

int rtc_rtcp_build_remb(uint8_t *buf, size_t capacity, size_t *len, uint32_t sender_ssrc,
                        uint32_t media_ssrc, uint32_t bitrate_bps) {
    if (!buf || !len || !media_ssrc)
        return RTC_ERR_INVALID;
    if (capacity < 24)
        return RTC_ERR_NOMEM;
    memset(buf, 0, 24);
    buf[0] = 0x8f;
    buf[1] = RTCP_PT_PSFB;
    write_u16(buf + 2, 5);
    write_u32(buf + 4, sender_ssrc);
    memcpy(buf + 12, "REMB", 4);
    buf[16] = 1;
    unsigned exponent = 0;
    uint32_t mantissa = bitrate_bps;
    while (mantissa > 0x3ffff) {
        mantissa >>= 1;
        exponent++;
    }
    buf[17] = (uint8_t)((exponent << 2) | (mantissa >> 16));
    buf[18] = (uint8_t)(mantissa >> 8);
    buf[19] = (uint8_t)mantissa;
    write_u32(buf + 20, media_ssrc);
    *len = 24;
    return RTC_OK;
}

int rtc_rtcp_parse_remb(rtc_rtcp_remb_t *out, const uint8_t *data, size_t len) {
    if (!out || !data || len < 24 || data[0] != 0x8f || data[1] != RTCP_PT_PSFB ||
        read_u16(data + 2) != 5 || read_u32(data + 8) != 0 || memcmp(data + 12, "REMB", 4) ||
        data[16] != 1)
        return RTC_ERR_INVALID;
    unsigned exponent = data[17] >> 2;
    uint32_t mantissa = ((uint32_t)(data[17] & 3) << 16) | ((uint32_t)data[18] << 8) | data[19];
    out->sender_ssrc = read_u32(data + 4);
    out->media_ssrc = read_u32(data + 20);
    out->bitrate_bps = !mantissa ? 0
                       : exponent >= 32 || mantissa > (UINT32_MAX >> exponent)
                           ? UINT32_MAX
                           : mantissa << exponent;
    return out->media_ssrc ? RTC_OK : RTC_ERR_INVALID;
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
