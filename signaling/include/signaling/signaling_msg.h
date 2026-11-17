/*
 * Signaling message wire format (JSON via cJSON).
 *
 * Shared by both signaling client and server.
 * Message types: join, leave, joined, peer_joined, peer_left,
 *                offer, answer, candidate, error.
 */
#ifndef SIGNALING_MSG_H
#define SIGNALING_MSG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Message types (matches JSON "type" field) */
typedef enum {
    SIG_MSG_JOIN,
    SIG_MSG_LEAVE,
    SIG_MSG_JOINED,
    SIG_MSG_PEER_JOINED,
    SIG_MSG_PEER_LEFT,
    SIG_MSG_OFFER,
    SIG_MSG_ANSWER,
    SIG_MSG_CANDIDATE,
    SIG_MSG_ERROR,
    SIG_MSG_TOPOLOGY_PREPARE,
    SIG_MSG_TOPOLOGY_READY,
    SIG_MSG_TOPOLOGY_COMMIT,
    SIG_MSG_TOPOLOGY_ACK,
    SIG_MSG_TOPOLOGY_ABORT,
    SIG_MSG_TOPOLOGY_REQUEST,
    SIG_MSG_UNKNOWN,
} sig_msg_type_t;

#define SIG_MAX_PEER_ID       16
#define SIG_MAX_MEETING       64
#define SIG_MAX_PEERS         8
#define SIG_MAX_SDP           8192
#define SIG_DC_HEADER_SIZE    (4 + SIG_MAX_PEER_ID)
#define SIG_MAX_CANDIDATE     256
#define SIG_MAX_ERROR_MSG     128
#define SIG_MAX_SESSION_ID    64
#define SIG_MAX_DISPLAY_NAME  64
#define SIG_PROTOCOL_VERSION  2
#define SIG_DC_V2_HEADER_SIZE (SIG_DC_HEADER_SIZE + 8)
#define SIG_MAX_DC_MESSAGE    65536

typedef enum {
    SIG_SESSION_MESH,
    SIG_SESSION_PUBLISH,
    SIG_SESSION_SUBSCRIBE,
} sig_session_role_t;

/* Parsed signaling message */
typedef struct {
    sig_msg_type_t type;
    unsigned version; /* absent on the wire is legacy version 1 */
    uint32_t epoch;
    char session_id[SIG_MAX_SESSION_ID];
    char display_name[SIG_MAX_DISPLAY_NAME];
    char peer_names[SIG_MAX_PEERS][SIG_MAX_DISPLAY_NAME];
    bool can_mesh;
    bool can_sfu;
    bool can_switch;
    char reason[SIG_MAX_ERROR_MSG];

    /* join */
    char meeting[SIG_MAX_MEETING];
    char mode[8]; /* "sfu" or explicit legacy "mesh" */

    /* joined */
    char peer_id[SIG_MAX_PEER_ID];
    char peers[SIG_MAX_PEERS][SIG_MAX_PEER_ID];
    int peer_count;

    /* offer/answer/candidate — addressing */
    char to[SIG_MAX_PEER_ID];
    char from[SIG_MAX_PEER_ID];

    /* offer/answer */
    char sdp[SIG_MAX_SDP];

    /* candidate */
    char candidate[SIG_MAX_CANDIDATE];

    /* error */
    char error_msg[SIG_MAX_ERROR_MSG];
} sig_msg_t;

/* Build helpers — return a JSON string the caller frees. */

/* Client → Server */
char *sig_msg_build_join(const char *meeting);
char *sig_msg_build_leave(void);
char *sig_msg_build_offer(const char *to, const char *sdp);
char *sig_msg_build_answer(const char *to, const char *sdp);
char *sig_msg_build_candidate(const char *to, const char *candidate);

/* Server → Client */
char *sig_msg_build_joined(const char *peer_id, const char **peers, int peer_count);
char *sig_msg_build_joined_mode(const char *peer_id, const char **peers, int peer_count,
                                const char *mode);
char *sig_msg_build_peer_joined(const char *peer_id);
char *sig_msg_build_peer_left(const char *peer_id);
char *sig_msg_build_relay_offer(const char *from, const char *sdp);
char *sig_msg_build_relay_answer(const char *from, const char *sdp);
char *sig_msg_build_relay_candidate(const char *from, const char *candidate);
char *sig_msg_build_error(const char *message);

/*
 * Parse a JSON string into a sig_msg_t.
 * Returns 0 on success, -1 on parse error.
 */
int sig_msg_parse(sig_msg_t *msg, const char *json, size_t json_len);

/* Generic builder for versioned room/session/topology messages. Legacy helpers
 * above keep their original wire shape. */
char *sig_msg_build(const sig_msg_t *msg);
int sig_session_id(char out[SIG_MAX_SESSION_ID], sig_session_role_t role, uint32_t epoch,
                   const char *owner, const char *remote);
int sig_dc_write(uint8_t *out, size_t capacity, size_t *out_len, const char *peer,
                 uint64_t message_id, const uint8_t *data, size_t len);
int sig_dc_read(const uint8_t *wire, size_t len, char peer[SIG_MAX_PEER_ID], uint64_t *message_id,
                const uint8_t **data, size_t *data_len);

#endif /* SIGNALING_MSG_H */
