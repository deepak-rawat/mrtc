/*
 * RTC transport API - one logical encrypted endpoint transport.
 */
#ifndef RTC_PUBLIC_TRANSPORT_H
#define RTC_PUBLIC_TRANSPORT_H

#include "rtc_common.h"
#include "rtc_rtp.h"
#include "rtc_rtcp.h"
#include "rtc_sdp.h"
#include "rtc_worker.h"

#define RTC_ICE_UFRAG_MAX                   257
#define RTC_ICE_PWD_MAX                     257
#define RTC_DTLS_FINGERPRINT_MAX            96
#define RTC_TRANSPORT_RTP_PROTECT_OVERHEAD  16 /* AES-GCM tag (RFC 7714) */
#define RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD 84 /* SRTCP (20) + compound feedback prefix (<=64) */

typedef struct rtc_transport rtc_transport_t;
typedef struct rtc_listener rtc_listener_t;

typedef void (*rtc_transport_rtp_fn)(const rtc_rtp_packet_t *pkt, void *user);
typedef void (*rtc_transport_rtcp_fn)(const uint8_t *data, size_t len, void *user);
typedef void (*rtc_transport_data_fn)(const uint8_t *data, size_t len, void *user);

typedef enum {
    RTC_TRANSPORT_CANDIDATE_HOST,
    RTC_TRANSPORT_CANDIDATE_SRFLX,
    RTC_TRANSPORT_CANDIDATE_RELAY,
    RTC_TRANSPORT_CANDIDATE_PRFLX,
} rtc_transport_candidate_type_t;

typedef struct {
    char foundation[32];
    char address[256]; /* numeric address or unresolved mDNS host */
    char related_address[64];
    uint16_t port;
    uint16_t related_port;
    char protocol[8];
    rtc_transport_candidate_type_t type;
    bool has_related_address;
    char username_fragment[RTC_ICE_UFRAG_MAX];
} rtc_transport_candidate_t;

typedef enum {
    RTC_ICE_MODE_FULL,
    RTC_ICE_MODE_LITE,
} rtc_ice_mode_t;

typedef struct {
    char username_fragment[RTC_ICE_UFRAG_MAX];
    char password[RTC_ICE_PWD_MAX];
    rtc_ice_mode_t mode;
} rtc_ice_parameters_t;

typedef enum {
    RTC_TRANSPORT_DTLS_ROLE_CLIENT,
    RTC_TRANSPORT_DTLS_ROLE_SERVER,
} rtc_transport_dtls_role_t;

typedef enum {
    RTC_TRANSPORT_DTLS_NEW,
    RTC_TRANSPORT_DTLS_CONNECTING,
    RTC_TRANSPORT_DTLS_CONNECTED,
    RTC_TRANSPORT_DTLS_FAILED,
    RTC_TRANSPORT_DTLS_CLOSED,
} rtc_transport_dtls_state_t;

typedef struct {
    rtc_transport_dtls_role_t role;
    char fingerprint[RTC_DTLS_FINGERPRINT_MAX];
} rtc_dtls_parameters_t;

typedef struct {
    rtc_listener_t *listener;
    rtc_ice_mode_t ice_mode;
    bool enable_sctp;
    bool enable_twcc;
    bool enable_rtp_pacing;    /* composed video streams only; raw RTP remains immediate */
    bool streaming_rtp_pacing; /* relay mode: do not wait for a complete incoming frame */
    uint32_t initial_outgoing_bitrate_bps;
    uint32_t consent_timeout_ms; /* zero: RFC 7675 default of 30 seconds */
} rtc_transport_config_t;

typedef struct {
    bool closed;
    rtc_ice_mode_t ice_mode;
    bool selected_tuple_valid;
    rtc_addr_t selected_remote;
    rtc_transport_dtls_state_t dtls_state;
    bool srtp_ready;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t dtls_packets_received;
    uint64_t rtp_packets_sent;   /* paced video: successful initial socket writes */
    uint64_t rtp_bytes_sent;     /* protected wire bytes, excluding retransmissions */
    uint64_t rtp_packets_queued; /* currently staged or scheduled */
    uint64_t rtp_bytes_queued;
    uint64_t rtp_packets_dropped;
    uint64_t rtp_frames_dropped;
    uint64_t rtp_retransmissions_sent;
    uint64_t rtp_retransmissions_rejected;
    bool ice_controlling;
    bool consent_expired;
    bool ice_restarting;
    uint32_t remote_candidates_deferred;
    uint32_t remote_candidates_ineligible;
    uint32_t target_bitrate_bps; /* configured/estimated target, not measured capacity */
    unsigned rtcp_members;
    unsigned rtcp_senders;
    unsigned turn_address_attempts;
    bool turn_failed; /* terminal failure, not a pending address retry */
} rtc_transport_stats_t;

rtc_transport_t *rtc_transport_create(rtc_worker_t *worker, const rtc_transport_config_t *cfg);
int rtc_transport_get_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out);
int rtc_transport_restart_ice(rtc_transport_t *transport);
int rtc_transport_set_remote_ice_parameters(rtc_transport_t *transport,
                                            const rtc_ice_parameters_t *remote);
int rtc_transport_add_remote_candidate(rtc_transport_t *transport,
                                       const rtc_transport_candidate_t *candidate);
int rtc_transport_candidate_from_ice(rtc_transport_candidate_t *out, const rtc_ice_candidate_t *in);
int rtc_transport_start_ice(rtc_transport_t *transport);
int rtc_transport_start_dtls(rtc_transport_t *transport);
/* Pin the remote SHA-256 certificate fingerprint before starting DTLS.
 * The role describes the remote endpoint; the local role is its inverse. */
int rtc_transport_set_remote_dtls_parameters(rtc_transport_t *transport,
                                             const rtc_dtls_parameters_t *remote);
int rtc_transport_set_ice_controlling(rtc_transport_t *transport, bool controlling);
/* Configure a UDP TURN allocation before connectivity starts. Relay-only:
 * all checks, DTLS and media use this allocation, never the listener socket.
 * Allocation is asynchronous; poll get_relay_candidate until RTC_OK, or
 * inspect get_stats().turn_failed for terminal failure. */
int rtc_transport_set_turn_server(rtc_transport_t *transport, const char *host, uint16_t port,
                                  const char *username, const char *credential);
typedef enum { RTC_TURN_UDP, RTC_TURN_TCP, RTC_TURN_TLS } rtc_turn_protocol_t;
#define RTC_TURN_MAX_ADDRESSES 4
typedef struct {
    const char *host;
    uint16_t port; /* zero: 3478, or 5349 for TLS */
    const char *username;
    const char *credential;
    rtc_turn_protocol_t protocol;
    const char *ca_file; /* NULL: OpenSSL's default trust store; TLS only */
    /* Optional caller-resolved addresses, copied by set_turn_server_ex.
     * Ports must match port above. TLS still verifies host, never an alias. */
    const rtc_addr_t *addresses;
    size_t address_count;
    /* Zero/AF_INET: IPv4 relay; AF_INET6: IPv6 relay (RFC 6156).
     * The relay family is independent of the server connection family. */
    int relay_family;
} rtc_turn_server_t;
/* DNS resolution occurs on the calling thread, before worker configuration.
 * Without caller-resolved addresses, hostname configuration must be outside
 * worker callbacks. Up to four addresses are tried before allocation; with
 * alternatives, each attempt has a five-second allocation budget.
 * An established allocation is never moved to another address automatically.
 * TCP/TLS is the client-to-relay leg; the allocated relay is still UDP. */
int rtc_transport_set_turn_server_ex(rtc_transport_t *transport, const rtc_turn_server_t *server);
/* At most four identities and four total address attempts, interleaved by
 * identity then address/family (preferring a different family when available),
 * each with a five-second allocation budget.
 * All alternatives must request the same relay family. No simultaneous
 * dual-family allocation, NAT64 synthesis or cross-family peer permission.
 * Configuration is owned until close. Only an explicit ICE restart may
 * replace a failed allocation; a healthy allocation remains pinned. */
int rtc_transport_set_turn_servers(rtc_transport_t *transport, const rtc_turn_server_t *servers,
                                   size_t count);
int rtc_transport_get_relay_candidate(rtc_transport_t *transport,
                                      rtc_transport_candidate_t *candidate);
typedef void (*rtc_transport_task_fn)(void *user);
int rtc_transport_invoke(rtc_transport_t *transport, rtc_transport_task_fn fn, void *user);
bool rtc_transport_on_worker_thread(rtc_transport_t *transport);
int rtc_transport_set_dtls_role(rtc_transport_t *transport, rtc_transport_dtls_role_t role);
int rtc_transport_get_dtls_parameters(rtc_transport_t *transport, rtc_dtls_parameters_t *out);
int rtc_transport_get_stats(rtc_transport_t *transport, rtc_transport_stats_t *out);
void rtc_transport_on_rtp(rtc_transport_t *transport, rtc_transport_rtp_fn fn, void *user);
void rtc_transport_on_rtcp(rtc_transport_t *transport, rtc_transport_rtcp_fn fn, void *user);
void rtc_transport_on_data(rtc_transport_t *transport, rtc_transport_data_fn fn, void *user);

/* Inbound RTP demux router. With a router installed, parsed RTP packets are
 * dispatched per-SSRC to the bound consumer via `sink`; an unknown SSRC is
 * offered to `resolve` (may be NULL), which may return a consumer to auto-bind
 * for O(1) dispatch thereafter. Packets the router does not deliver fall back
 * to rtc_transport_on_rtp (the single-stream raw handler, e.g. loopback tests).
 * bind / unbind / set run on the worker loop. */
void rtc_transport_set_rtp_router(rtc_transport_t *transport, rtc_rtp_sink_fn sink,
                                  rtc_rtp_resolve_fn resolve, void *resolve_user);
int rtc_transport_bind_rtp(rtc_transport_t *transport, uint32_t ssrc, void *user);
void rtc_transport_unbind_rtp(rtc_transport_t *transport, uint32_t ssrc);

/* Look up the consumer currently bound to `ssrc` in the RTP router, or NULL.
 * Worker-thread accessor: call only from a transport callback (e.g. on_rtcp),
 * used to route RTCP Sender Reports back to the matching receive stream. */
void *rtc_transport_rtp_bound(rtc_transport_t *transport, uint32_t ssrc);

#ifdef MRTC_ENABLE_TWCC
typedef void (*rtc_transport_bitrate_fn)(uint32_t bitrate_bps, void *user);

/* Enable transport-wide congestion control once SDP negotiation has selected a
 * transport-cc header extension id. The transport owns the TWCC sender history,
 * the receiver arrival window + 100 ms feedback timer, and the GCC bandwidth
 * estimator (seeded by initial_outgoing_bitrate_bps). Requires cfg->enable_twcc
 * at create; `local_ssrc` identifies the feedback sender. */
int rtc_transport_enable_twcc(rtc_transport_t *transport, uint8_t ext_id, uint32_t local_ssrc);
/* Directional negotiation; zero id with both permissions false disables TWCC.
 * Sender history survives permission changes. A live id remap is unsupported. */
int rtc_transport_configure_twcc(rtc_transport_t *transport, uint8_t ext_id, uint32_t local_ssrc,
                                 bool send, bool receive);
/* Optional per-source receive permission, evaluated on the worker after RTP
 * routing. Unset means all RTP sources may contribute TWCC arrivals. */
typedef bool (*rtc_transport_rtp_filter_fn)(const rtc_rtp_packet_t *packet, void *user);
void rtc_transport_on_twcc_rtp(rtc_transport_t *transport, rtc_transport_rtp_filter_fn fn,
                               void *user);

/* Subscribe to GCC bandwidth-estimate changes (fires on the worker loop). */
void rtc_transport_on_bitrate_estimate(rtc_transport_t *transport, rtc_transport_bitrate_fn fn,
                                       void *user);

/* Feed an RTCP RR fraction-lost (0..255) into the loss-based estimator. Call
 * from a transport on_rtcp callback (worker thread). No-op without TWCC. */
void rtc_transport_report_rtcp_loss(rtc_transport_t *transport, uint8_t fraction_lost);
#endif

int rtc_transport_send_data(rtc_transport_t *transport, const uint8_t *data, size_t len);
int rtc_transport_send_rtp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap);
/* Resend cached wire bytes from a successful send_rtp on this transport.
 * Runs on the worker, checks readiness/consent, and does not change SRTP state. */
int rtc_transport_retransmit_rtp(rtc_transport_t *transport, const uint8_t *wire, size_t len);
/* Standalone feedback is prefixed with RR/SDES before encryption. Reserve
 * RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD beyond the original packet length. */
int rtc_transport_send_rtcp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap);
int rtc_transport_send_report(rtc_transport_t *transport, rtc_rtcp_stats_t *stats, bool sender);
int rtc_transport_send_bye(rtc_transport_t *transport, uint32_t ssrc);
int rtc_transport_send_bye_named(rtc_transport_t *transport, uint32_t ssrc, const char *cname);
void rtc_transport_close(rtc_transport_t *transport);
void rtc_transport_destroy(rtc_transport_t *transport);

#endif /* RTC_PUBLIC_TRANSPORT_H */
