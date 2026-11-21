/*
 * ICE (RFC 8445) with consent freshness (RFC 7675) for logical transports.
 *
 * Manages credentials, remote candidates, authenticated connectivity checks,
 * nomination and restart state on the transport worker. The listener gathers
 * local candidates; the transport owns TURN allocations and encrypted media.
 */
#ifndef RTC_ICE_H
#define RTC_ICE_H

#include "rtc/rtc_transport.h"
#include "rtc_listener_internal.h"
#include <stdatomic.h>

/* Transport-provided direct/relay I/O and peer permission setup */
typedef int (*rtc_ice_send_fn)(const uint8_t *data, size_t len, const rtc_addr_t *to, void *user);
typedef int (*rtc_ice_prepare_peer_fn)(const rtc_addr_t *to, void *user);

typedef struct {
    /* Borrowed transport resources; must outlive ICE close */
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_mutex_t *send_mutex;   /* guards selected-address publication and media sends */
    const atomic_bool *closed; /* transport closure flag */
    rtc_ice_send_fn send;
    rtc_ice_prepare_peer_fn prepare_peer;
    rtc_listener_packet_fn on_packet;
    void *user;
    rtc_ice_mode_t mode;
    uint32_t consent_timeout_ms; /* zero: 30 seconds */
} rtc_ice_config_t;

typedef struct {
    rtc_ice_config_t config;

    /* Local and remote credentials */
    char ufrag[RTC_ICE_UFRAG_MAX];
    char pwd[RTC_ICE_PWD_MAX];
    char remote_ufrag[RTC_ICE_UFRAG_MAX];
    char remote_pwd[RTC_ICE_PWD_MAX];
    rtc_ice_mode_t mode;

    /* Selected path and remote candidate rotation */
    rtc_addr_t selected_remote;
    atomic_bool selected_remote_valid;
    rtc_addr_t remote_candidate;
    rtc_addr_t remote_candidates[ICE_MAX_CANDIDATES];
    int remote_candidate_count;
    unsigned remote_candidate_index;
    uint32_t remote_candidates_deferred;   /* unresolved mDNS candidates */
    uint32_t remote_candidates_ineligible; /* TCP or incompatible relay family */
    bool remote_ice_valid;
    bool remote_candidate_valid;

    /* Outstanding connectivity check and listener registrations */
    uint8_t txn_id[STUN_TXN_ID_SIZE];
    bool txn_registered;
    bool txn_controlling; /* role used by the outstanding request */
    bool ufrag_registered;
    rtc_worker_timer_t timer;
    int check_count;

    /* ICE role and consent state */
    bool controlling;
    uint64_t tie_breaker;
    atomic_uint_fast64_t consent_last_ms;
    uint32_t consent_timeout_ms;
    bool consent_expired;
    bool restarting;
    bool relayed; /* selected path uses TURN, not a direct listener tuple */
} rtc_ice_agent_t;

/* Initialize credentials and state without registering listener routes */
int rtc_ice_init(rtc_ice_agent_t *ice, const rtc_ice_config_t *config);

/* Register the local ufrag after transport resources are ready */
int rtc_ice_bind(rtc_ice_agent_t *ice);

/* Unregister routes and cancel checks on the worker; shared resources remain owned by transport */
void rtc_ice_close(rtc_ice_agent_t *ice);

/* Copy local credentials for signaling */
void rtc_ice_get_parameters(const rtc_ice_agent_t *ice, rtc_ice_parameters_t *out);

/* Detect changed credentials in an established remote ICE generation */
bool rtc_ice_remote_changed(const rtc_ice_agent_t *ice, const rtc_ice_parameters_t *remote);

/* Rotate local credentials and reset checks, retaining the selected path for media continuity */
int rtc_ice_restart(rtc_ice_agent_t *ice);

/* Apply remote credentials; a new generation clears candidates and outstanding checks */
int rtc_ice_set_remote_parameters(rtc_ice_agent_t *ice, const rtc_ice_parameters_t *remote);

/* Add a numeric UDP candidate; defer mDNS and ignore stale/ineligible candidates.
 * AF_UNSPEC disables relay-family filtering. */
int rtc_ice_add_remote_candidate(rtc_ice_agent_t *ice, const rtc_transport_candidate_t *candidate,
                                 int relay_family);

/* Convert a parsed SDP candidate while preserving mDNS and transport metadata */
int rtc_ice_candidate_from_sdp(rtc_transport_candidate_t *out, const rtc_ice_candidate_t *in);

/* Set the local role; ICE-lite cannot be controlling */
int rtc_ice_set_controlling(rtc_ice_agent_t *ice, bool controlling);

/* Begin full-ICE checks and schedule retries; an established path is left unchanged */
int rtc_ice_start(rtc_ice_agent_t *ice);

/* Process authenticated checks, role conflicts, nomination and consent responses */
void rtc_ice_handle_stun(rtc_ice_agent_t *ice, const uint8_t *data, size_t len,
                         const rtc_addr_t *from);

/* Schedule the next check/consent deadline unless already armed or closed */
void rtc_ice_arm_timer(rtc_ice_agent_t *ice);

/* Invalidate the selected path and remove its direct listener route */
void rtc_ice_clear_selected(rtc_ice_agent_t *ice);

/* Receive-path gate: only the selected address with unexpired consent is eligible */
bool rtc_ice_accepts_media(const rtc_ice_agent_t *ice, const rtc_addr_t *from);

/* Send-path consent check with the transport send mutex held; no worker-state mutation */
bool rtc_ice_can_send(const rtc_ice_agent_t *ice);

#endif /* RTC_ICE_H */
