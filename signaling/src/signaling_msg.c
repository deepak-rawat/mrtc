/*
 * signaling_msg.c - JSON message build/parse using cJSON.
 */
#include "signaling/signaling_msg.h"
#include <cjson/cJSON.h>
#include <string.h>
#include <stdlib.h>

/* ---- Type string mapping ---- */
static const char *type_strings[] = {
    [SIG_MSG_JOIN] = "join",           [SIG_MSG_LEAVE] = "leave",
    [SIG_MSG_JOINED] = "joined",       [SIG_MSG_PEER_JOINED] = "peer_joined",
    [SIG_MSG_PEER_LEFT] = "peer_left", [SIG_MSG_OFFER] = "offer",
    [SIG_MSG_ANSWER] = "answer",       [SIG_MSG_CANDIDATE] = "candidate",
    [SIG_MSG_ERROR] = "error",
};

static sig_msg_type_t type_from_string(const char *s) {
    for (int i = 0; i <= SIG_MSG_ERROR; i++) {
        if (strcmp(s, type_strings[i]) == 0)
            return (sig_msg_type_t)i;
    }
    return SIG_MSG_UNKNOWN;
}

/* ---- Helper: create base object with "type" ---- */
static cJSON *make_base(sig_msg_type_t type) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "type", type_strings[type]);
    return obj;
}

static char *finish(cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return s;
}

/* ---- Build: Client → Server ---- */

char *sig_msg_build_join(const char *meeting) {
    cJSON *obj = make_base(SIG_MSG_JOIN);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "meeting", meeting);
    return finish(obj);
}

char *sig_msg_build_leave(void) {
    cJSON *obj = make_base(SIG_MSG_LEAVE);
    if (!obj)
        return NULL;
    return finish(obj);
}

char *sig_msg_build_offer(const char *to, const char *sdp) {
    cJSON *obj = make_base(SIG_MSG_OFFER);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "to", to);
    cJSON_AddStringToObject(obj, "sdp", sdp);
    return finish(obj);
}

char *sig_msg_build_answer(const char *to, const char *sdp) {
    cJSON *obj = make_base(SIG_MSG_ANSWER);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "to", to);
    cJSON_AddStringToObject(obj, "sdp", sdp);
    return finish(obj);
}

char *sig_msg_build_candidate(const char *to, const char *candidate) {
    cJSON *obj = make_base(SIG_MSG_CANDIDATE);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "to", to);
    cJSON_AddStringToObject(obj, "candidate", candidate);
    return finish(obj);
}

/* ---- Build: Server → Client ---- */

char *sig_msg_build_joined(const char *peer_id, const char **peers, int peer_count) {
    cJSON *obj = make_base(SIG_MSG_JOINED);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "peer_id", peer_id);
    cJSON *arr = cJSON_AddArrayToObject(obj, "peers");
    for (int i = 0; i < peer_count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(peers[i]));
    return finish(obj);
}

char *sig_msg_build_peer_joined(const char *peer_id) {
    cJSON *obj = make_base(SIG_MSG_PEER_JOINED);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "peer_id", peer_id);
    return finish(obj);
}

char *sig_msg_build_peer_left(const char *peer_id) {
    cJSON *obj = make_base(SIG_MSG_PEER_LEFT);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "peer_id", peer_id);
    return finish(obj);
}

char *sig_msg_build_relay_offer(const char *from, const char *sdp) {
    cJSON *obj = make_base(SIG_MSG_OFFER);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "from", from);
    cJSON_AddStringToObject(obj, "sdp", sdp);
    return finish(obj);
}

char *sig_msg_build_relay_answer(const char *from, const char *sdp) {
    cJSON *obj = make_base(SIG_MSG_ANSWER);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "from", from);
    cJSON_AddStringToObject(obj, "sdp", sdp);
    return finish(obj);
}

char *sig_msg_build_relay_candidate(const char *from, const char *candidate) {
    cJSON *obj = make_base(SIG_MSG_CANDIDATE);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "from", from);
    cJSON_AddStringToObject(obj, "candidate", candidate);
    return finish(obj);
}

char *sig_msg_build_error(const char *message) {
    cJSON *obj = make_base(SIG_MSG_ERROR);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "message", message);
    return finish(obj);
}

/* ---- Parse ---- */

static void safe_copy(char *dst, size_t dst_size, const cJSON *item) {
    if (cJSON_IsString(item) && item->valuestring) {
        size_t len = strlen(item->valuestring);
        if (len >= dst_size)
            len = dst_size - 1;
        memcpy(dst, item->valuestring, len);
        dst[len] = '\0';
    }
}

int sig_msg_parse(sig_msg_t *msg, const char *json, size_t json_len) {
    memset(msg, 0, sizeof(*msg));
    msg->type = SIG_MSG_UNKNOWN;

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root)
        return -1;

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return -1;
    }

    msg->type = type_from_string(type_item->valuestring);
    if (msg->type == SIG_MSG_UNKNOWN) {
        cJSON_Delete(root);
        return -1;
    }

    switch (msg->type) {
        case SIG_MSG_JOIN:
            safe_copy(msg->meeting, sizeof(msg->meeting),
                      cJSON_GetObjectItemCaseSensitive(root, "meeting"));
            break;

        case SIG_MSG_LEAVE:
            break;

        case SIG_MSG_JOINED:
            safe_copy(msg->peer_id, sizeof(msg->peer_id),
                      cJSON_GetObjectItemCaseSensitive(root, "peer_id"));
            {
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "peers");
                if (cJSON_IsArray(arr)) {
                    int n = cJSON_GetArraySize(arr);
                    if (n > SIG_MAX_PEERS)
                        n = SIG_MAX_PEERS;
                    for (int i = 0; i < n; i++) {
                        cJSON *item = cJSON_GetArrayItem(arr, i);
                        safe_copy(msg->peers[i], SIG_MAX_PEER_ID, item);
                    }
                    msg->peer_count = n;
                }
            }
            break;

        case SIG_MSG_PEER_JOINED:
        case SIG_MSG_PEER_LEFT:
            safe_copy(msg->peer_id, sizeof(msg->peer_id),
                      cJSON_GetObjectItemCaseSensitive(root, "peer_id"));
            break;

        case SIG_MSG_OFFER:
        case SIG_MSG_ANSWER:
            safe_copy(msg->to, sizeof(msg->to), cJSON_GetObjectItemCaseSensitive(root, "to"));
            safe_copy(msg->from, sizeof(msg->from), cJSON_GetObjectItemCaseSensitive(root, "from"));
            safe_copy(msg->sdp, sizeof(msg->sdp), cJSON_GetObjectItemCaseSensitive(root, "sdp"));
            break;

        case SIG_MSG_CANDIDATE:
            safe_copy(msg->to, sizeof(msg->to), cJSON_GetObjectItemCaseSensitive(root, "to"));
            safe_copy(msg->from, sizeof(msg->from), cJSON_GetObjectItemCaseSensitive(root, "from"));
            safe_copy(msg->candidate, sizeof(msg->candidate),
                      cJSON_GetObjectItemCaseSensitive(root, "candidate"));
            break;

        case SIG_MSG_ERROR:
            safe_copy(msg->error_msg, sizeof(msg->error_msg),
                      cJSON_GetObjectItemCaseSensitive(root, "message"));
            break;

        default:
            break;
    }

    cJSON_Delete(root);
    return 0;
}
