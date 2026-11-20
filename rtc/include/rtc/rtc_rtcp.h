/*
 * RTCP (RFC 3550) - Minimal Sender/Receiver Reports.
 *
 * Supports:
 *  - Sender Report (SR, PT=200)
 *  - Receiver Report (RR, PT=201)
 *  - Building and parsing RTCP packets
 *  - Jitter/loss statistics tracking
 */
#ifndef RTC_RTCP_H
#define RTC_RTCP_H

#include "rtc_common.h"

/* RTCP packet types (RFC 3550 section 12.1) */
#define RTCP_PT_SR    200
#define RTCP_PT_RR    201
#define RTCP_PT_SDES  202
#define RTCP_PT_BYE   203
#define RTCP_PT_RTPFB 205 /* Transport layer feedback (RFC 4585) */
#define RTCP_PT_PSFB  206 /* Payload-specific feedback (RFC 4585) */

/* Feedback Message Types — FMT field in header (RFC 4585 / RFC 5104) */
#define RTCP_FMT_NACK 1 /* Generic NACK (RTPFB, RFC 4585 §6.2.1) */
#define RTCP_FMT_PLI  1 /* Picture Loss Indication (PSFB, RFC 4585 §6.3.1) */
#define RTCP_FMT_FIR  4 /* Full Intra Request (PSFB, RFC 5104 §4.3.1) */

/* NACK limits */
#define RTCP_NACK_MAX_FCIS 64
#define RTCP_NACK_MAX_SEQS (RTCP_NACK_MAX_FCIS * 17)

/* RTCP header size (common part: V, P, RC, PT, length) */
#define RTCP_HEADER_SIZE     4
#define RTCP_SR_SIZE         20 /* Sender info (NTP ts + RTP ts + counts), not including SSRC */
#define RTCP_RR_BLOCK_SIZE   24 /* Per-source reception report block */
#define RTCP_MAX_PACKET      1400
#define RTC_RTCP_MAX_MEMBERS 128
#define RTC_RTCP_RECONSIDER_POLL_MS 100
#define RTC_RTCP_SR_HISTORY         8

typedef struct {
    uint32_t ssrc;
    bool used, local, sender;
    uint64_t activity_ms, rtp_ms;
} rtc_rtcp_member_t;

typedef struct {
    rtc_rtcp_member_t entries[RTC_RTCP_MAX_MEMBERS];
    unsigned members, senders;
    double average_size;
} rtc_rtcp_membership_t;

/* RTCP common header */
typedef struct {
    uint8_t version; /* always 2 */
    bool padding;
    uint8_t count;       /* RC or SC */
    uint8_t packet_type; /* 200-203 */
    uint16_t length;     /* length in 32-bit words minus 1 */
} rtc_rtcp_header_t;

/* Sender Report sender info block */
typedef struct {
    uint32_t ssrc;
    uint32_t ntp_sec;  /* NTP timestamp (seconds) */
    uint32_t ntp_frac; /* NTP timestamp (fraction) */
    uint32_t rtp_timestamp;
    uint32_t sender_pkt_count;
    uint32_t sender_oct_count;
} rtc_rtcp_sr_t;

/* Reception Report block (used in both SR and RR) */
typedef struct {
    uint32_t ssrc;           /* SSRC of source being reported */
    uint8_t fraction_lost;   /* fraction lost since last SR/RR */
    int32_t cumulative_lost; /* 24-bit signed cumulative packets lost */
    uint32_t highest_seq;    /* highest sequence number received (with cycles) */
    uint32_t jitter;         /* interarrival jitter */
    uint32_t last_sr;        /* last SR timestamp (middle 32 bits of NTP) */
    uint32_t delay_since_sr; /* delay since last SR in 1/65536 seconds */
} rtc_rtcp_rr_block_t;

/* RTCP packet (SR or RR) */
typedef struct {
    rtc_rtcp_header_t header;
    uint32_t sender_ssrc; /* SSRC of sender of this report */

    /* SR-specific sender info (only valid if header.packet_type == RTCP_PT_SR) */
    rtc_rtcp_sr_t sr;

    /* Reception report blocks (0 or more) */
    rtc_rtcp_rr_block_t reports[31]; /* max RC = 5 bits = 31 */
    int report_count;

    /* Serialized form */
    uint8_t buf[RTCP_MAX_PACKET];
    size_t buf_len;
} rtc_rtcp_packet_t;

typedef struct {
    uint32_t ssrc; /* local SSRC */

    /* Sender stats */
    uint32_t packets_sent;
    uint32_t octets_sent;
    uint32_t rtp_timestamp; /* last RTP timestamp sent */
    uint32_t send_clock_rate;
    uint64_t send_time_ms;
    bool send_clock_valid;

    /* Receiver stats (tracking a single remote source) */
    uint32_t remote_ssrc; /* SSRC of remote source we're receiving */
    uint32_t packets_received;
    uint32_t packets_expected;
    uint32_t packets_lost;
    uint32_t expected_prior;
    uint32_t received_prior;
    uint32_t base_seq;     /* extended seq of first packet received (RFC 3550 §A.1) */
    uint32_t highest_seq;  /* highest seq received (with ROC in upper 16 bits) */
    uint32_t jitter;       /* interarrival jitter (Q16.16 fixed point) */
    uint32_t last_transit; /* last transit time for jitter calc */
    bool first_packet;     /* true until first RTP packet received */

    /* Last SR received */
    uint32_t last_sr_ntp;       /* middle 32 bits of NTP from last SR */
    uint64_t last_sr_recv_time; /* local time (ms) when last SR was received */

    /* Timing for report interval */
    uint64_t last_report_time; /* when we last sent SR/RR */
    uint64_t next_report_time;
    uint32_t report_target_bps;
    size_t last_report_size;
    unsigned report_members, report_senders, report_pmembers;
    bool report_we_sent, report_initial;
    double report_previous_ms;
    double report_average_size;
    char cname[33];
    bool source_clock_valid;
    rtc_rtcp_sr_t source_clock;
    uint64_t source_clock_ms;
    struct {
        uint32_t lsr;
        uint64_t sent_ms;
        bool used;
    } sr_history[RTC_RTCP_SR_HISTORY];
    unsigned sr_history_next;
} rtc_rtcp_stats_t;

/* Initialize RTCP statistics context */
int rtc_rtcp_stats_init(rtc_rtcp_stats_t *stats, uint32_t ssrc);
/* Reset reception state for a new/retired remote source, retaining local
 * sender counters and the existing RTCP scheduling budget/deadline. */
int rtc_rtcp_stats_reset_source(rtc_rtcp_stats_t *stats);
/* Start a fresh local sender/reporting identity while retaining reception
 * statistics, CNAME and any external source clock correlation. */
void rtc_rtcp_stats_restart_sender(rtc_rtcp_stats_t *stats, uint32_t ssrc);

/* Update receiver stats when an RTP packet is received */
void rtc_rtcp_stats_on_rtp_recv(rtc_rtcp_stats_t *stats, uint16_t seq, uint32_t timestamp,
                                uint32_t ssrc, uint32_t clock_rate);

/* Update sender stats when an RTP packet is sent */
void rtc_rtcp_stats_on_rtp_send(rtc_rtcp_stats_t *stats, uint32_t timestamp, size_t payload_len);
void rtc_rtcp_stats_on_rtp_send_at(rtc_rtcp_stats_t *stats, uint32_t timestamp, size_t payload_len,
                                   uint64_t monotonic_ms);

/* Update receiver stats when an RTCP Sender Report is received. */
void rtc_rtcp_stats_on_sr_recv(rtc_rtcp_stats_t *stats, const rtc_rtcp_sr_t *sr);

/* Compute RTT in milliseconds from an RR block (RFC 3550 section 6.4.1). */
int rtc_rtcp_rtt_from_rr(const rtc_rtcp_rr_block_t *rr);
void rtc_rtcp_stats_record_sr(rtc_rtcp_stats_t *stats, const rtc_rtcp_sr_t *sr, uint64_t now_ms);
int rtc_rtcp_stats_rtt_from_rr(const rtc_rtcp_stats_t *stats, const rtc_rtcp_rr_block_t *rr,
                               uint64_t now_ms);

/* Build a Sender Report */
int rtc_rtcp_build_sr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats);
/* Explicit clock pair; monotonic_ms and unix_ms describe the same instant. */
int rtc_rtcp_build_sr_at(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats,
                         uint64_t monotonic_ms, uint64_t unix_ms);

/* Build a Receiver Report */
int rtc_rtcp_build_rr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats);

/* Process-local random CNAME, shared by SDP and RTCP. NULL on RNG failure. */
const char *rtc_rtcp_cname(void);

/* Build SR/RR followed by an SDES CNAME chunk (RFC 3550 section 6.1). */
int rtc_rtcp_build_report(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats, bool sender);
int rtc_rtcp_stats_report_sent(rtc_rtcp_stats_t *stats);
/* RFC 3550 A.7 interval calculation, including initial minimum/randomization. */
uint32_t rtc_rtcp_interval_ms(uint32_t bandwidth_bps, size_t average_packet_bytes, unsigned members,
                              unsigned senders, bool sending, bool initial, uint32_t random_value);
/* RTC_ERR_TIMEOUT means the report is not due yet, including reconsideration. */
int rtc_rtcp_report_due(rtc_rtcp_stats_t *stats, uint32_t media_target_bps, size_t report_size,
                        uint64_t now_ms);
/* Explicit random draw for deterministic RFC 3550 A.7 scheduling. */
int rtc_rtcp_report_due_at(rtc_rtcp_stats_t *stats, uint32_t media_target_bps, size_t report_size,
                           uint64_t now_ms, uint32_t random_value);
/* Serialize membership access with the owning transport's send mutex.
 * Only authenticated input belongs here; sender=true denotes RTP activity. */
bool rtc_rtcp_membership_can_add(const rtc_rtcp_membership_t *group, uint32_t ssrc, bool local);
int rtc_rtcp_membership_touch(rtc_rtcp_membership_t *group, uint32_t ssrc, bool local, bool sender,
                              uint64_t now_ms);
int rtc_rtcp_membership_on_rtcp(rtc_rtcp_membership_t *group, const uint8_t *data, size_t len,
                                bool local, size_t wire_size, uint64_t now_ms);
void rtc_rtcp_membership_expire(rtc_rtcp_membership_t *group, uint32_t media_target_bps,
                                uint64_t now_ms);
void rtc_rtcp_stats_set_membership(rtc_rtcp_stats_t *stats, const rtc_rtcp_membership_t *group,
                                   uint64_t now_ms);
int rtc_rtcp_build_bye(uint8_t *buf, size_t capacity, size_t *len, uint32_t ssrc);
typedef struct {
    uint32_t ssrcs[31];
    unsigned count;
} rtc_rtcp_bye_t;
/* Validates the optional reason/alignment and RTCP padding before returning
 * the source list. A zero-source BYE is valid and has no lifecycle effect. */
int rtc_rtcp_parse_bye(rtc_rtcp_bye_t *bye, const uint8_t *data, size_t len);
/* Validate all framing/padding and report/BYE bodies before dispatch. */
int rtc_rtcp_validate_compound(const uint8_t *data, size_t len);
uint64_t rtc_rtcp_source_timeout_ms(const rtc_rtcp_stats_t *stats);
/* Prefix standalone feedback/BYE with RR + SDES; reduced-size RTCP is not
 * advertised. Existing SR/RR compounds are left unchanged. */
int rtc_rtcp_compound_feedback(uint8_t *buf, size_t *len, size_t capacity);
int rtc_rtcp_compound_feedback_named(uint8_t *buf, size_t *len, size_t capacity, const char *cname);

/* Parse an RTCP packet from raw bytes */
int rtc_rtcp_parse(rtc_rtcp_packet_t *pkt, const uint8_t *data, size_t len);

/* Check if raw data looks like RTCP (for demux alongside RTP) */
bool rtc_rtcp_is_rtcp(const uint8_t *data, size_t len);

/* RTCP feedback packets (RFC 4585 / 5104). */

/* NACK parse result */
typedef struct {
    uint32_t sender_ssrc;
    uint32_t media_ssrc;
    uint16_t lost_seqs[RTCP_NACK_MAX_SEQS];
    int lost_count;
} rtc_rtcp_nack_t;

/* PLI parse result */
typedef struct {
    uint32_t sender_ssrc;
    uint32_t media_ssrc;
} rtc_rtcp_pli_t;

/* FIR parse result */
typedef struct {
    uint32_t sender_ssrc;
    uint32_t media_ssrc;
    uint8_t seq_nr;
} rtc_rtcp_fir_t;

/* Transport-CC feedback parse result (draft-holmer-rmcat-transport-wide-cc).
 * Receive deltas are normalized to microseconds, relative to the start of
 * the feedback's reference time. */
#define RTC_TWCC_PARSE_MAX 256

typedef struct {
    uint32_t sender_ssrc;
    uint32_t media_ssrc;
    uint16_t base_seq;
    uint8_t fb_pkt_count;
    uint64_t reference_time_us; /* absolute, derived from 24-bit field × 64ms */
    struct {
        uint16_t seq;
        bool received;
        uint64_t recv_time_us; /* absolute (reference_time_us + cumulative deltas) */
    } items[RTC_TWCC_PARSE_MAX];
    int item_count;
} rtc_rtcp_twcc_t;

/* Build feedback packets — write into caller-supplied buffer.
 * Returns RTC_OK on success, error code on failure.
 * *out_len is set to the number of bytes written. */
int rtc_rtcp_build_nack(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t sender_ssrc,
                        uint32_t media_ssrc, const uint16_t *lost_seqs, int count);
int rtc_rtcp_build_pli(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t sender_ssrc,
                       uint32_t media_ssrc);
int rtc_rtcp_build_fir(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t sender_ssrc,
                       uint32_t media_ssrc, uint8_t seq_nr);

/* Parse feedback packets from raw RTCP bytes (after SRTCP unprotect).
 * Caller must check PT and FMT before calling the appropriate parse function. */
int rtc_rtcp_parse_nack(rtc_rtcp_nack_t *out, const uint8_t *data, size_t len);
int rtc_rtcp_parse_pli(rtc_rtcp_pli_t *out, const uint8_t *data, size_t len);
int rtc_rtcp_parse_fir(rtc_rtcp_fir_t *out, const uint8_t *data, size_t len);
int rtc_rtcp_parse_twcc(rtc_rtcp_twcc_t *out, const uint8_t *data, size_t len);
typedef struct {
    uint32_t sender_ssrc;
    uint32_t media_ssrc;
    uint32_t bitrate_bps;
} rtc_rtcp_remb_t;
/* Single-source REMB (goog-remb) for the one-camera profile. */
int rtc_rtcp_build_remb(uint8_t *buf, size_t capacity, size_t *len, uint32_t sender_ssrc,
                        uint32_t media_ssrc, uint32_t bitrate_bps);
int rtc_rtcp_parse_remb(rtc_rtcp_remb_t *out, const uint8_t *data, size_t len);

/* Extract PT and FMT from an RTCP header (after version check).
 * Useful for dispatch. Returns false if data too short or version != 2. */
bool rtc_rtcp_get_pt_fmt(const uint8_t *data, size_t len, uint8_t *pt, uint8_t *fmt);

#endif /* RTC_RTCP_H */
