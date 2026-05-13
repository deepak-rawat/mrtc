/*
 * ICE (RFC 8445) - Minimal ICE-lite / ICE-controlling implementation.
 *
 * Gathers host and server-reflexive candidates, performs connectivity checks.
 * Uses an rtc_transport_t for all socket I/O (transport is not owned by ICE).
 */
#ifndef RTC_ICE_H
#define RTC_ICE_H

#include "rtc_common.h"
#include "rtc_stun.h"
#include "rtc_sdp.h" /* for rtc_ice_candidate_t, ICE_MAX_CANDIDATES, etc. */

/* Forward declaration - avoid including rtc_transport.h to prevent circular deps */
typedef struct rtc_transport rtc_transport_t;

typedef enum {
    ICE_STATE_NEW,
    ICE_STATE_GATHERING,
    ICE_STATE_CHECKING,
    ICE_STATE_CONNECTED,
    ICE_STATE_FAILED,
    ICE_STATE_CLOSED,
} rtc_ice_state_t;

typedef struct {
    /* Local credentials */
    char ufrag[ICE_UFRAG_LEN];
    char pwd[ICE_PWD_LEN];

    /* Remote credentials */
    char remote_ufrag[ICE_UFRAG_LEN];
    char remote_pwd[ICE_PWD_LEN];

    /* Candidates */
    rtc_ice_candidate_t local_candidates[ICE_MAX_CANDIDATES];
    int local_candidate_count;
    rtc_ice_candidate_t remote_candidates[ICE_MAX_CANDIDATES];
    int remote_candidate_count;

    /* Selected pair */
    rtc_addr_t selected_local;
    rtc_addr_t selected_remote;

    /* State */
    rtc_ice_state_t state;

    /* Transport (borrowed pointer, not owned) */
    rtc_transport_t *transport;

    /* STUN server for srflx gathering */
    char stun_server[64];
    uint16_t stun_port;

    /* Controlling / controlled */
    bool controlling;
    uint64_t tie_breaker;

    /* Async connectivity-check state (set by rtc_ice_connect /
     * rtc_ice_send_check, read by rtc_ice_handle_stun on BINDING_RESPONSE). */
    uint8_t last_txn_id[STUN_TXN_ID_SIZE];
    uint64_t check_deadline_ms; /* overall deadline (rtc_time_ms) */
    int check_round;            /* round-robin index across remote candidates */
} rtc_ice_agent_t;

/* Initialize an ICE agent with a borrowed transport, generate ufrag/pwd */
int rtc_ice_init(rtc_ice_agent_t *agent, rtc_transport_t *transport, const char *stun_server,
                 uint16_t stun_port);

/* Gather local candidates (host + srflx) */
int rtc_ice_gather(rtc_ice_agent_t *agent);

/* Set remote ICE credentials */
int rtc_ice_set_remote_credentials(rtc_ice_agent_t *agent, const char *ufrag, const char *pwd);

/* Add a remote candidate */
int rtc_ice_add_remote_candidate(rtc_ice_agent_t *agent, const rtc_ice_candidate_t *cand);

/*
 * Kick off connectivity checks (non-blocking).
 * Sends the first STUN binding request and sets state to CHECKING. The caller
 * is responsible for scheduling periodic rtc_ice_send_check() retries (e.g.
 * via rtc_transport_add_timer) until state transitions to CONNECTED or
 * rtc_ice_check_deadline_passed() returns true.
 *
 * STUN binding responses are processed asynchronously through the transport's
 * recv callback dispatching to rtc_ice_handle_stun().
 */
int rtc_ice_connect(rtc_ice_agent_t *agent);

/* Send the next round-robin connectivity check. Returns RTC_ERR_ICE if no
 * remote candidates or state is not CHECKING. */
int rtc_ice_send_check(rtc_ice_agent_t *agent);

/* True if the overall ICE deadline set by rtc_ice_connect has elapsed. */
bool rtc_ice_check_deadline_passed(const rtc_ice_agent_t *agent);

/*
 * Handle an incoming STUN packet (called by transport's recv callback).
 * Processes binding requests (sends response) and binding responses
 * (completes connectivity checks).
 */
void rtc_ice_handle_stun(rtc_ice_agent_t *agent, const uint8_t *data, size_t len,
                         const rtc_addr_t *from);

/* Close ICE agent (does NOT close the transport) */
void rtc_ice_close(rtc_ice_agent_t *agent);

#endif /* RTC_ICE_H */
