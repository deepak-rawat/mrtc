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

static void broadcast_to_meeting(meeting_manager_t *mgr, meeting_t *m, const char *json,
                                 meeting_peer_t *exclude) {
    for (int i = 0; i < m->peer_count; i++) {
        if (m->peers[i] != exclude)
            mgr->send_fn(m->peers[i], json, mgr->send_user);
    }
}

int meeting_join(meeting_manager_t *mgr, meeting_peer_t *peer, const char *name) {
    if (!name || !name[0])
        return -1;

    /* Leave current meeting first */
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

    /* Collect existing peer IDs for the "joined" message */
    const char *peer_ids[MEETING_MAX_PEERS];
    for (int i = 0; i < m->peer_count; i++)
        peer_ids[i] = m->peers[i]->id;
    int existing_count = m->peer_count;

    /* Add peer to meeting */
    int idx = (int)(m - mgr->meetings);
    m->peers[m->peer_count++] = peer;
    peer->meeting_idx = idx;

    /* Send "joined" to the new peer */
    char *joined = sig_msg_build_joined(peer->id, peer_ids, existing_count);
    if (joined) {
        mgr->send_fn(peer, joined, mgr->send_user);
        free(joined);
    }

    /* Broadcast "peer_joined" to existing peers */
    char *pj = sig_msg_build_peer_joined(peer->id);
    if (pj) {
        broadcast_to_meeting(mgr, m, pj, peer);
        free(pj);
    }

    return 0;
}

void meeting_leave(meeting_manager_t *mgr, meeting_peer_t *peer) {
    if (peer->meeting_idx < 0)
        return;

    meeting_t *m = &mgr->meetings[peer->meeting_idx];
    remove_peer_from_meeting(m, peer);

    /* Notify remaining peers */
    char *pl = sig_msg_build_peer_left(peer->id);
    if (pl) {
        broadcast_to_meeting(mgr, m, pl, NULL);
        free(pl);
    }

    maybe_destroy_meeting(mgr, m);
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
    return -1; /* Target not found in meeting */
}
