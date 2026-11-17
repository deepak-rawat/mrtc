/*
 * WebSocket signaling client API.
 *
 * Connects to a signaling server, joins a meeting, and exchanges
 * offer/answer/candidate messages with peers.
 */
#ifndef SIGNALING_CLIENT_H
#define SIGNALING_CLIENT_H

#include <rtc/rtc_common.h>
#include "signaling_msg.h"

/* Callbacks fired on the lws service thread */
typedef void (*sig_on_joined_fn)(const char *my_id, const char **peer_ids, int peer_count,
                                 void *user);
typedef void (*sig_on_peer_joined_fn)(const char *peer_id, void *user);
typedef void (*sig_on_peer_left_fn)(const char *peer_id, void *user);
typedef void (*sig_on_offer_fn)(const char *from, const char *sdp, void *user);
typedef void (*sig_on_answer_fn)(const char *from, const char *sdp, void *user);
typedef void (*sig_on_candidate_fn)(const char *from, const char *candidate, void *user);
typedef void (*sig_on_error_fn)(const char *message, void *user);

typedef struct {
    const char *server_url; /* "ws://host:port" or "wss://host:port" */
    const char *meeting;    /* meeting name to join on connect */

    sig_on_joined_fn on_joined;
    sig_on_peer_joined_fn on_peer_joined;
    sig_on_peer_left_fn on_peer_left;
    sig_on_offer_fn on_offer;
    sig_on_answer_fn on_answer;
    sig_on_candidate_fn on_candidate;
    sig_on_error_fn on_error;
    void *user_data;
    void (*on_mode)(const char *mode, void *user);
    void (*on_disconnected)(void *user);
    unsigned protocol_version; /* zero retains the legacy join shape */
    const char *display_name;
    bool can_mesh;
    bool can_sfu;
    bool can_switch;
    /* Receives parsed events instead of legacy callbacks. Message is borrowed
     * only for the callback duration; copy it before asynchronous use. */
    void (*on_event)(const sig_msg_t *message, void *user);
} signaling_config_t;

typedef struct signaling_client signaling_client_t;

/*
 * Create a signaling client. Does not connect yet.
 * Returns NULL on error.
 */
signaling_client_t *signaling_create(const signaling_config_t *cfg);

/*
 * Connect to the signaling server and start the event loop on a background thread.
 * The client will automatically send "join" once connected.
 */
rtc_err_t signaling_connect(signaling_client_t *sc);

/* Send an SDP offer to a specific peer. Thread-safe. */
rtc_err_t signaling_send_offer(signaling_client_t *sc, const char *to, const char *sdp);

/* Send an SDP answer to a specific peer. Thread-safe. */
rtc_err_t signaling_send_answer(signaling_client_t *sc, const char *to, const char *sdp);

/* Send an ICE candidate to a specific peer. Thread-safe. */
rtc_err_t signaling_send_candidate(signaling_client_t *sc, const char *to, const char *candidate);
rtc_err_t signaling_send_message(signaling_client_t *sc, const sig_msg_t *message);

/* Leave the current meeting. Thread-safe. */
rtc_err_t signaling_leave(signaling_client_t *sc);

/* Disconnect and destroy. Blocks until the background thread exits. */
void signaling_destroy(signaling_client_t *sc);
/* Stop callbacks and close the connection; the client may be connected again. */
void signaling_disconnect(signaling_client_t *sc);

#endif /* SIGNALING_CLIENT_H */
