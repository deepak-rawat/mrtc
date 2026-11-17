/*
 * Meeting management implementation.
 */
#include "meeting.h"
#include "signaling/signaling_msg.h"
#include <rtc/rtc_common.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void random_peer_id(char *buf, size_t len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t raw[16];
    rtc_random_bytes(raw, sizeof(raw));
    for (size_t i = 0; i < len - 1; i++)
        buf[i] = charset[raw[i % sizeof(raw)] % (sizeof(charset) - 1)];
    buf[len - 1] = '\0';
}

void meeting_manager_init(meeting_manager_t *mgr, meeting_send_fn send_fn, void *user) {
    memset(mgr, 0, sizeof(*mgr));
    rtc_str_map_init(&mgr->name_index);
    mgr->send_fn = send_fn;
    mgr->send_user = user;
    mgr->policy = MEETING_MODE_MESH;
    mgr->p2p_max = 2;
    mgr->downgrade_delay_ms = 15000;
    mgr->transition_timeout_ms = 10000;
}

void meeting_manager_close(meeting_manager_t *mgr) {
    if (!mgr)
        return;
    rtc_str_map_free(&mgr->name_index);
}

meeting_peer_t *meeting_peer_create(void *ws_handle) {
    meeting_peer_t *p = calloc(1, sizeof(meeting_peer_t));
    if (!p)
        return NULL;
    random_peer_id(p->id, MEETING_PEER_ID_LEN);
    p->ws_handle = ws_handle;
    p->meeting_idx = -1;
    p->version = 1;
    return p;
}

static meeting_t *find_meeting(meeting_manager_t *mgr, const char *name) {
    return (meeting_t *)rtc_str_map_get(&mgr->name_index, name);
}

static meeting_t *create_meeting(meeting_manager_t *mgr, const char *name) {
    for (int i = 0; i < MEETING_MAX_ROOMS; i++) {
        if (!mgr->meetings[i].active) {
            meeting_t *m = &mgr->meetings[i];
            memset(m, 0, sizeof(*m));
            snprintf(m->name, sizeof(m->name), "%s", name);
            m->active = true;
            m->mode = mgr->policy == MEETING_MODE_SFU ? MEETING_MODE_SFU : MEETING_MODE_MESH;
            m->desired_mode = m->mode;
            m->epoch = m->last_epoch = 1;
            /* Index by name (key borrows m->name; valid until meeting deactivates) */
            if (rtc_str_map_set(&mgr->name_index, m->name, m) != RTC_OK) {
                m->active = false;
                return NULL;
            }
            mgr->meeting_count++;
            return m;
        }
    }
    return NULL;
}

static void remove_peer_from_meeting(meeting_t *m, meeting_peer_t *peer) {
    for (int i = 0; i < m->peer_count; i++) {
        if (m->peers[i] == peer) {
            m->peers[i] = m->peers[m->peer_count - 1];
            m->peers[m->peer_count - 1] = NULL;
            m->peer_count--;
            peer->meeting_idx = -1;
            break;
        }
    }
}

static void maybe_destroy_meeting(meeting_manager_t *mgr, meeting_t *m) {
    if (m->peer_count == 0) {
        rtc_str_map_remove(&mgr->name_index, m->name);
        m->active = false;
        mgr->meeting_count--;
    }
}

static void emit_message(meeting_manager_t *mgr, meeting_peer_t *peer, const sig_msg_t *message) {
    char *json = sig_msg_build(message);
    if (json) {
        mgr->send_fn(peer, json, mgr->send_user);
        free(json);
    }
}

static void send_error(meeting_manager_t *mgr, meeting_peer_t *peer, const char *reason) {
    char *json = sig_msg_build_error(reason);
    if (json) {
        mgr->send_fn(peer, json, mgr->send_user);
        free(json);
    }
}

static const char *mode_name(meeting_mode_t mode) {
    return mode == MEETING_MODE_SFU ? "sfu" : "mesh";
}

static void roster(meeting_t *room, meeting_peer_t *recipient, sig_msg_t *message) {
    for (int i = 0; i < room->peer_count; i++) {
        meeting_peer_t *peer = room->peers[i];
        if (peer == recipient)
            continue;
        int index = message->peer_count++;
        snprintf(message->peers[index], sizeof(message->peers[index]), "%s", peer->id);
        snprintf(message->peer_names[index], sizeof(message->peer_names[index]), "%s",
                 peer->display_name);
    }
}

static void topology_broadcast(meeting_manager_t *mgr, meeting_t *room, sig_msg_type_t type,
                               const char *reason) {
    for (int i = 0; i < room->peer_count; i++) {
        sig_msg_t message = {.type = type, .version = 2, .epoch = room->pending_epoch};
        snprintf(message.mode, sizeof(message.mode), "%s", mode_name(room->desired_mode));
        if (reason)
            snprintf(message.reason, sizeof(message.reason), "%s", reason);
        if (type == SIG_MSG_TOPOLOGY_PREPARE)
            roster(room, room->peers[i], &message);
        emit_message(mgr, room->peers[i], &message);
    }
}

static void retire_epoch(meeting_manager_t *mgr, meeting_t *room, uint32_t epoch) {
    for (unsigned i = 0; i < room->mesh_session_count;) {
        if (room->mesh_sessions[i].epoch == epoch)
            room->mesh_sessions[i] = room->mesh_sessions[--room->mesh_session_count];
        else
            i++;
    }
    if (mgr->retire_fn)
        for (int i = 0; i < room->peer_count; i++)
            mgr->retire_fn(room->name, room->peers[i]->id, epoch, mgr->retire_user);
}

static void abort_transition(meeting_manager_t *mgr, meeting_t *room, const char *reason) {
    topology_broadcast(mgr, room, SIG_MSG_TOPOLOGY_ABORT, reason);
    retire_epoch(mgr, room, room->pending_epoch);
    room->pending_epoch = 0;
    room->phase = MEETING_STABLE;
    room->desired_mode = room->mode;
    room->retry_blocked = true;
    room->downgrade_at_ms = 0;
}

static void reconcile(meeting_manager_t *mgr, meeting_t *room, uint64_t now) {
    if (mgr->policy != MEETING_MODE_AUTO || room->phase != MEETING_STABLE || !room->peer_count ||
        room->retry_blocked)
        return;
    meeting_mode_t desired = room->mesh_failed || (unsigned)room->peer_count > mgr->p2p_max
                                 ? MEETING_MODE_SFU
                                 : MEETING_MODE_MESH;
    room->desired_mode = desired;
    if (desired == room->mode) {
        room->downgrade_at_ms = 0;
        return;
    }
    if (desired == MEETING_MODE_MESH) {
        if (!room->downgrade_at_ms)
            room->downgrade_at_ms = now + mgr->downgrade_delay_ms;
        if (now < room->downgrade_at_ms)
            return;
    }
    if (room->last_epoch == UINT32_MAX) {
        room->retry_blocked = true;
        return;
    }
    room->pending_epoch = ++room->last_epoch;
    room->phase = MEETING_PREPARING;
    room->deadline_ms = now + mgr->transition_timeout_ms;
    for (int i = 0; i < room->peer_count; i++)
        room->peers[i]->ready = room->peers[i]->ack = false;
    topology_broadcast(mgr, room, SIG_MSG_TOPOLOGY_PREPARE, NULL);
}

static void advance_transition(meeting_manager_t *mgr, meeting_t *room, uint64_t now) {
    bool complete = true;
    for (int i = 0; i < room->peer_count; i++)
        complete &= room->phase == MEETING_PREPARING ? room->peers[i]->ready : room->peers[i]->ack;
    if (!complete)
        return;
    if (room->phase == MEETING_PREPARING) {
        room->previous_epoch = room->epoch;
        room->previous_mode = room->mode;
        room->epoch = room->pending_epoch;
        room->mode = room->desired_mode;
        room->phase = MEETING_COMMITTING;
        room->deadline_ms = now + mgr->transition_timeout_ms;
        topology_broadcast(mgr, room, SIG_MSG_TOPOLOGY_COMMIT, NULL);
    } else if (room->phase == MEETING_COMMITTING) {
        retire_epoch(mgr, room, room->previous_epoch);
        room->previous_epoch = room->pending_epoch = 0;
        room->phase = MEETING_STABLE;
        room->downgrade_at_ms = 0;
        reconcile(mgr, room, now);
    }
}

int meeting_join_message(meeting_manager_t *mgr, meeting_peer_t *peer, const sig_msg_t *message,
                         uint64_t now) {
    const char *name = message->meeting;
    if (!name || !name[0])
        return -1;
    if (mgr->policy == MEETING_MODE_AUTO && (message->version != 2 || !message->can_mesh ||
                                             !message->can_sfu || !message->can_switch)) {
        send_error(mgr, peer, "auto rooms require version 2 mesh/sfu/topology_switch capabilities");
        return -1;
    }
    if (message->version >= 2 && ((mgr->policy == MEETING_MODE_MESH && !message->can_mesh) ||
                                  (mgr->policy == MEETING_MODE_SFU && !message->can_sfu))) {
        send_error(mgr, peer, "client does not support configured topology");
        return -1;
    }
    if (peer->meeting_idx >= 0)
        meeting_leave(mgr, peer);

    meeting_t *m = find_meeting(mgr, name);
    if (!m) {
        m = create_meeting(mgr, name);
        if (!m)
            return -1;
    }

    if (m->peer_count >= MEETING_MAX_PEERS) {
        char *err = sig_msg_build_error("meeting full");
        if (err) {
            mgr->send_fn(peer, err, mgr->send_user);
            free(err);
        }
        return -1;
    }
    unsigned version = message->version ? message->version : 1;
    if (m->peer_count && m->version != version) {
        send_error(mgr, peer, "room protocol version mismatch");
        return -1;
    }
    m->version = version;
    if (peer->has_joined)
        random_peer_id(peer->id, sizeof(peer->id));
    peer->has_joined = true;
    peer->protocol_failed = false;

    if (m->phase == MEETING_PREPARING)
        abort_transition(mgr, m, "membership changed");
    m->retry_blocked = false;
    peer->version = version;
    peer->can_mesh = message->can_mesh;
    peer->can_sfu = message->can_sfu;
    peer->can_switch = message->can_switch;
    peer->ready = peer->ack = m->phase == MEETING_COMMITTING;
    snprintf(peer->display_name, sizeof(peer->display_name), "%s", message->display_name);

    /* Add peer to meeting */
    int idx = (int)(m - mgr->meetings);
    m->peers[m->peer_count++] = peer;
    peer->meeting_idx = idx;

    /* Send "joined" to the new peer */
    sig_msg_t joined = {.type = SIG_MSG_JOINED,
                        .version = peer->version,
                        .epoch = peer->version >= 2 ? m->epoch : 0};
    snprintf(joined.peer_id, sizeof(joined.peer_id), "%s", peer->id);
    snprintf(joined.mode, sizeof(joined.mode), "%s", mode_name(m->mode));
    roster(m, peer, &joined);
    emit_message(mgr, peer, &joined);
    for (int i = 0; i < m->peer_count; i++) {
        meeting_peer_t *other = m->peers[i];
        if (other == peer)
            continue;
        sig_msg_t notification = {.type = SIG_MSG_PEER_JOINED,
                                  .version = other->version,
                                  .epoch = other->version >= 2 ? m->epoch : 0};
        snprintf(notification.peer_id, sizeof(notification.peer_id), "%s", peer->id);
        snprintf(notification.display_name, sizeof(notification.display_name), "%s",
                 peer->display_name);
        emit_message(mgr, other, &notification);
    }
    reconcile(mgr, m, now);
    return 0;
}

int meeting_join(meeting_manager_t *mgr, meeting_peer_t *peer, const char *name) {
    sig_msg_t message = {.type = SIG_MSG_JOIN, .version = 1};
    if (!name || strlen(name) >= sizeof(message.meeting))
        return -1;
    snprintf(message.meeting, sizeof(message.meeting), "%s", name);
    return meeting_join_message(mgr, peer, &message, rtc_time_ms());
}

void meeting_leave(meeting_manager_t *mgr, meeting_peer_t *peer) {
    if (peer->meeting_idx < 0)
        return;

    meeting_t *m = &mgr->meetings[peer->meeting_idx];
    if (m->phase == MEETING_PREPARING)
        abort_transition(mgr, m, "membership changed");
    if (mgr->retire_fn)
        mgr->retire_fn(m->name, peer->id, 0, mgr->retire_user);
    for (unsigned i = 0; i < m->mesh_session_count;) {
        meeting_mesh_session_t *session = &m->mesh_sessions[i];
        if (!strcmp(session->owner, peer->id) || !strcmp(session->remote, peer->id))
            *session = m->mesh_sessions[--m->mesh_session_count];
        else
            i++;
    }
    remove_peer_from_meeting(m, peer);

    for (int i = 0; i < m->peer_count; i++) {
        sig_msg_t left = {.type = SIG_MSG_PEER_LEFT,
                          .version = m->peers[i]->version,
                          .epoch = m->peers[i]->version >= 2 ? m->epoch : 0};
        snprintf(left.peer_id, sizeof(left.peer_id), "%s", peer->id);
        emit_message(mgr, m->peers[i], &left);
    }

    maybe_destroy_meeting(mgr, m);
    if (m->active) {
        m->retry_blocked = false;
        if (m->phase == MEETING_COMMITTING)
            advance_transition(mgr, m, rtc_time_ms());
        reconcile(mgr, m, rtc_time_ms());
    }
}

void meeting_peer_destroy(meeting_manager_t *mgr, meeting_peer_t *peer) {
    if (!peer)
        return;
    meeting_leave(mgr, peer);
    free(peer);
}

int meeting_route_message(meeting_manager_t *mgr, meeting_peer_t *from, const char *to_id,
                          const char *json) {
    if (from->meeting_idx < 0)
        return -1;

    meeting_t *m = &mgr->meetings[from->meeting_idx];
    for (int i = 0; i < m->peer_count; i++) {
        if (strcmp(m->peers[i]->id, to_id) == 0) {
            mgr->send_fn(m->peers[i], json, mgr->send_user);
            return 0;
        }
    }
    return -1;
}

meeting_peer_t *meeting_find_peer(meeting_manager_t *mgr, const char *room, const char *id) {
    meeting_t *meeting = find_meeting(mgr, room);
    if (meeting)
        for (int i = 0; i < meeting->peer_count; i++)
            if (strcmp(meeting->peers[i]->id, id) == 0)
                return meeting->peers[i];
    return NULL;
}

int meeting_handle_topology(meeting_manager_t *mgr, meeting_peer_t *peer, const sig_msg_t *message,
                            uint64_t now) {
    if (!peer || peer->meeting_idx < 0 || peer->version != 2 || message->version != 2 ||
        mgr->policy != MEETING_MODE_AUTO)
        return -1;
    meeting_t *room = &mgr->meetings[peer->meeting_idx];
    if (message->type == SIG_MSG_TOPOLOGY_REQUEST) {
        if (message->epoch != room->epoch || room->mode != MEETING_MODE_MESH ||
            strcmp(message->mode, "sfu") || !message->reason[0] || room->mesh_failed)
            return -1;
        if (room->phase == MEETING_PREPARING)
            abort_transition(mgr, room, "P2P failure superseded transition");
        room->mesh_failed = true;
        room->retry_blocked = false;
        reconcile(mgr, room, now);
        return 0;
    }
    if (message->epoch != room->pending_epoch ||
        strcmp(message->mode, mode_name(room->desired_mode)))
        return -1;
    if (message->type == SIG_MSG_TOPOLOGY_ABORT && room->phase == MEETING_PREPARING) {
        abort_transition(mgr, room,
                         message->reason[0] ? message->reason : "client preparation failed");
        return 0;
    }
    if (message->type == SIG_MSG_TOPOLOGY_READY && room->phase == MEETING_PREPARING)
        peer->ready = true;
    else if (message->type == SIG_MSG_TOPOLOGY_ACK && room->phase == MEETING_COMMITTING)
        peer->ack = true;
    else
        return -1;
    advance_transition(mgr, room, now);
    return 0;
}

void meeting_tick(meeting_manager_t *mgr, uint64_t now) {
    for (int i = 0; i < MEETING_MAX_ROOMS; i++) {
        meeting_t *room = &mgr->meetings[i];
        if (!room->active)
            continue;
        if (room->phase == MEETING_PREPARING && now >= room->deadline_ms)
            abort_transition(mgr, room, "topology preparation timed out");
        else if (room->phase == MEETING_COMMITTING && now >= room->deadline_ms) {
            /* An ACKing participant may already have retired its old graph. */
            for (int j = 0; j < room->peer_count; j++)
                if (!room->peers[j]->ack) {
                    send_error(mgr, room->peers[j], "topology acknowledgement timed out");
                    room->peers[j]->protocol_failed = true;
                    room->peers[j]->ack = true;
                }
            advance_transition(mgr, room, now);
        }
        reconcile(mgr, room, now);
    }
}

int meeting_validate_session(meeting_manager_t *mgr, meeting_peer_t *peer,
                             const sig_msg_t *message) {
    if (!peer || peer->meeting_idx < 0 || message->version != peer->version ||
        (message->type != SIG_MSG_OFFER && message->type != SIG_MSG_ANSWER &&
         message->type != SIG_MSG_CANDIDATE))
        return -1;
    meeting_t *room = &mgr->meetings[peer->meeting_idx];
    meeting_mode_t mode = room->mode;
    if (peer->version >= 2) {
        if (room->phase == MEETING_PREPARING && message->epoch == room->pending_epoch)
            mode = room->desired_mode;
        else if (room->phase == MEETING_COMMITTING && message->epoch == room->previous_epoch)
            mode = room->previous_mode;
        else if (message->epoch != room->epoch)
            return -1;
    }
    if (strcmp(message->to, "sfu") != 0 &&
        (!meeting_find_peer(mgr, room->name, message->to) || strcmp(message->to, peer->id) == 0))
        return -1;
    if (mode == MEETING_MODE_MESH && strcmp(message->to, "sfu") == 0)
        return -1;
    if (peer->version >= 2) {
        char expected[SIG_MAX_SESSION_ID];
        sig_session_role_t role = mode == MEETING_MODE_MESH         ? SIG_SESSION_MESH
                                  : strcmp(message->to, "sfu") == 0 ? SIG_SESSION_PUBLISH
                                                                    : SIG_SESSION_SUBSCRIBE;
        if (sig_session_id(expected, role, message->epoch, peer->id, message->to) != 0 ||
            strcmp(expected, message->session_id))
            return -1;
        if (message->type == SIG_MSG_OFFER && mode == MEETING_MODE_SFU &&
            role != SIG_SESSION_PUBLISH)
            return -1;
        if (message->type == SIG_MSG_OFFER && mode == MEETING_MODE_MESH) {
            unsigned index = 0;
            while (index < room->mesh_session_count &&
                   strcmp(room->mesh_sessions[index].id, expected))
                index++;
            if (index == room->mesh_session_count) {
                if (strcmp(peer->id, message->to) > 0 ||
                    room->mesh_session_count == MEETING_MAX_MESH_SESSIONS)
                    return -1;
                meeting_mesh_session_t *session = &room->mesh_sessions[room->mesh_session_count++];
                session->epoch = message->epoch;
                snprintf(session->id, sizeof(session->id), "%s", expected);
                snprintf(session->owner, sizeof(session->owner), "%s", peer->id);
                snprintf(session->remote, sizeof(session->remote), "%s", message->to);
            }
        }
        if (message->type == SIG_MSG_ANSWER && mode == MEETING_MODE_SFU &&
            role == SIG_SESSION_PUBLISH)
            return -1;
    }
    return mode;
}
