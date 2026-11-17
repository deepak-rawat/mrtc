#ifndef APP_CLI_CONTROL_H
#define APP_CLI_CONTROL_H

#include "app_session.h"
#include <cjson/cJSON.h>

#define APP_CONTROL_MAX_LINE 8192
#define APP_CONTROL_MAX_TEXT 4096

typedef enum {
    APP_COMMAND_JOIN,
    APP_COMMAND_LEAVE,
    APP_COMMAND_SEND_MESSAGE,
    APP_COMMAND_MUTE,
    APP_COMMAND_STATS,
    APP_COMMAND_QUIT
} app_command_type_t;

typedef struct {
    cJSON *json;
    const cJSON *id;
    app_command_type_t type;
    app_session_options_t options;
    const char *text;
    const char *peer_id;
    const char *source;
    bool muted;
} app_command_t;

/* On failure json/id may still be set for a correlated error response.
 * Always call app_command_close, including after a parse error. */
int app_command_parse(app_command_t *out, const char *line, const char **error);
bool app_control_valid_utf8(const char *text, size_t length);
void app_command_close(app_command_t *command);
cJSON *app_control_snapshot(const app_session_snapshot_t *snapshot);
cJSON *app_control_result(const app_command_t *command, const app_session_snapshot_t *snapshot,
                          int result, const char *error, cJSON *data);

#endif
