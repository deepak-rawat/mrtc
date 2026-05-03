/*
 * meeting.h - Meeting (room) management for the signaling server.
 *
 * A meeting holds a set of peers. The server creates/destroys meetings
 * as peers join/leave via WebSocket.
 */
#ifndef MEETING_H
#define MEETING_H

#include <stddef.h>
#include <stdbool.h>

#define MEETING_MAX_PEERS   8
#define MEETING_MAX_NAME    64
#define MEETING_MAX_ROOMS   32
#define MEETING_PEER_ID_LEN 16

/* Opaque peer handle (maps to a WebSocket connection) */
typedef struct meeting_peer {
    char id[MEETING_PEER_ID_LEN];
    void *ws_handle; /* lws * — opaque to meeting logic */
    int meeting_idx; /* index into meeting_manager_t.meetings[], or -1 */
} meeting_peer_t;

typedef struct {
    char name[MEETING_MAX_NAME];
    meeting_peer_t *peers[MEETING_MAX_PEERS];
    int peer_count;
    bool active;
} meeting_t;

/* Callback for sending a message to a specific peer */
typedef void (*meeting_send_fn)(meeting_peer_t *peer, const char *json, void *user);

typedef struct {
    meeting_t meetings[MEETING_MAX_ROOMS];
    int meeting_count;
    meeting_send_fn send_fn;
    void *send_user;
} meeting_manager_t;

/* Initialize the meeting manager */
void meeting_manager_init(meeting_manager_t *mgr, meeting_send_fn send_fn, void *user);

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
