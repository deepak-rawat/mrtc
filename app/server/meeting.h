/*
 * Meeting (room) management for the signaling server.
 *
 * A meeting holds a set of peers. The server creates/destroys meetings
 * as peers join/leave via WebSocket.
 */
#ifndef MEETING_H
#define MEETING_H

#include <stddef.h>
#include <stdbool.h>
#include <rtc/rtc_str_map.h>
#include "signaling/signaling_msg.h"

#define MEETING_MAX_PEERS         8
#define MEETING_MAX_NAME          64
#define MEETING_MAX_ROOMS         32
#define MEETING_PEER_ID_LEN       16
#define MEETING_MAX_MESH_SESSIONS (MEETING_MAX_PEERS * (MEETING_MAX_PEERS - 1))

/* Opaque peer handle (maps to a WebSocket connection) */
typedef struct meeting_peer {
    char id[MEETING_PEER_ID_LEN];
    void *ws_handle; /* lws * — opaque to meeting logic */
    int meeting_idx; /* index into meeting_manager_t.meetings[], or -1 */
    unsigned version;
    char display_name[SIG_MAX_DISPLAY_NAME];
    bool can_mesh;
    bool can_sfu;
    bool can_switch;
    bool ready;
    bool ack;
    bool protocol_failed;
    bool has_joined;
} meeting_peer_t;

typedef enum { MEETING_MODE_MESH, MEETING_MODE_SFU, MEETING_MODE_AUTO } meeting_mode_t;
typedef enum { MEETING_STABLE, MEETING_PREPARING, MEETING_COMMITTING } meeting_phase_t;

typedef struct {
    uint32_t epoch;
    char id[SIG_MAX_SESSION_ID];
    char owner[MEETING_PEER_ID_LEN];
    char remote[MEETING_PEER_ID_LEN];
} meeting_mesh_session_t;

typedef struct {
    char name[MEETING_MAX_NAME];
    meeting_peer_t *peers[MEETING_MAX_PEERS];
    int peer_count;
    bool active;
    unsigned version;
    meeting_mode_t mode;
    meeting_mode_t desired_mode;
    meeting_mode_t previous_mode;
    uint32_t epoch;
    uint32_t last_epoch;
    uint32_t pending_epoch;
    uint32_t previous_epoch;
    meeting_phase_t phase;
    uint64_t deadline_ms;
    uint64_t downgrade_at_ms;
    bool mesh_failed;
    bool retry_blocked;
    meeting_mesh_session_t mesh_sessions[MEETING_MAX_MESH_SESSIONS];
    unsigned mesh_session_count;
} meeting_t;

/* Callback for sending a message to a specific peer */
typedef void (*meeting_send_fn)(meeting_peer_t *peer, const char *json, void *user);
typedef void (*meeting_retire_fn)(const char *room, const char *peer, uint32_t epoch, void *user);

typedef struct {
    meeting_t meetings[MEETING_MAX_ROOMS];
    int meeting_count;
    rtc_str_map_t name_index; /* meeting name → meeting_t * (borrowed keys: point into
                                 meeting_t.name) */
    meeting_send_fn send_fn;
    void *send_user;
    meeting_mode_t policy;
    unsigned p2p_max;
    uint64_t downgrade_delay_ms;
    uint64_t transition_timeout_ms;
    meeting_retire_fn retire_fn;
    void *retire_user;
} meeting_manager_t;

/* Initialize the meeting manager */
void meeting_manager_init(meeting_manager_t *mgr, meeting_send_fn send_fn, void *user);

/* Free resources held by the meeting manager. Does not free peers. */
void meeting_manager_close(meeting_manager_t *mgr);

/* Create a new peer (called on WS connect). Assigns random ID. */
meeting_peer_t *meeting_peer_create(void *ws_handle);

/* Destroy a peer (called on WS close). Removes from meeting, notifies others. */
void meeting_peer_destroy(meeting_manager_t *mgr, meeting_peer_t *peer);

/*
 * Join a meeting by name. Creates meeting if it doesn't exist.
 * Sends "joined" to the joining peer, "peer_joined" to existing peers.
 * Returns 0 on success, -1 on error (meeting full, etc.).
 */
int meeting_join(meeting_manager_t *mgr, meeting_peer_t *peer, const char *name);
int meeting_join_message(meeting_manager_t *mgr, meeting_peer_t *peer, const sig_msg_t *message,
                         uint64_t now);
int meeting_handle_topology(meeting_manager_t *mgr, meeting_peer_t *peer, const sig_msg_t *message,
                            uint64_t now);
void meeting_tick(meeting_manager_t *mgr, uint64_t now);
/* Returns concrete mode, or -1 for stale/invalid session addressing. */
int meeting_validate_session(meeting_manager_t *mgr, meeting_peer_t *peer,
                             const sig_msg_t *message);
meeting_peer_t *meeting_find_peer(meeting_manager_t *mgr, const char *room, const char *id);

/*
 * Leave the current meeting.
 * Sends "peer_left" to remaining peers.
 */
void meeting_leave(meeting_manager_t *mgr, meeting_peer_t *peer);

/*
 * Route a message from one peer to another (offer/answer/candidate).
 * Looks up the target peer by ID within the same meeting.
 * Returns 0 on success, -1 if target not found.
 */
int meeting_route_message(meeting_manager_t *mgr, meeting_peer_t *from, const char *to_id,
                          const char *json);

#endif /* MEETING_H */
