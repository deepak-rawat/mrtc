/*
 * WebSocket signaling server using libwebsockets.
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
#include <stdatomic.h>
#include <errno.h>
#ifdef MRTC_SIGNALING_SFU
#  include "sfu.h"
#  include <rtc/rtc_client.h>
static signaling_sfu_t *g_sfu;
#endif

static volatile sig_atomic_t interrupted = 0;
static meeting_manager_t g_mgr;
static struct lws_context *g_context;
static _Atomic bool g_pending;

/* Per-connection data stored by libwebsockets */
struct per_session_data {
    meeting_peer_t *peer;
    /* Outgoing message queue (simple ring buffer) */
    char *pending_msgs[64];
    int msg_head;
    int msg_tail;
    int msg_count;
    rtc_mutex_t mutex;
    bool overflow;
    char incoming[2 * SIG_MAX_SDP + 1024];
    size_t incoming_len;
};

/* Queue a message for sending (will be sent on LWS_CALLBACK_SERVER_WRITEABLE) */
static void queue_message(struct per_session_data *pss, const char *json) {
    rtc_mutex_lock(&pss->mutex);
    char *copy = pss->msg_count < 64 ? strdup(json) : NULL;
    if (!copy) {
        pss->overflow = true;
    } else {
        pss->pending_msgs[pss->msg_tail] = copy;
        pss->msg_tail = (pss->msg_tail + 1) % 64;
        pss->msg_count++;
    }
    rtc_mutex_unlock(&pss->mutex);
    atomic_store(&g_pending, true);
    lws_cancel_service(g_context);
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
    lws_cancel_service(g_context);
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

#ifdef MRTC_SIGNALING_SFU
    if (g_sfu) {
        if (msg.type == SIG_MSG_LEAVE || msg.type == SIG_MSG_JOIN)
            signaling_sfu_leave(g_sfu, pss->peer);
        if (msg.type == SIG_MSG_OFFER || msg.type == SIG_MSG_ANSWER ||
            msg.type == SIG_MSG_CANDIDATE) {
            int rc = signaling_sfu_handle(g_sfu, pss->peer, &msg);
            if (rc != RTC_OK) {
                const char *type = msg.type == SIG_MSG_OFFER    ? "offer"
                                   : msg.type == SIG_MSG_ANSWER ? "answer"
                                                                : "candidate";
                RTC_LOG_ERR("SFU negotiation rejected peer=%s type=%s to=%s rc=%d candidate=%s",
                            pss->peer->id, type, msg.to, rc,
                            msg.type == SIG_MSG_CANDIDATE ? msg.candidate : "");
                char *error = sig_msg_build_error("SFU negotiation rejected");
                if (error) {
                    queue_message(pss, error);
                    free(error);
                }
            }
            lws_callback_on_writable(wsi);
            return;
        }
    }
#endif
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
            if (rtc_mutex_init(&pss->mutex) != RTC_OK)
                return -1;
            pss->peer = meeting_peer_create((void *)wsi);
            if (!pss->peer)
                return -1;
            fprintf(stderr, "[signaling] peer connected: %s\n", pss->peer->id);
            break;

        case LWS_CALLBACK_RECEIVE:
            if (len > sizeof(pss->incoming) - pss->incoming_len)
                return -1;
            memcpy(pss->incoming + pss->incoming_len, in, len);
            pss->incoming_len += len;
            if (pss->peer && lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                handle_message(wsi, pss, pss->incoming, pss->incoming_len);
                pss->incoming_len = 0;
            }
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            rtc_mutex_lock(&pss->mutex);
            if (pss->overflow) {
                rtc_mutex_unlock(&pss->mutex);
                return -1;
            }
            while (pss->msg_count > 0) {
                char *json = pss->pending_msgs[pss->msg_head];
                if (json) {
                    size_t json_len = strlen(json);
                    /* LWS requires LWS_PRE bytes before the payload */
                    unsigned char *buf = malloc(LWS_PRE + json_len);
                    if (buf) {
                        memcpy(buf + LWS_PRE, json, json_len);
                        int written = lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
                        free(buf);
                        if (written != (int)json_len)
                            pss->overflow = true;
                    } else {
                        pss->overflow = true;
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
            rtc_mutex_unlock(&pss->mutex);
            break;

        case LWS_CALLBACK_CLOSED:
            if (pss->peer) {
#ifdef MRTC_SIGNALING_SFU
                signaling_sfu_leave(g_sfu, pss->peer);
#endif
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
            rtc_mutex_destroy(&pss->mutex);
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

static bool parse_port(const char *text, bool allow_zero, uint16_t *out) {
    if (!text || text[0] < '0' || text[0] > '9')
        return false;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno == ERANGE || !end || *end || value > 65535 || value < (allow_zero ? 0 : 1))
        return false;
    *out = (uint16_t)value;
    return true;
}

int main(int argc, char *argv[]) {
    uint16_t port = 8080;
    int first_option = 1;
    if (argc > 1 && argv[1][0] != '-') {
        if (!parse_port(argv[1], false, &port)) {
            fprintf(stderr, "Invalid signaling port: %s\n", argv[1]);
            return 1;
        }
        first_option = 2;
    }
    bool mesh = false;
    const char *listen_ip = "127.0.0.1";
    const char *announced_ip = NULL;
    uint16_t media_port = 0;
    for (int i = first_option; i < argc; i++) {
        if (strcmp(argv[i], "--mesh") == 0)
            mesh = true;
        else if (strcmp(argv[i], "--listen-ip") == 0 && i + 1 < argc && argv[i + 1][0] &&
                 argv[i + 1][0] != '-')
            listen_ip = argv[++i];
        else if (strcmp(argv[i], "--announced-ip") == 0 && i + 1 < argc && argv[i + 1][0] &&
                 argv[i + 1][0] != '-')
            announced_ip = argv[++i];
        else if (strcmp(argv[i], "--media-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], true, &media_port)) {
                fprintf(stderr, "Invalid media port: %s\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    meeting_manager_init(&g_mgr, ws_send_fn, NULL);
#ifdef MRTC_SIGNALING_SFU
    if (rtc_client_init() != RTC_OK)
        return 1;
    if (!mesh) {
        g_sfu = signaling_sfu_create(&g_mgr, listen_ip, announced_ip, media_port);
        if (!g_sfu) {
            fprintf(stderr, "SFU initialization failed\n");
            rtc_client_cleanup();
            return 1;
        }
        g_mgr.sfu_mode = true;
    }
#else
    (void)listen_ip;
    (void)announced_ip;
    (void)media_port;
    if (!mesh) {
        fprintf(stderr, "SFU requires client and routing APIs; use --mesh explicitly\n");
        return 1;
    }
#endif

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.iface = listen_ip;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

    struct lws_context *ctx = lws_create_context(&info);
    if (!ctx) {
        fprintf(stderr, "Failed to create lws context\n");
        return 1;
    }
    g_context = ctx;

    fprintf(stderr, "Signaling server listening on port %d\n", port);

    while (!interrupted) {
        if (atomic_exchange(&g_pending, false))
            lws_callback_on_writable_all_protocol(ctx, &protocols[0]);
        lws_service(ctx, 100);
    }

    lws_context_destroy(ctx);
#ifdef MRTC_SIGNALING_SFU
    signaling_sfu_destroy(g_sfu);
    rtc_client_cleanup();
#endif
    meeting_manager_close(&g_mgr);
    fprintf(stderr, "Signaling server stopped\n");
    return 0;
}
