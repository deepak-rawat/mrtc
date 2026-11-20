#include "control.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <conio.h>
#else
#  include <sys/select.h>
#endif

#define EVENT_QUEUE_SIZE 128
#define EVENT_MAX_SIZE   32768
#define OUTPUT_MAX_SIZE  65536

typedef struct {
    app_session_t *session;
    rtc_mutex_t mutex;
    char *events[EVENT_QUEUE_SIZE];
    size_t head, count;
    bool overflow;
    bool json;
    bool control;
    bool quit;
} cli_t;

static volatile sig_atomic_t interrupted;

static void interrupt_handler(int number) {
    (void)number;
    interrupted = 1;
}

static cJSON *event_new(cli_t *cli, const char *type) {
    app_session_snapshot_t snapshot;
    app_session_get_snapshot(cli->session, &snapshot);
    cJSON *event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "version", 1);
    cJSON_AddStringToObject(event, "event", type);
    cJSON_AddStringToObject(event, "type", type);
    cJSON_AddNumberToObject(event, "generation", (double)snapshot.generation);
    cJSON_AddNumberToObject(event, "epoch", snapshot.epoch);
    return event;
}

static void enqueue(cli_t *cli, cJSON *event) {
    char *line = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    rtc_mutex_lock(&cli->mutex);
    if (!line || strlen(line) > EVENT_MAX_SIZE || cli->count == EVENT_QUEUE_SIZE) {
        cli->overflow = true;
        cJSON_free(line);
    } else {
        cli->events[(cli->head + cli->count) % EVENT_QUEUE_SIZE] = line;
        ++cli->count;
    }
    rtc_mutex_unlock(&cli->mutex);
}

static void on_joined(const char *id, void *user) {
    cli_t *cli = user;
    cJSON *event = event_new(cli, "joined");
    cJSON_AddStringToObject(event, "peer_id", id);
    enqueue(cli, event);
}

static void peer_event(cli_t *cli, const char *type, const char *id) {
    cJSON *event = event_new(cli, type);
    cJSON_AddStringToObject(event, "peer_id", id);
    enqueue(cli, event);
}

static void on_peer_joined(const char *id, void *user) {
    peer_event(user, "peer_joined", id);
}

static void on_peer_left(const char *id, void *user) {
    peer_event(user, "peer_left", id);
}

static void on_message(const char *id, const uint8_t *data, size_t length, void *user) {
    cli_t *cli = user;
    cJSON *event = event_new(cli, "message");
    char text[APP_CONTROL_MAX_TEXT + 1];
    size_t used = length < APP_CONTROL_MAX_TEXT ? length : APP_CONTROL_MAX_TEXT;
    memcpy(text, data, used);
    text[used] = '\0';
    while (used && !app_control_valid_utf8(text, used))
        text[--used] = '\0';
    cJSON_AddStringToObject(event, "peer_id", id);
    cJSON_AddStringToObject(event, "text", text);
    cJSON_AddBoolToObject(event, "truncated", length > used || strlen(text) != used);
    enqueue(cli, event);
}

static void on_error(const char *text, void *user) {
    cli_t *cli = user;
    cJSON *event = event_new(cli, "error");
    cJSON_AddStringToObject(event, "text", text);
    cJSON_AddStringToObject(event, "error", text);
    enqueue(cli, event);
}

static void on_topology(const char *mode, const char *status, uint32_t epoch, const char *reason,
                        void *user) {
    cli_t *cli = user;
    cJSON *event = event_new(cli, "topology");
    cJSON_AddStringToObject(event, "mode", mode);
    cJSON_AddStringToObject(event, "status", status);
    cJSON_AddStringToObject(event, "reason", reason ? reason : "");
    cJSON_ReplaceItemInObjectCaseSensitive(event, "epoch", cJSON_CreateNumber(epoch));
    enqueue(cli, event);
}

static bool output_json(cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    bool ok = text && strlen(text) <= OUTPUT_MAX_SIZE && fputs(text, stdout) >= 0 &&
              fputc('\n', stdout) != EOF && fflush(stdout) == 0;
    cJSON_free(text);
    return ok;
}

static void drain_events(cli_t *cli) {
    for (size_t processed = 0; processed < EVENT_QUEUE_SIZE; ++processed) {
        rtc_mutex_lock(&cli->mutex);
        char *line = NULL;
        if (cli->count) {
            line = cli->events[cli->head];
            cli->head = (cli->head + 1) % EVENT_QUEUE_SIZE;
            --cli->count;
        }
        bool overflow = cli->overflow;
        cli->overflow = false;
        rtc_mutex_unlock(&cli->mutex);
        if (overflow) {
            fprintf(stderr, "Event queue overflow; shutting down\n");
            if (cli->json) {
                cJSON *event = event_new(cli, "error");
                cJSON_AddStringToObject(event, "text", "event queue overflow");
                output_json(event);
            }
            cli->quit = true;
        }
        if (!line)
            break;
        if (cli->json) {
            if (fputs(line, stdout) < 0 || fputc('\n', stdout) == EOF || fflush(stdout))
                cli->quit = true;
        } else {
            /* Human output shares the same single writer as protocol output. */
            cJSON *event = cJSON_Parse(line);
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(event, "event");
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(event, "peer_id");
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(event, "text");
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(event, "mode");
            const cJSON *status = cJSON_GetObjectItemCaseSensitive(event, "status");
            fprintf(!strcmp(type->valuestring, "error") ? stderr : stdout, "[%s%s%s%s%s%s%s]\n",
                    type->valuestring, id ? " " : "", id ? id->valuestring : "",
                    text   ? ": "
                    : mode ? " "
                           : "",
                    text   ? text->valuestring
                    : mode ? mode->valuestring
                           : "",
                    status ? " " : "", status ? status->valuestring : "");
            fflush(stdout);
            cJSON_Delete(event);
        }
        cJSON_free(line);
    }
}

static void control_line(cli_t *cli, const char *line, bool oversized) {
    app_command_t command = {0};
    const char *error = "line exceeds 8192 bytes";
    int rc = oversized ? RTC_ERR_INVALID : app_command_parse(&command, line, &error);
    cJSON *data = NULL;
    if (rc == RTC_OK) {
        switch (command.type) {
            case APP_COMMAND_JOIN:
                rc = app_session_start(cli->session, &command.options);
                if (rc == RTC_OK) {
                    data = cJSON_CreateObject();
                    cJSON_AddBoolToObject(data, "accepted", true);
                }
                break;
            case APP_COMMAND_LEAVE:
                app_session_stop(cli->session);
                break;
            case APP_COMMAND_SEND_MESSAGE:
                rc = app_session_send_message(cli->session, command.peer_id,
                                              (const uint8_t *)command.text, strlen(command.text));
                if (rc == RTC_ERR_INVALID)
                    error = "peer not ready or not open";
                else if (rc == RTC_ERR_GENERIC && !command.peer_id)
                    error = "broadcast incomplete: peer not ready or send failed";
                break;
            case APP_COMMAND_MUTE:
                rc = app_session_mute(cli->session, command.source, command.muted);
                break;
            case APP_COMMAND_STATS:
                break;
            case APP_COMMAND_QUIT:
                cli->quit = true;
                break;
        }
    }
    app_session_snapshot_t snapshot;
    app_session_get_snapshot(cli->session, &snapshot);
    if (rc == RTC_OK && command.type == APP_COMMAND_STATS)
        data = app_control_snapshot(&snapshot);
    if (!output_json(app_control_result(&command, &snapshot, rc, error, data)))
        cli->quit = true;
    app_command_close(&command);
}

static void interactive_line(cli_t *cli, const char *line, bool oversized) {
    if (oversized) {
        fprintf(stderr, "Input line too long\n");
        return;
    }
    if (!strcmp(line, "quit") || !strcmp(line, "/quit")) {
        cli->quit = true;
        return;
    }
    if (!strcmp(line, "status") || !strcmp(line, "/status")) {
        app_session_snapshot_t snapshot;
        app_session_get_snapshot(cli->session, &snapshot);
        if (cli->json) {
            cJSON *event = event_new(cli, "stats");
            cJSON_AddItemToObject(event, "stats", app_control_snapshot(&snapshot));
            output_json(event);
        } else {
            printf("[%s %s local=%s peers=%zu generation=%llu epoch=%u]\n", snapshot.topology,
                   snapshot.status, snapshot.local_id, snapshot.peer_count,
                   (unsigned long long)snapshot.generation, snapshot.epoch);
        }
        return;
    }
    if (!strncmp(line, "mute ", 5) || !strncmp(line, "unmute ", 7)) {
        bool muted = line[0] == 'm';
        int rc = app_session_mute(cli->session, line + (muted ? 5 : 7), muted);
        if (rc != RTC_OK)
            fprintf(stderr, "Use mute/unmute video|audio (source must be enabled)\n");
        return;
    }
    if (line[0]) {
        int rc = app_session_send_message(cli->session, NULL, (const uint8_t *)line, strlen(line));
        if (rc != RTC_OK)
            fprintf(stderr, "Message not sent: %d\n", rc);
    }
}

/* Return bytes read, zero while idle, or -1 on EOF. Console polling is never
 * used on a pipe: Windows pipes need PeekNamedPipe to observe closure. */
static int input_read(char *buffer, size_t capacity) {
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD type = GetFileType(input), available = 0, count = 0, console_mode = 0;
    if (type == FILE_TYPE_CHAR && GetConsoleMode(input, &console_mode)) {
        size_t used = 0;
        while (used < capacity && _kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 224) {
                (void)_getch();
                continue;
            }
            if (ch == 26)
                return used ? (int)used : -1;
            if (ch == '\r')
                ch = '\n';
            buffer[used++] = (char)ch;
            fputc(ch, stderr);
        }
        return (int)used;
    }
    if (type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL))
            return -1;
        if (!available)
            return 0;
        if (capacity > available)
            capacity = available;
    }
    if (!ReadFile(input, buffer, (DWORD)capacity, &count, NULL) || !count)
        return -1;
    return (int)count;
#else
    fd_set readers;
    FD_ZERO(&readers);
    FD_SET(STDIN_FILENO, &readers);
    struct timeval timeout = {0, 0};
    int ready = select(STDIN_FILENO + 1, &readers, NULL, NULL, &timeout);
    if (ready <= 0)
        return ready < 0 && errno != EINTR ? -1 : 0;
    ssize_t count = read(STDIN_FILENO, buffer, capacity);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
        return 0;
    return count > 0 ? (int)count : -1;
#endif
}

static void sleep_tick(void) {
#ifdef _WIN32
    Sleep(5);
#else
    struct timeval timeout = {0, 5000};
    select(0, NULL, NULL, NULL, &timeout);
#endif
}

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --room ROOM [--server URL] [--name NAME] [--stun HOST]\n"
            "  [--video synthetic|none] [--audio synthetic|none] [--data-only]\n"
            "  [--duration SECONDS]\n"
            "  [--no-interactive] [--events jsonl]\n"
            "  --meeting ROOM is an alias for --room ROOM.\n"
            "  %s --control stdio   (version 1 JSONL commands on stdin)\n"
            "Interactive commands: status, mute/unmute video|audio, quit; other lines chat.\n",
            program, program);
}

int main(int argc, char **argv) {
    app_session_options_t options = {
        .server = APP_SESSION_DEFAULT_SERVER,
        .name = "native",
        .stun = APP_SESSION_DEFAULT_STUN,
        .video = APP_MEDIA_SYNTHETIC,
        .audio = APP_MEDIA_SYNTHETIC,
    };
    cli_t cli = {0};
    bool interactive = true;
    uint64_t duration = 0;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(arg, "--no-interactive")) {
            interactive = false;
            continue;
        }
        if (!strcmp(arg, "--data-only")) {
            options.data_only = true;
            options.video = options.audio = APP_MEDIA_NONE;
            continue;
        }
        if (i + 1 == argc) {
            usage(argv[0]);
            return 1;
        }
        const char *value = argv[++i];
        if (!strcmp(arg, "--server"))
            options.server = value;
        else if (!strcmp(arg, "--meeting") || !strcmp(arg, "--room"))
            options.room = value;
        else if (!strcmp(arg, "--name"))
            options.name = value;
        else if (!strcmp(arg, "--stun"))
            options.stun = value;
        else if (!strcmp(arg, "--video") || !strcmp(arg, "--audio")) {
            if (strcmp(value, "none") && strcmp(value, "synthetic")) {
                usage(argv[0]);
                return 1;
            }
            app_media_mode_t mode = !strcmp(value, "none") ? APP_MEDIA_NONE : APP_MEDIA_SYNTHETIC;
            if (!strcmp(arg, "--video"))
                options.video = mode;
            else
                options.audio = mode;
        } else if (!strcmp(arg, "--control") && !strcmp(value, "stdio")) {
            cli.control = cli.json = true;
        } else if (!strcmp(arg, "--events") && !strcmp(value, "jsonl")) {
            cli.json = true;
        } else if (!strcmp(arg, "--duration")) {
            char *end = NULL;
            errno = 0;
            unsigned long seconds = strtoul(value, &end, 10);
            if (errno || !value[0] || value[0] == '-' || *end || !seconds || seconds > 86400) {
                usage(argv[0]);
                return 1;
            }
            duration = (uint64_t)seconds * 1000;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if ((!cli.control || options.room) && app_session_validate_options(&options) != RTC_OK) {
        usage(argv[0]);
        return 1;
    }
    if (rtc_mutex_init(&cli.mutex) != RTC_OK)
        return 1;
    app_session_config_t config = {
        .callbacks = {.on_joined = on_joined,
                      .on_peer_joined = on_peer_joined,
                      .on_peer_left = on_peer_left,
                      .on_message = on_message,
                      .on_error = on_error,
                      .on_topology = on_topology},
        .user_data = &cli,
    };
    cli.session = app_session_create(&config);
    if (!cli.session) {
        rtc_mutex_destroy(&cli.mutex);
        return 1;
    }
    signal(SIGINT, interrupt_handler);
    signal(SIGTERM, interrupt_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    int exit_code = 0;
    if (options.room && app_session_start(cli.session, &options) != RTC_OK) {
        fprintf(stderr, "Could not start native session\n");
        exit_code = 1;
        cli.quit = true;
    }
    if (!cli.json && !cli.quit)
        fprintf(stderr, "Native session ready; status, mute/unmute video|audio, quit, or chat.\n");
    uint64_t started = rtc_time_ms();
    char line[APP_CONTROL_MAX_LINE + 1];
    size_t used = 0;
    bool oversized = false;
    while (!cli.quit && !interrupted) {
        uint64_t now = rtc_time_ms();
        if (duration && now - started >= duration)
            break;
        app_session_tick(cli.session, now);
        drain_events(&cli);
        if (interactive || cli.control) {
            char input[512];
            int count = input_read(input, sizeof(input));
            for (int i = 0; i < count && !cli.quit; ++i) {
                if (input[i] == '\n') {
                    if (used && line[used - 1] == '\r')
                        --used;
                    line[used] = '\0';
                    if (cli.control)
                        control_line(&cli, line, oversized);
                    else
                        interactive_line(&cli, line, oversized);
                    used = 0;
                    oversized = false;
                    drain_events(&cli);
                } else if (used < APP_CONTROL_MAX_LINE && !oversized) {
                    if (!cli.control && (input[i] == '\b' || input[i] == 127)) {
                        if (used)
                            --used;
                    } else {
                        line[used++] = input[i] ? input[i] : '\1';
                    }
                } else {
                    oversized = true;
                }
            }
            if (count < 0) {
                if (used || oversized) {
                    line[used] = '\0';
                    if (cli.control)
                        control_line(&cli, line, oversized);
                    else
                        interactive_line(&cli, line, oversized);
                }
                break;
            }
        }
        sleep_tick();
    }
    app_session_stop(cli.session);
    drain_events(&cli);
    app_session_destroy(cli.session);
    rtc_mutex_destroy(&cli.mutex);
    return exit_code;
}
