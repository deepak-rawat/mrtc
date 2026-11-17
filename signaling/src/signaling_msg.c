/*
 * JSON message build/parse using cJSON.
 */
#include "signaling/signaling_msg.h"
#include <cjson/cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <rtc/rtc_common.h>

static const char *type_strings[] = {
    [SIG_MSG_JOIN] = "join",
    [SIG_MSG_LEAVE] = "leave",
    [SIG_MSG_JOINED] = "joined",
    [SIG_MSG_PEER_JOINED] = "peer_joined",
    [SIG_MSG_PEER_LEFT] = "peer_left",
    [SIG_MSG_OFFER] = "offer",
    [SIG_MSG_ANSWER] = "answer",
    [SIG_MSG_CANDIDATE] = "candidate",
    [SIG_MSG_ERROR] = "error",
    [SIG_MSG_TOPOLOGY_PREPARE] = "topology_prepare",
    [SIG_MSG_TOPOLOGY_READY] = "topology_ready",
    [SIG_MSG_TOPOLOGY_COMMIT] = "topology_commit",
    [SIG_MSG_TOPOLOGY_ACK] = "topology_ack",
    [SIG_MSG_TOPOLOGY_ABORT] = "topology_abort",
    [SIG_MSG_TOPOLOGY_REQUEST] = "topology_request",
};

static sig_msg_type_t type_from_string(const char *s) {
    for (int i = 0; i < SIG_MSG_UNKNOWN; i++) {
        if (strcmp(s, type_strings[i]) == 0)
            return (sig_msg_type_t)i;
    }
    return SIG_MSG_UNKNOWN;
}

static cJSON *make_base(sig_msg_type_t type) {
    if (type < 0 || type >= SIG_MSG_UNKNOWN)
        return NULL;
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;
    if (!cJSON_AddStringToObject(obj, "type", type_strings[type])) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

static char *finish(cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return s;
}

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

char *sig_msg_build_joined(const char *peer_id, const char **peers, int peer_count) {
    return sig_msg_build_joined_mode(peer_id, peers, peer_count, "mesh");
}

char *sig_msg_build_joined_mode(const char *peer_id, const char **peers, int peer_count,
                                const char *mode) {
    cJSON *obj = make_base(SIG_MSG_JOINED);
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "peer_id", peer_id);
    cJSON_AddStringToObject(obj, "mode", mode);
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

typedef struct {
    const char *name;
    size_t offset;
    size_t capacity;
} text_field_t;

#define TEXT_FIELD(name, member) \
    {name, offsetof(sig_msg_t, member), sizeof(((sig_msg_t *)0)->member)}
static const text_field_t text_fields[] = {
    TEXT_FIELD("meeting", meeting),
    TEXT_FIELD("mode", mode),
    TEXT_FIELD("peer_id", peer_id),
    TEXT_FIELD("to", to),
    TEXT_FIELD("from", from),
    TEXT_FIELD("sdp", sdp),
    TEXT_FIELD("candidate", candidate),
    TEXT_FIELD("message", error_msg),
    TEXT_FIELD("reason", reason),
    TEXT_FIELD("session_id", session_id),
    TEXT_FIELD("display_name", display_name),
};
#undef TEXT_FIELD

static bool valid_id(const char *id) {
    if (!id || !id[0] || strlen(id) >= SIG_MAX_PEER_ID)
        return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (*p >= 128 || (!isalnum(*p) && *p != '_' && *p != '-'))
            return false;
    return true;
}

static bool concrete_mode(const char *mode) {
    return strcmp(mode, "mesh") == 0 || strcmp(mode, "sfu") == 0;
}

static bool valid_message(const sig_msg_t *msg) {
    unsigned version = msg->version ? msg->version : 1;
    if (msg->type < 0 || msg->type >= SIG_MSG_UNKNOWN ||
        (version != 1 && version != SIG_PROTOCOL_VERSION) || msg->peer_count < 0 ||
        msg->peer_count > SIG_MAX_PEERS)
        return false;
    for (size_t i = 0; i < sizeof(text_fields) / sizeof(text_fields[0]); i++)
        if (!memchr((const char *)msg + text_fields[i].offset, '\0', text_fields[i].capacity))
            return false;
    for (int i = 0; i < msg->peer_count; i++) {
        if (!memchr(msg->peers[i], '\0', SIG_MAX_PEER_ID) ||
            !memchr(msg->peer_names[i], '\0', SIG_MAX_DISPLAY_NAME) || !valid_id(msg->peers[i]))
            return false;
        for (int j = 0; j < i; j++)
            if (strcmp(msg->peers[i], msg->peers[j]) == 0)
                return false;
    }
    if (version == 1)
        return msg->type <= SIG_MSG_ERROR;
    if ((msg->peer_id[0] && !valid_id(msg->peer_id)) || (msg->to[0] && !valid_id(msg->to)) ||
        (msg->from[0] && !valid_id(msg->from)))
        return false;
    switch (msg->type) {
        case SIG_MSG_JOIN:
            return msg->meeting[0] != '\0';
        case SIG_MSG_JOINED:
            return msg->peer_id[0] && msg->epoch && concrete_mode(msg->mode);
        case SIG_MSG_PEER_JOINED:
        case SIG_MSG_PEER_LEFT:
            return msg->peer_id[0] && msg->epoch;
        case SIG_MSG_OFFER:
        case SIG_MSG_ANSWER:
            return msg->epoch && msg->session_id[0] && msg->sdp[0] && (msg->to[0] || msg->from[0]);
        case SIG_MSG_CANDIDATE:
            return msg->epoch && msg->session_id[0] && (msg->to[0] || msg->from[0]);
        case SIG_MSG_TOPOLOGY_PREPARE:
        case SIG_MSG_TOPOLOGY_READY:
        case SIG_MSG_TOPOLOGY_COMMIT:
        case SIG_MSG_TOPOLOGY_ACK:
            return msg->epoch && concrete_mode(msg->mode);
        case SIG_MSG_TOPOLOGY_ABORT:
            return msg->epoch && concrete_mode(msg->mode) && msg->reason[0];
        case SIG_MSG_TOPOLOGY_REQUEST:
            return msg->epoch && strcmp(msg->mode, "sfu") == 0 && msg->reason[0];
        case SIG_MSG_ERROR:
            return msg->error_msg[0] || msg->reason[0];
        default:
            return true;
    }
}

char *sig_msg_build(const sig_msg_t *msg) {
    if (!msg || !valid_message(msg))
        return NULL;
    cJSON *root = make_base(msg->type);
    if (!root)
        return NULL;
    if (msg->version > 1 && !cJSON_AddNumberToObject(root, "version", msg->version))
        goto fail;
    if (msg->epoch && !cJSON_AddNumberToObject(root, "epoch", msg->epoch))
        goto fail;
    for (size_t i = 0; i < sizeof(text_fields) / sizeof(text_fields[0]); i++) {
        const char *text = (const char *)msg + text_fields[i].offset;
        if ((text[0] ||
             (msg->type == SIG_MSG_CANDIDATE && strcmp(text_fields[i].name, "candidate") == 0)) &&
            !cJSON_AddStringToObject(root, text_fields[i].name, text))
            goto fail;
    }
    if (msg->type == SIG_MSG_JOIN && msg->version == SIG_PROTOCOL_VERSION) {
        cJSON *caps = cJSON_AddObjectToObject(root, "capabilities");
        if (!caps || !cJSON_AddBoolToObject(caps, "mesh", msg->can_mesh) ||
            !cJSON_AddBoolToObject(caps, "sfu", msg->can_sfu) ||
            !cJSON_AddBoolToObject(caps, "topology_switch", msg->can_switch))
            goto fail;
    }
    if (msg->type == SIG_MSG_JOINED || msg->type == SIG_MSG_TOPOLOGY_PREPARE ||
        msg->type == SIG_MSG_TOPOLOGY_COMMIT || msg->peer_count) {
        cJSON *peers = cJSON_AddArrayToObject(root, "peers");
        if (!peers)
            goto fail;
        cJSON *names = NULL;
        for (int i = 0; i < msg->peer_count; i++) {
            cJSON *id = cJSON_CreateString(msg->peers[i]);
            if (!id)
                goto fail;
            if (!cJSON_AddItemToArray(peers, id)) {
                cJSON_Delete(id);
                goto fail;
            }
            if (msg->peer_names[i][0]) {
                if (!names)
                    names = cJSON_AddObjectToObject(root, "peer_names");
                if (!names || !cJSON_AddStringToObject(names, msg->peers[i], msg->peer_names[i]))
                    goto fail;
            }
        }
    }
    return finish(root);
fail:
    cJSON_Delete(root);
    return NULL;
}

static bool read_uint(const cJSON *root, const char *name, uint32_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item)
        return true;
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0) || !(item->valuedouble <= UINT32_MAX))
        return false;
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble)
        return false;
    *out = value;
    return true;
}

static bool read_bool(const cJSON *root, const char *name, bool *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item)
        return true;
    if (!cJSON_IsBool(item))
        return false;
    *out = cJSON_IsTrue(item);
    return true;
}

static bool copy_json_string(char *out, size_t capacity, const cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= capacity)
        return false;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

int sig_msg_parse(sig_msg_t *msg, const char *json, size_t json_len) {
    if (!msg || !json)
        return -1;
    memset(msg, 0, sizeof(*msg));
    msg->type = SIG_MSG_UNKNOWN;
    msg->version = 1;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &end, false);
    if (!root)
        return -1;
    while (end < json + json_len && isspace((unsigned char)*end))
        end++;
    if (end != json + json_len || !cJSON_IsObject(root))
        goto invalid;
    for (const cJSON *item = root->child; item; item = item->next)
        for (const cJSON *previous = root->child; previous != item; previous = previous->next)
            if (strcmp(item->string, previous->string) == 0)
                goto invalid;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type))
        goto invalid;
    msg->type = type_from_string(type->valuestring);
    uint32_t version = 1;
    if (!read_uint(root, "version", &version) ||
        (version != 1 && version != SIG_PROTOCOL_VERSION) || !read_uint(root, "epoch", &msg->epoch))
        goto invalid;
    msg->version = version;
    for (size_t i = 0; i < sizeof(text_fields) / sizeof(text_fields[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, text_fields[i].name);
        if (item &&
            !copy_json_string((char *)msg + text_fields[i].offset, text_fields[i].capacity, item))
            goto invalid;
    }
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (caps && (!cJSON_IsObject(caps) || !read_bool(caps, "mesh", &msg->can_mesh) ||
                 !read_bool(caps, "sfu", &msg->can_sfu) ||
                 !read_bool(caps, "topology_switch", &msg->can_switch)))
        goto invalid;
    const cJSON *peers = cJSON_GetObjectItemCaseSensitive(root, "peers");
    if (peers) {
        if (!cJSON_IsArray(peers) || cJSON_GetArraySize(peers) > SIG_MAX_PEERS)
            goto invalid;
        msg->peer_count = cJSON_GetArraySize(peers);
        for (int i = 0; i < msg->peer_count; i++)
            if (!copy_json_string(msg->peers[i], sizeof(msg->peers[i]),
                                  cJSON_GetArrayItem(peers, i)))
                goto invalid;
    }
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(root, "peer_names");
    if (names) {
        if (!cJSON_IsObject(names))
            goto invalid;
        for (const cJSON *item = names->child; item; item = item->next) {
            int index = 0;
            while (index < msg->peer_count && strcmp(item->string, msg->peers[index]))
                index++;
            if (index == msg->peer_count ||
                !copy_json_string(msg->peer_names[index], sizeof(msg->peer_names[index]), item))
                goto invalid;
        }
    }
    if (version == SIG_PROTOCOL_VERSION &&
        ((msg->type == SIG_MSG_CANDIDATE &&
          !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "candidate"))) ||
         ((msg->type == SIG_MSG_JOINED || msg->type == SIG_MSG_TOPOLOGY_PREPARE) && !peers)))
        goto invalid;
    if (!valid_message(msg))
        goto invalid;
    cJSON_Delete(root);
    return 0;
invalid:
    cJSON_Delete(root);
    return -1;
}

int sig_session_id(char out[SIG_MAX_SESSION_ID], sig_session_role_t role, uint32_t epoch,
                   const char *owner, const char *remote) {
    if (!out || !epoch || !valid_id(owner) ||
        (role != SIG_SESSION_PUBLISH && (!valid_id(remote) || strcmp(owner, remote) == 0)))
        return RTC_ERR_INVALID;
    int len;
    if (role == SIG_SESSION_PUBLISH) {
        len = snprintf(out, SIG_MAX_SESSION_ID, "p:%" PRIu32 ":%s", epoch, owner);
    } else if (role == SIG_SESSION_MESH) {
        const char *first = strcmp(owner, remote) < 0 ? owner : remote;
        const char *second = first == owner ? remote : owner;
        len = snprintf(out, SIG_MAX_SESSION_ID, "m:%" PRIu32 ":%s:%s", epoch, first, second);
    } else if (role == SIG_SESSION_SUBSCRIBE) {
        len = snprintf(out, SIG_MAX_SESSION_ID, "s:%" PRIu32 ":%s:%s", epoch, owner, remote);
    } else {
        return RTC_ERR_INVALID;
    }
    return len >= 0 && len < SIG_MAX_SESSION_ID ? RTC_OK : RTC_ERR_INVALID;
}

int sig_dc_write(uint8_t *out, size_t capacity, size_t *out_len, const char *peer,
                 uint64_t message_id, const uint8_t *data, size_t len) {
    if (!out || !out_len || !peer || (!valid_id(peer) && strcmp(peer, "*")) || !message_id ||
        (!data && len) || len > SIG_MAX_DC_MESSAGE - SIG_DC_V2_HEADER_SIZE)
        return RTC_ERR_INVALID;
    if (capacity < SIG_DC_V2_HEADER_SIZE || len > capacity - SIG_DC_V2_HEADER_SIZE)
        return RTC_ERR_NOMEM;
    memset(out, 0, SIG_DC_V2_HEADER_SIZE);
    memcpy(out, "MRC2", 4);
    memcpy(out + 4, peer, strlen(peer));
    for (int i = 0; i < 8; i++)
        out[SIG_DC_HEADER_SIZE + i] = (uint8_t)(message_id >> (56 - 8 * i));
    if (len)
        memcpy(out + SIG_DC_V2_HEADER_SIZE, data, len);
    *out_len = SIG_DC_V2_HEADER_SIZE + len;
    return RTC_OK;
}

int sig_dc_read(const uint8_t *wire, size_t len, char peer[SIG_MAX_PEER_ID], uint64_t *message_id,
                const uint8_t **data, size_t *data_len) {
    if (!wire || !peer || !message_id || !data || !data_len || len < SIG_DC_HEADER_SIZE ||
        len > SIG_MAX_DC_MESSAGE)
        return RTC_ERR_INVALID;
    bool version2 = memcmp(wire, "MRC2", 4) == 0;
    size_t header = version2 ? SIG_DC_V2_HEADER_SIZE : SIG_DC_HEADER_SIZE;
    if ((!version2 && memcmp(wire, "MRTC", 4)) || len < header)
        return RTC_ERR_INVALID;
    const uint8_t *terminator = memchr(wire + 4, 0, SIG_MAX_PEER_ID);
    if (!terminator)
        return RTC_ERR_INVALID;
    size_t peer_len = (size_t)(terminator - wire - 4);
    memcpy(peer, wire + 4, peer_len);
    peer[peer_len] = '\0';
    if (!valid_id(peer) && strcmp(peer, "*"))
        return RTC_ERR_INVALID;
    for (size_t i = peer_len; i < SIG_MAX_PEER_ID; i++)
        if (wire[4 + i])
            return RTC_ERR_INVALID;
    *message_id = 0;
    if (version2) {
        for (int i = 0; i < 8; i++)
            *message_id = (*message_id << 8) | wire[SIG_DC_HEADER_SIZE + i];
        if (!*message_id)
            return RTC_ERR_INVALID;
    }
    *data = wire + header;
    *data_len = len - header;
    return RTC_OK;
}
