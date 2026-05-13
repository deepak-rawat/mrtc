/*
 * turn_server.c - Minimal TURN server for development/testing.
 *
 * Usage: turn_server [public_ip] [port] [username] [password]
 * Defaults: 127.0.0.1 3478 user pass
 */
#include "turn_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
}

int main(int argc, char *argv[]) {
    const char *public_ip = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 3478;
    const char *username = argc > 3 ? argv[3] : "user";
    const char *password = argc > 4 ? argv[4] : "pass";
    const char *realm = "mrtc";

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    turn_server_t ts;
    if (turn_server_init(&ts, public_ip, port, username, password, realm) != RTC_OK) {
        fprintf(stderr, "Failed to initialize TURN server\n");
        return 1;
    }

    fprintf(stderr, "TURN server running on %s:%d (user=%s)\n", public_ip, port, username);

    uint8_t buf[2048];
    uint64_t last_expire = rtc_time_ms();

    while (!interrupted) {
        fd_set fds;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000}; /* 100ms */

        int max_fd = (int)ts.listen_sock;

        FD_ZERO(&fds);
        FD_SET(ts.listen_sock, &fds);

        /* Also poll relay sockets */
        for (int i = 0; i < TURN_MAX_ALLOCATIONS; i++) {
            if (ts.allocs[i].active) {
                FD_SET(ts.allocs[i].relay_sock, &fds);
                if ((int)ts.allocs[i].relay_sock > max_fd)
                    max_fd = (int)ts.allocs[i].relay_sock;
            }
        }

        int sel = select(max_fd + 1, &fds, NULL, NULL, &tv);
        if (sel < 0)
            break;

        /* Check listen socket */
        if (sel > 0 && FD_ISSET(ts.listen_sock, &fds)) {
            rtc_addr_t from;
            from.len = sizeof(from.addr);
            ssize_t n = recvfrom(ts.listen_sock, (char *)buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from.addr, &from.len);
            if (n > 0)
                turn_server_handle_packet(&ts, buf, (size_t)n, &from);
        }

        /* Check relay sockets */
        for (int i = 0; i < TURN_MAX_ALLOCATIONS; i++) {
            if (ts.allocs[i].active && FD_ISSET(ts.allocs[i].relay_sock, &fds)) {
                rtc_addr_t peer_from;
                peer_from.len = sizeof(peer_from.addr);
                ssize_t n = recvfrom(ts.allocs[i].relay_sock, (char *)buf, sizeof(buf), 0,
                                     (struct sockaddr *)&peer_from.addr, &peer_from.len);
                if (n > 0)
                    turn_server_relay_from_peer(&ts, &ts.allocs[i], buf, (size_t)n, &peer_from);
            }
        }

        /* Periodic expiry check */
        if (rtc_time_ms() - last_expire > 10000) {
            turn_server_expire(&ts);
            last_expire = rtc_time_ms();
        }
    }

    turn_server_close(&ts);
    fprintf(stderr, "TURN server stopped\n");

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
