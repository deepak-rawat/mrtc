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
#define RTCP_FMT_NACK 1  /* Generic NACK (RTPFB, RFC 4585 §6.2.1) */
#define RTCP_FMT_PLI  1  /* Picture Loss Indication (PSFB, RFC 4585 §6.3.1) */
#define RTCP_FMT_FIR  4  /* Full Intra Request (PSFB, RFC 5104 §4.3.1) */
#define RTCP_FMT_REMB 15 /* REMB (PSFB, draft-alvestrand-rmcat-remb) */

/* NACK limits */
#define RTCP_NACK_MAX_FCIS 64
#define RTCP_NACK_MAX_SEQS (RTCP_NACK_MAX_FCIS * 17)

/* RTCP header size (common part: V, P, RC, PT, length) */
#define RTCP_HEADER_SIZE   4
#define RTCP_SR_SIZE       20 /* Sender info (NTP ts + RTP ts + counts), not including SSRC */
#define RTCP_RR_BLOCK_SIZE 24 /* Per-source reception report block */
#define RTCP_MAX_PACKET    1400

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

/* ---------- Statistics tracking ---------- */

typedef struct {
    uint32_t ssrc; /* local SSRC */

    /* Sender stats */
    uint32_t packets_sent;
    uint32_t octets_sent;
    uint32_t rtp_timestamp; /* last RTP timestamp sent */

    /* Receiver stats (tracking a single remote source) */
    uint32_t remote_ssrc; /* SSRC of remote source we're receiving */
    uint32_t packets_received;
    uint32_t packets_expected;
    uint32_t packets_lost;
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
} rtc_rtcp_stats_t;

/* Initialize RTCP statistics context */
int rtc_rtcp_stats_init(rtc_rtcp_stats_t *stats, uint32_t ssrc);

/* Update receiver stats when an RTP packet is received */
void rtc_rtcp_stats_on_rtp_recv(rtc_rtcp_stats_t *stats, uint16_t seq, uint32_t timestamp,
                                uint32_t ssrc, uint32_t clock_rate);

/* Update sender stats when an RTP packet is sent */
void rtc_rtcp_stats_on_rtp_send(rtc_rtcp_stats_t *stats, uint32_t timestamp, size_t payload_len);

/* Build a Sender Report */
int rtc_rtcp_build_sr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats);

/* Build a Receiver Report */
int rtc_rtcp_build_rr(rtc_rtcp_packet_t *pkt, const rtc_rtcp_stats_t *stats);

/* Parse an RTCP packet from raw bytes */
int rtc_rtcp_parse(rtc_rtcp_packet_t *pkt, const uint8_t *data, size_t len);

/* Check if raw data looks like RTCP (for demux alongside RTP) */
bool rtc_rtcp_is_rtcp(const uint8_t *data, size_t len);

/* ---------- RTCP Feedback packets (RFC 4585 / 5104 / REMB) ---------- */

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

/* REMB parse result */
typedef struct {
    uint32_t sender_ssrc;
    uint32_t media_ssrcs[256];
    int ssrc_count;
    uint32_t bitrate_bps;
} rtc_rtcp_remb_t;

/* Build feedback packets — write into caller-supplied buffer.
 * Returns RTC_OK on success, error code on failure.
 * *out_len is set to the number of bytes written. */
int rtc_rtcp_build_nack(uint8_t *buf, size_t buf_cap, size_t *out_len,
                        uint32_t sender_ssrc, uint32_t media_ssrc,
                        const uint16_t *lost_seqs, int count);
int rtc_rtcp_build_pli(uint8_t *buf, size_t buf_cap, size_t *out_len,
                       uint32_t sender_ssrc, uint32_t media_ssrc);
int rtc_rtcp_build_fir(uint8_t *buf, size_t buf_cap, size_t *out_len,
                       uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t seq_nr);
int rtc_rtcp_build_remb(uint8_t *buf, size_t buf_cap, size_t *out_len,
                        uint32_t sender_ssrc, const uint32_t *media_ssrcs,
                        int ssrc_count, uint32_t bitrate_bps);

/* Parse feedback packets from raw RTCP bytes (after SRTCP unprotect).
 * Caller must check PT and FMT before calling the appropriate parse function. */
int rtc_rtcp_parse_nack(rtc_rtcp_nack_t *out, const uint8_t *data, size_t len);
int rtc_rtcp_parse_pli(rtc_rtcp_pli_t *out, const uint8_t *data, size_t len);
int rtc_rtcp_parse_fir(rtc_rtcp_fir_t *out, const uint8_t *data, size_t len);
int rtc_rtcp_parse_remb(rtc_rtcp_remb_t *out, const uint8_t *data, size_t len);

/* Extract PT and FMT from an RTCP header (after version check).
 * Useful for dispatch. Returns false if data too short or version != 2. */
bool rtc_rtcp_get_pt_fmt(const uint8_t *data, size_t len, uint8_t *pt, uint8_t *fmt);

#endif /* RTC_RTCP_H */
