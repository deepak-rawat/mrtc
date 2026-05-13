/*
 * signaling_server.c - WebSocket signaling server using libwebsockets.
 *
 * Usage: signaling_server [port]   (default: 8080)
 */
#include "meeting.h"
#include "signaling/signaling_msg.h"
#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static volatile int interrupted = 0;
static meeting_manager_t g_mgr;

/* Per-connection data stored by libwebsockets */
struct per_session_data {
    meeting_peer_t *peer;
    /* Outgoing message queue (simple ring buffer) */
    char *pending_msgs[64];
    int msg_head;
    int msg_tail;
    int msg_count;
};

/* Queue a message for sending (will be sent on LWS_CALLBACK_SERVER_WRITEABLE) */
static void queue_message(struct per_session_data *pss, const char *json) {
    if (pss->msg_count >= 64)
        return; /* drop if full */
    pss->pending_msgs[pss->msg_tail] = strdup(json);
    pss->msg_tail = (pss->msg_tail + 1) % 64;
    pss->msg_count++;
}

/* Send callback for meeting_manager */
static void ws_send_fn(meeting_peer_t *peer, const char *json, void *user) {
    (void)user;
    if (!peer || !peer->ws_handle)
        return;

    struct lws *wsi = (struct lws *)peer->ws_handle;
    struct per_session_data *pss = (struct per_session_data *)lws_wsi_user(wsi);
    if (!pss)
        return;

    queue_message(pss, json);
    lws_callback_on_writable(wsi);
}

/* Handle incoming message from a client */
static void handle_message(struct lws *wsi, struct per_session_data *pss, const char *data,
                           size_t len) {
    sig_msg_t msg;
    if (sig_msg_parse(&msg, data, len) != 0) {
        char *err = sig_msg_build_error("invalid message");
        if (err) {
            queue_message(pss, err);
            free(err);
            lws_callback_on_writable(wsi);
        }
        return;
    }

    switch (msg.type) {
        case SIG_MSG_JOIN:
            meeting_join(&g_mgr, pss->peer, msg.meeting);
            break;

        case SIG_MSG_LEAVE:
            meeting_leave(&g_mgr, pss->peer);
            break;

        case SIG_MSG_OFFER: {
            char *relay = sig_msg_build_relay_offer(pss->peer->id, msg.sdp);
            if (relay) {
                meeting_route_message(&g_mgr, pss->peer, msg.to, relay);
                free(relay);
            }
            break;
        }

        case SIG_MSG_ANSWER: {
            char *relay = sig_msg_build_relay_answer(pss->peer->id, msg.sdp);
            if (relay) {
                meeting_route_message(&g_mgr, pss->peer, msg.to, relay);
                free(relay);
            }
            break;
        }

        case SIG_MSG_CANDIDATE: {
            char *relay = sig_msg_build_relay_candidate(pss->peer->id, msg.candidate);
            if (relay) {
                meeting_route_message(&g_mgr, pss->peer, msg.to, relay);
                free(relay);
            }
            break;
        }

        default: {
            char *err = sig_msg_build_error("unsupported message type");
            if (err) {
                queue_message(pss, err);
                free(err);
                lws_callback_on_writable(wsi);
            }
            break;
        }
    }
}

/* libwebsockets protocol callback */
static int callback_signaling(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                              void *in, size_t len) {
    struct per_session_data *pss = (struct per_session_data *)user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            memset(pss, 0, sizeof(*pss));
            pss->peer = meeting_peer_create((void *)wsi);
            if (!pss->peer)
                return -1;
            fprintf(stderr, "[signaling] peer connected: %s\n", pss->peer->id);
            break;

        case LWS_CALLBACK_RECEIVE:
            if (pss->peer)
                handle_message(wsi, pss, (const char *)in, len);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            while (pss->msg_count > 0) {
                char *json = pss->pending_msgs[pss->msg_head];
                if (json) {
                    size_t json_len = strlen(json);
                    /* LWS requires LWS_PRE bytes before the payload */
                    unsigned char *buf = malloc(LWS_PRE + json_len);
                    if (buf) {
                        memcpy(buf + LWS_PRE, json, json_len);
                        lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
                        free(buf);
                    }
                    free(json);
                    pss->pending_msgs[pss->msg_head] = NULL;
                }
                pss->msg_head = (pss->msg_head + 1) % 64;
                pss->msg_count--;

                if (pss->msg_count > 0)
                    lws_callback_on_writable(wsi);
                break; /* One write per callback for flow control */
            }
            break;

        case LWS_CALLBACK_CLOSED:
            if (pss->peer) {
                fprintf(stderr, "[signaling] peer disconnected: %s\n", pss->peer->id);
                meeting_peer_destroy(&g_mgr, pss->peer);
                pss->peer = NULL;
            }
            /* Free any pending messages */
            while (pss->msg_count > 0) {
                free(pss->pending_msgs[pss->msg_head]);
                pss->msg_head = (pss->msg_head + 1) % 64;
                pss->msg_count--;
            }
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {{
                                                     .name = "signaling",
                                                     .callback = callback_signaling,
                                                     .per_session_data_size =
                                                         sizeof(struct per_session_data),
                                                     .rx_buffer_size = 8192,
                                                 },
                                                 LWS_PROTOCOL_LIST_TERM};

static void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
}

int main(int argc, char *argv[]) {
    int port = 8080;
    if (argc > 1)
        port = atoi(argv[1]);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    meeting_manager_init(&g_mgr, ws_send_fn, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

    struct lws_context *ctx = lws_create_context(&info);
    if (!ctx) {
        fprintf(stderr, "Failed to create lws context\n");
        return 1;
    }

    fprintf(stderr, "Signaling server listening on port %d\n", port);

    while (!interrupted) {
        lws_service(ctx, 100);
    }

    lws_context_destroy(ctx);
    meeting_manager_close(&g_mgr);
    fprintf(stderr, "Signaling server stopped\n");
    return 0;
}
