/*
 * RTC transport API - one logical encrypted endpoint transport.
 */
#ifndef RTC_PUBLIC_TRANSPORT_H
#define RTC_PUBLIC_TRANSPORT_H

#include "rtc_common.h"
#include "rtc_rtp.h"
#include "rtc_worker.h"

#define RTC_ICE_UFRAG_MAX                   8
#define RTC_ICE_PWD_MAX                     24
#define RTC_DTLS_FINGERPRINT_MAX            96
#define RTC_TRANSPORT_RTP_PROTECT_OVERHEAD  10
#define RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD 14

typedef struct rtc_transport rtc_transport_t;
typedef struct rtc_listener rtc_listener_t;

typedef void (*rtc_transport_rtp_fn)(const rtc_rtp_packet_t *pkt, void *user);
typedef void (*rtc_transport_rtcp_fn)(const uint8_t *data, size_t len, void *user);
typedef void (*rtc_transport_data_fn)(const uint8_t *data, size_t len, void *user);

typedef enum {
    RTC_TRANSPORT_CANDIDATE_HOST,
    RTC_TRANSPORT_CANDIDATE_SRFLX,
    RTC_TRANSPORT_CANDIDATE_RELAY,
} rtc_transport_candidate_type_t;

typedef struct {
    char foundation[32];
    char address[64];
    uint16_t port;
    char protocol[8];
    rtc_transport_candidate_type_t type;
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
    uint32_t initial_outgoing_bitrate_bps;
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
} rtc_transport_stats_t;

rtc_transport_t *rtc_transport_create(rtc_worker_t *worker, const rtc_transport_config_t *cfg);
int rtc_transport_get_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out);
int rtc_transport_restart_ice(rtc_transport_t *transport);
int rtc_transport_set_remote_ice_parameters(rtc_transport_t *transport,
                                            const rtc_ice_parameters_t *remote);
int rtc_transport_add_remote_candidate(rtc_transport_t *transport,
                                       const rtc_transport_candidate_t *candidate);
int rtc_transport_start_ice(rtc_transport_t *transport);
int rtc_transport_start_dtls(rtc_transport_t *transport);
int rtc_transport_set_dtls_role(rtc_transport_t *transport, rtc_transport_dtls_role_t role);
int rtc_transport_get_dtls_parameters(rtc_transport_t *transport, rtc_dtls_parameters_t *out);
int rtc_transport_get_stats(rtc_transport_t *transport, rtc_transport_stats_t *out);
void rtc_transport_on_rtp(rtc_transport_t *transport, rtc_transport_rtp_fn fn, void *user);
void rtc_transport_on_rtcp(rtc_transport_t *transport, rtc_transport_rtcp_fn fn, void *user);
void rtc_transport_on_data(rtc_transport_t *transport, rtc_transport_data_fn fn, void *user);
int rtc_transport_send_data(rtc_transport_t *transport, const uint8_t *data, size_t len);
int rtc_transport_send_rtp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap);
int rtc_transport_send_rtcp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap);
int rtc_transport_send_protected_rtp(rtc_transport_t *transport, const uint8_t *data, size_t len);
void rtc_transport_close(rtc_transport_t *transport);
void rtc_transport_destroy(rtc_transport_t *transport);

#endif /* RTC_PUBLIC_TRANSPORT_H */
