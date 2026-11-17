#include "control.h"
#include <math.h>
#include <string.h>

bool app_control_valid_utf8(const char *text, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char lead = (unsigned char)text[i++];
        if (lead < 128)
            continue;
        unsigned remaining, code, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            remaining = 1;
            code = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            remaining = 2;
            code = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            remaining = 3;
            code = lead & 7;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining > length - i)
            return false;
        while (remaining--) {
            unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}

static bool string_field(const cJSON *json, const char *key, size_t max, bool required,
                         bool empty) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return !value ? !required
                  : cJSON_IsString(value) && (empty || value->valuestring[0]) &&
                        strlen(value->valuestring) <= max;
}

static const char *text_field(const cJSON *json, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return value ? value->valuestring : NULL;
}

static bool allowed_fields(const cJSON *json, const char *allowed) {
    const cJSON *field;
    cJSON_ArrayForEach(field, json) {
        bool found = false;
        const char *start = allowed;
        while (*start) {
            const char *end = strchr(start, ',');
            size_t length = end ? (size_t)(end - start) : strlen(start);
            if (strlen(field->string) == length && !strncmp(start, field->string, length))
                found = true;
            start += length;
            if (*start)
                ++start;
        }
        if (!found)
            return false;
        for (const cJSON *previous = json->child; previous != field; previous = previous->next)
            if (!strcmp(previous->string, field->string))
                return false;
    }
    return true;
}

static bool has_null_escape(const char *text) {
    bool in_string = false;
    for (size_t i = 0; text[i]; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 && (in_string || (ch != '\n' && ch != '\r' && ch != '\t')))
            return true;
        if (text[i] == '"')
            in_string = !in_string;
        if (text[i] != '\\')
            continue;
        ++i;
        if (!text[i])
            break;
        if (!strncmp(text + i, "u0000", 5))
            return true;
    }
    return false;
}

int app_command_parse(app_command_t *out, const char *line, const char **error) {
    memset(out, 0, sizeof(*out));
    *error = "invalid JSON object";
    if (!line || strlen(line) > APP_CONTROL_MAX_LINE || has_null_escape(line) ||
        !app_control_valid_utf8(line, strlen(line)))
        return RTC_ERR_INVALID;
    out->json = cJSON_ParseWithOpts(line, NULL, true);
    if (!cJSON_IsObject(out->json))
        return RTC_ERR_INVALID;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(out->json, "id");
    if ((cJSON_IsString(id) && id->valuestring[0] && strlen(id->valuestring) <= 64) ||
        (cJSON_IsNumber(id) && isfinite(id->valuedouble) &&
         fabs(id->valuedouble) <= 9007199254740991.0 && floor(id->valuedouble) == id->valuedouble))
        out->id = id;
    else {
        *error = "id must be a nonempty string (max 64) or safe integer";
        return RTC_ERR_INVALID;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(out->json, "version");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1) {
        *error = "version must be 1";
        return RTC_ERR_INVALID;
    }
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(out->json, "command");
    if (!cJSON_IsString(command)) {
        *error = "command must be a string";
        return RTC_ERR_INVALID;
    }
    const char *name = command->valuestring;
    const char *fields = "version,id,command";
    if (!strcmp(name, "join")) {
        out->type = APP_COMMAND_JOIN;
        fields = "version,id,command,server,room,name,video,audio,stun,data_only";
        *error = "invalid join fields";
        if (!string_field(out->json, "server", APP_SESSION_SERVER_SIZE - 1, true, false) ||
            !string_field(out->json, "room", SIG_MAX_MEETING - 1, true, false) ||
            !string_field(out->json, "name", SIG_MAX_DISPLAY_NAME - 1, true, false) ||
            !string_field(out->json, "stun", 255, false, true) ||
            !string_field(out->json, "video", 9, true, false) ||
            !string_field(out->json, "audio", 9, true, false))
            return RTC_ERR_INVALID;
        const char *video = text_field(out->json, "video");
        const char *audio = text_field(out->json, "audio");
        if ((strcmp(video, "synthetic") && strcmp(video, "none")) ||
            (strcmp(audio, "synthetic") && strcmp(audio, "none")))
            return RTC_ERR_INVALID;
        out->options = (app_session_options_t){
            .server = text_field(out->json, "server"),
            .room = text_field(out->json, "room"),
            .name = text_field(out->json, "name"),
            .stun = text_field(out->json, "stun"),
            .video = !strcmp(video, "synthetic") ? APP_MEDIA_SYNTHETIC : APP_MEDIA_NONE,
            .audio = !strcmp(audio, "synthetic") ? APP_MEDIA_SYNTHETIC : APP_MEDIA_NONE};
        if (app_session_validate_options(&out->options) != RTC_OK)
            return RTC_ERR_INVALID;
    } else if (!strcmp(name, "send_message")) {
        out->type = APP_COMMAND_SEND_MESSAGE;
        fields = "version,id,command,text,peer_id";
        *error = "invalid message fields";
        if (!string_field(out->json, "text", APP_CONTROL_MAX_TEXT, true, false) ||
            !string_field(out->json, "peer_id", SIG_MAX_PEER_ID - 1, false, false))
            return RTC_ERR_INVALID;
        out->text = text_field(out->json, "text");
        out->peer_id = text_field(out->json, "peer_id");
    } else if (!strcmp(name, "mute")) {
        out->type = APP_COMMAND_MUTE;
        fields = "version,id,command,source,muted";
        *error = "source must be video or audio and muted must be boolean";
        if (!string_field(out->json, "source", 5, true, false) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(out->json, "muted")))
            return RTC_ERR_INVALID;
        out->source = text_field(out->json, "source");
        if (strcmp(out->source, "video") && strcmp(out->source, "audio"))
            return RTC_ERR_INVALID;
        out->muted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out->json, "muted"));
    } else if (!strcmp(name, "leave")) {
        out->type = APP_COMMAND_LEAVE;
    } else if (!strcmp(name, "stats")) {
        out->type = APP_COMMAND_STATS;
    } else if (!strcmp(name, "quit")) {
        out->type = APP_COMMAND_QUIT;
    } else {
        *error = "unknown command";
        return RTC_ERR_INVALID;
    }
    const cJSON *data_only = cJSON_GetObjectItemCaseSensitive(out->json, "data_only");
    if (data_only && !cJSON_IsBool(data_only))
        return RTC_ERR_INVALID;
    out->options.data_only = cJSON_IsTrue(data_only);
    *error = "unknown or duplicate field";
    if (!allowed_fields(out->json, fields))
        return RTC_ERR_INVALID;
    *error = NULL;
    return RTC_OK;
}

void app_command_close(app_command_t *command) {
    cJSON_Delete(command->json);
    memset(command, 0, sizeof(*command));
}

cJSON *app_control_snapshot(const app_session_snapshot_t *s) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "generation", (double)s->generation);
    cJSON_AddNumberToObject(json, "epoch", s->epoch);
    cJSON_AddBoolToObject(json, "active", s->active);
    cJSON_AddBoolToObject(json, "joined", s->joined);
    cJSON_AddBoolToObject(json, "video_muted", s->video_muted);
    cJSON_AddBoolToObject(json, "audio_muted", s->audio_muted);
    cJSON_AddStringToObject(json, "local_id", s->local_id);
    cJSON_AddStringToObject(json, "peer_id", s->local_id);
    cJSON_AddStringToObject(json, "name", s->name);
    cJSON_AddStringToObject(json, "topology", s->topology);
    cJSON_AddStringToObject(json, "mode", s->topology);
    cJSON_AddStringToObject(json, "status", s->status);
    cJSON_AddStringToObject(json, "error", s->error);
    cJSON *peers = cJSON_AddArrayToObject(json, "peers");
    for (size_t i = 0; i < s->peer_count; ++i) {
        const app_session_peer_t *p = &s->peers[i];
        cJSON *peer = cJSON_CreateObject();
        cJSON_AddStringToObject(peer, "id", p->id);
        cJSON_AddStringToObject(peer, "peer_id", p->id);
        cJSON_AddStringToObject(peer, "name", p->name);
        cJSON_AddBoolToObject(peer, "connected", p->connected);
        cJSON_AddNumberToObject(peer, "video_frames", (double)p->video_frames);
        cJSON_AddNumberToObject(peer, "audio_frames", (double)p->audio_frames);
        cJSON_AddNumberToObject(peer, "audio_samples", (double)p->audio_samples);
        cJSON_AddNumberToObject(peer, "epoch", s->epoch);
        cJSON_AddItemToArray(peers, peer);
    }
    cJSON *sources = cJSON_AddArrayToObject(json, "sources");
    for (size_t i = 0; i < s->source_count; ++i) {
        const app_session_source_t *p = &s->sources[i];
        cJSON *source = cJSON_CreateObject();
        cJSON_AddStringToObject(source, "peer_id", p->peer_id);
        cJSON_AddStringToObject(source, "label", p->label);
        cJSON_AddNumberToObject(source, "video_frames", (double)p->video_frames);
        cJSON_AddNumberToObject(source, "audio_frames", (double)p->audio_frames);
        cJSON_AddNumberToObject(source, "audio_samples", (double)p->audio_samples);
        cJSON_AddNumberToObject(source, "generation", (double)s->generation);
        cJSON_AddNumberToObject(source, "epoch", s->epoch);
        cJSON_AddItemToArray(sources, source);
    }
    return json;
}

cJSON *app_control_result(const app_command_t *command, const app_session_snapshot_t *snapshot,
                          int result, const char *error, cJSON *data) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "version", 1);
    cJSON_AddStringToObject(json, "type", "result");
    cJSON_AddStringToObject(json, "event", "result");
    cJSON_AddItemToObject(json, "id",
                          command->id ? cJSON_Duplicate(command->id, true) : cJSON_CreateNull());
    cJSON_AddBoolToObject(json, "ok", result == RTC_OK);
    cJSON_AddNumberToObject(json, "generation", (double)snapshot->generation);
    cJSON_AddNumberToObject(json, "epoch", snapshot->epoch);
    if (result != RTC_OK) {
        cJSON_AddStringToObject(json, "error", error ? error : "command failed");
        cJSON_AddNumberToObject(json, "code", result);
    }
    if (data) {
        if (command->type == APP_COMMAND_STATS && result == RTC_OK)
            cJSON_AddItemToObject(json, "stats", cJSON_Duplicate(data, true));
        cJSON_AddItemToObject(json, "data", data);
    }
    return json;
}
