#include <conference/conference.h>
#include <rtc/rtc_client.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <conio.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <sys/select.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

static void on_joined(const char *id, void *user) {
    (void)user;
    printf("  [joined as %s]\n", id);
    fflush(stdout);
}

static void on_peer_joined(const char *id, void *user) {
    (void)user;
    printf("  [%s joined]\n", id);
    fflush(stdout);
}

static void on_peer_left(const char *id, void *user) {
    (void)user;
    printf("  [%s left]\n", id);
    fflush(stdout);
}

static void on_message(const char *id, const uint8_t *data, size_t len, void *user) {
    (void)user;
    printf("  [%s] %.*s\n", id, (int)len, (const char *)data);
    fflush(stdout);
}

static void on_error(const char *message, void *user) {
    (void)user;
    fprintf(stderr, "  [error] %s\n", message);
}

static bool stdin_has_data(void) {
#ifdef _WIN32
    return _kbhit() != 0;
#else
    fd_set fds;
    struct timeval timeout = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) > 0;
#endif
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s --meeting <name> [--server <url>] [--stun <ip>]\n", program);
}

int main(int argc, char **argv) {
    const char *meeting = NULL;
    const char *server = "ws://127.0.0.1:8080";
    const char *stun = "stun.l.google.com";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--meeting") == 0 && i + 1 < argc)
            meeting = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server = argv[++i];
        else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc)
            stun = argv[++i];
        else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!meeting || !meeting[0]) {
        usage(argv[0]);
        return 1;
    }
    /* Keep descriptor readiness consistent with input buffered by stdio. */
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        fprintf(stderr, "Could not configure chat input\n");
        return 1;
    }
    if (rtc_client_init() != RTC_OK) {
        fprintf(stderr, "Could not initialize RTC\n");
        return 1;
    }
    conference_config_t config = {
        .stun_server = stun[0] ? stun : NULL,
        .callbacks = {.on_joined = on_joined,
                      .on_peer_joined = on_peer_joined,
                      .on_peer_left = on_peer_left,
                      .on_message = on_message,
                      .on_error = on_error},
    };
    conference_t *conference = conference_create(&config);
    if (!conference) {
        fprintf(stderr, "Could not create conference\n");
        rtc_client_cleanup();
        return 1;
    }
    signaling_config_t signaling_config;
    conference_get_signaling_callbacks(conference, &signaling_config, server, meeting);
    signaling_client_t *signaling = signaling_create(&signaling_config);
    if (!signaling) {
        fprintf(stderr, "Could not create signaling client\n");
        conference_destroy(conference);
        rtc_client_cleanup();
        return 1;
    }
    if (conference_join(conference, signaling) != RTC_OK ||
        signaling_connect(signaling) != RTC_OK) {
        fprintf(stderr, "Could not connect to %s\n", server);
        conference_leave(conference);
        signaling_destroy(signaling);
        conference_destroy(conference);
        rtc_client_cleanup();
        return 1;
    }

    puts("  Chat ready! Type messages and press Enter; type 'quit' to leave.");
    fflush(stdout);
    char input[1024];
    while (true) {
        if (stdin_has_data()) {
            if (!fgets(input, sizeof(input), stdin))
                break;
            size_t len = strcspn(input, "\r\n");
            input[len] = '\0';
            if (strcmp(input, "quit") == 0)
                break;
            if (len) {
                int rc = conference_broadcast_message(conference, (const uint8_t *)input, len);
                if (rc != RTC_OK)
                    fprintf(stderr, "Message not sent: %d\n", rc);
                else {
                    printf("  [me] %s\n", input);
                    fflush(stdout);
                }
            }
        }
        SLEEP_MS(10);
    }

    conference_leave(conference);
    signaling_destroy(signaling);
    conference_destroy(conference);
    rtc_client_cleanup();
    puts("  Bye!");
    return 0;
}
