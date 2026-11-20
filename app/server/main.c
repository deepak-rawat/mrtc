/*
 * WebSocket signaling server using libwebsockets.
 *
 * Usage: mrtc_server [port]   (default: 8080)
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
#include <limits.h>
#include <sys/stat.h>
#include <rtc/rtc.h>
#ifdef MRTC_SIGNALING_SFU
#  include <rtc/rtc_sfu.h>
#  include <rtc/rtc_sctp.h>
static rtc_sfu_t *g_sfu;

typedef struct media_event {
    struct media_event *next;
    rtc_sfu_event_t event;
} media_event_t;
static rtc_mutex_t g_events_mutex;
static media_event_t *g_events_head, *g_events_tail;
static unsigned g_events_count;
static bool g_events_overflow;

static int subscription_id(char out[RTC_SFU_SESSION_SIZE], uint32_t epoch, const char *subscriber,
                           const char *publisher) {
    return sig_session_id(out, SIG_SESSION_SUBSCRIBE, epoch, subscriber, publisher);
}
#endif

static volatile sig_atomic_t interrupted = 0;
static meeting_manager_t g_mgr;
static struct lws_context *g_context;
static _Atomic bool g_pending;

#ifdef MRTC_SIGNALING_SFU
static void on_media_event(const rtc_sfu_event_t *event, void *user) {
    (void)user;
    rtc_mutex_lock(&g_events_mutex);
    media_event_t *copy = g_events_count < 256 ? malloc(sizeof(*copy)) : NULL;
    if (!copy) {
        g_events_overflow = true;
    } else {
        copy->next = NULL;
        copy->event = *event;
        if (g_events_tail)
            g_events_tail->next = copy;
        else
            g_events_head = copy;
        g_events_tail = copy;
        g_events_count++;
    }
    rtc_mutex_unlock(&g_events_mutex);
    if (g_context)
        lws_cancel_service(g_context);
}

static void retire_media(const char *room, const char *peer, uint32_t epoch, void *user) {
    (void)user;
    rtc_sfu_leave(g_sfu, room, peer, epoch);
}

static void route_data(const rtc_sfu_event_t *event, meeting_t *room) {
    char destination[SIG_MAX_PEER_ID];
    uint64_t message_id;
    const uint8_t *payload;
    size_t payload_len;
    int parsed =
        sig_dc_read(event->data, event->data_len, destination, &message_id, &payload, &payload_len);
    if (parsed != 0 && event->version == 1) {
        if (event->data_len >= 4 &&
            (!memcmp(event->data, "MRTC", 4) || !memcmp(event->data, "MRC2", 4)))
            return;
        for (int i = 0; i < room->peer_count; i++)
            rtc_sfu_send_data(g_sfu, room->name, room->peers[i]->id, event->epoch, event->data,
                              event->data_len);
        return;
    }
    if (parsed != 0 || (event->version >= 2 && !message_id))
        return;
    uint8_t wire[65535];
    size_t wire_len;
    if (message_id) {
        if (sig_dc_write(wire, sizeof(wire), &wire_len, event->owner, message_id, payload,
                         payload_len) != 0)
            return;
    } else {
        if (event->data_len > sizeof(wire))
            return;
        wire_len = event->data_len;
        memcpy(wire, event->data, wire_len);
        memset(wire + 4, 0, SIG_MAX_PEER_ID);
        memcpy(wire + 4, event->owner, strlen(event->owner));
    }
    for (int i = 0; i < room->peer_count; i++) {
        const char *id = room->peers[i]->id;
        if (strcmp(id, event->owner) && (!strcmp(destination, "*") || !strcmp(destination, id))) {
            int rc = rtc_sfu_send_data(g_sfu, room->name, id, event->epoch, wire, wire_len);
            if (rc != RTC_OK)
                RTC_LOG_DBG("SFU data recipient not ready: %s (%d)", id, rc);
        }
    }
}

static void drain_media_events(void) {
    for (;;) {
        rtc_mutex_lock(&g_events_mutex);
        media_event_t *item = g_events_head;
        if (item) {
            g_events_head = item->next;
            if (!g_events_head)
                g_events_tail = NULL;
            g_events_count--;
        }
        bool overflow = g_events_overflow;
        rtc_mutex_unlock(&g_events_mutex);
        if (overflow) {
            RTC_LOG_ERR("SFU control event queue overflow");
            interrupted = 1;
        }
        if (!item)
            break;
        rtc_sfu_event_t *event = &item->event;
        meeting_peer_t *peer = meeting_find_peer(&g_mgr, event->room, event->owner);
        if (peer) {
            meeting_t *room = &g_mgr.meetings[peer->meeting_idx];
            bool valid =
                event->epoch == room->epoch ||
                (room->phase == MEETING_PREPARING && event->epoch == room->pending_epoch) ||
                (room->phase == MEETING_COMMITTING && event->epoch == room->previous_epoch);
            if (valid && event->type == RTC_SFU_DATA)
                route_data(event, room);
            else if (valid) {
                if (event->type == RTC_SFU_ERROR && room->phase == MEETING_PREPARING &&
                    event->epoch == room->pending_epoch) {
                    sig_msg_t failure = {
                        .type = SIG_MSG_TOPOLOGY_ABORT, .version = 2, .epoch = event->epoch};
                    snprintf(failure.mode, sizeof(failure.mode), "%s",
                             room->desired_mode == MEETING_MODE_SFU ? "sfu" : "mesh");
                    snprintf(failure.reason, sizeof(failure.reason), "SFU subscription failed");
                    meeting_handle_topology(&g_mgr, peer, &failure, rtc_time_ms());
                }
                sig_msg_t message = {.version = peer->version,
                                     .epoch = peer->version >= 2 ? event->epoch : 0};
                message.type = event->type == RTC_SFU_OFFER       ? SIG_MSG_OFFER
                               : event->type == RTC_SFU_ANSWER    ? SIG_MSG_ANSWER
                               : event->type == RTC_SFU_CANDIDATE ? SIG_MSG_CANDIDATE
                                                                  : SIG_MSG_ERROR;
                snprintf(message.from, sizeof(message.from), "%s", event->remote);
                if (peer->version >= 2)
                    snprintf(message.session_id, sizeof(message.session_id), "%s",
                             event->session_id);
                if (message.type == SIG_MSG_CANDIDATE)
                    snprintf(message.candidate, sizeof(message.candidate), "%.*s",
                             (int)sizeof(message.candidate) - 1, event->text);
                else if (message.type == SIG_MSG_ERROR)
                    snprintf(message.error_msg, sizeof(message.error_msg), "%.*s",
                             (int)sizeof(message.error_msg) - 1, event->text);
                else
                    snprintf(message.sdp, sizeof(message.sdp), "%s", event->text);
                char *json = sig_msg_build(&message);
                if (json) {
                    g_mgr.send_fn(peer, json, g_mgr.send_user);
                    free(json);
                }
            }
        }
        free(item);
    }
}
#endif

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

    switch (msg.type) {
        case SIG_MSG_JOIN:
            meeting_join_message(&g_mgr, pss->peer, &msg, rtc_time_ms());
            break;

        case SIG_MSG_LEAVE:
            meeting_leave(&g_mgr, pss->peer);
            break;

        case SIG_MSG_OFFER:
        case SIG_MSG_ANSWER:
        case SIG_MSG_CANDIDATE: {
            int mode = meeting_validate_session(&g_mgr, pss->peer, &msg);
            int rc = -1;
            if (mode == MEETING_MODE_MESH) {
                snprintf(msg.from, sizeof(msg.from), "%s", pss->peer->id);
                char *relay = sig_msg_build(&msg);
                if (relay) {
                    rc = meeting_route_message(&g_mgr, pss->peer, msg.to, relay);
                    free(relay);
                }
            }
#ifdef MRTC_SIGNALING_SFU
            else if (mode == MEETING_MODE_SFU && g_sfu) {
                meeting_t *room = &g_mgr.meetings[pss->peer->meeting_idx];
                rtc_sfu_command_t command = {.version = pss->peer->version,
                                             .epoch =
                                                 pss->peer->version >= 2 ? msg.epoch : room->epoch};
                command.type = msg.type == SIG_MSG_OFFER    ? RTC_SFU_OFFER
                               : msg.type == SIG_MSG_ANSWER ? RTC_SFU_ANSWER
                                                            : RTC_SFU_CANDIDATE;
                snprintf(command.room, sizeof(command.room), "%s", room->name);
                snprintf(command.owner, sizeof(command.owner), "%s", pss->peer->id);
                snprintf(command.remote, sizeof(command.remote), "%s", msg.to);
                snprintf(command.session_id, sizeof(command.session_id), "%s", msg.session_id);
                snprintf(command.text, sizeof(command.text), "%s",
                         msg.type == SIG_MSG_CANDIDATE ? msg.candidate : msg.sdp);
                command.peer_count = (unsigned)room->peer_count;
                for (int i = 0; i < room->peer_count; i++)
                    snprintf(command.peers[i], sizeof(command.peers[i]), "%s", room->peers[i]->id);
                rc = rtc_sfu_handle(g_sfu, &command);
            }
#endif
            if (rc != 0) {
                if (mode >= 0 && pss->peer->meeting_idx >= 0) {
                    meeting_t *room = &g_mgr.meetings[pss->peer->meeting_idx];
                    if (room->phase == MEETING_PREPARING && msg.epoch == room->pending_epoch) {
                        sig_msg_t failure = {
                            .type = SIG_MSG_TOPOLOGY_ABORT, .version = 2, .epoch = msg.epoch};
                        snprintf(failure.mode, sizeof(failure.mode), "%s",
                                 room->desired_mode == MEETING_MODE_SFU ? "sfu" : "mesh");
                        snprintf(failure.reason, sizeof(failure.reason), "SFU negotiation failed");
                        meeting_handle_topology(&g_mgr, pss->peer, &failure, rtc_time_ms());
                    }
                }
                char *error = sig_msg_build_error(mode < 0 ? "stale or invalid session"
                                                           : "negotiation rejected");
                if (error) {
                    queue_message(pss, error);
                    free(error);
                }
            }
            break;
        }
        case SIG_MSG_TOPOLOGY_READY:
        case SIG_MSG_TOPOLOGY_ACK:
        case SIG_MSG_TOPOLOGY_ABORT:
        case SIG_MSG_TOPOLOGY_REQUEST: {
            if (meeting_handle_topology(&g_mgr, pss->peer, &msg, rtc_time_ms()) != 0) {
                char *error = sig_msg_build_error("stale or invalid topology message");
                if (error) {
                    queue_message(pss, error);
                    free(error);
                }
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
            if (pss->peer && pss->peer->protocol_failed && !pss->msg_count)
                return -1;
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
            if (pss->peer && pss->peer->protocol_failed)
                lws_callback_on_writable(wsi);
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
            rtc_mutex_destroy(&pss->mutex);
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {.name = "http", .callback = lws_callback_http_dummy},
    {
        .name = "signaling",
        .callback = callback_signaling,
        .per_session_data_size = sizeof(struct per_session_data),
        .rx_buffer_size = 8192,
    },
    LWS_PROTOCOL_LIST_TERM};

static void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
}

static lws_sorted_usec_list_t g_tick;

static void control_tick(lws_sorted_usec_list_t *timer) {
    (void)timer;
    lws_sul_schedule(g_context, 0, &g_tick, control_tick, 10000);
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

static bool parse_number(const char *text, uint64_t max, uint64_t *out) {
    if (!text || text[0] < '0' || text[0] > '9')
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end || value > max)
        return false;
    *out = value;
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
    meeting_mode_t policy = MEETING_MODE_AUTO;
    bool mode_set = false;
    unsigned p2p_max = 2;
    uint64_t downgrade_delay_ms = 15000;
    const char *web_root = NULL;
    const char *listen_ip = "127.0.0.1";
    const char *announced_ip = NULL;
    uint16_t media_port = 0;
    for (int i = first_option; i < argc; i++) {
        if (strcmp(argv[i], "--mesh") == 0) {
            if (mode_set && policy != MEETING_MODE_MESH) {
                fprintf(stderr, "Conflicting topology mode options\n");
                return 1;
            }
            policy = MEETING_MODE_MESH;
            mode_set = true;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            meeting_mode_t next;
            if (!strcmp(value, "mesh"))
                next = MEETING_MODE_MESH;
            else if (!strcmp(value, "sfu"))
                next = MEETING_MODE_SFU;
            else if (!strcmp(value, "auto"))
                next = MEETING_MODE_AUTO;
            else {
                fprintf(stderr, "Invalid topology mode: %s\n", value);
                return 1;
            }
            if (mode_set && next != policy) {
                fprintf(stderr, "Conflicting topology mode options\n");
                return 1;
            }
            policy = next;
            mode_set = true;
        } else if (strcmp(argv[i], "--p2p-max") == 0 && i + 1 < argc) {
            uint64_t value;
            if (!parse_number(argv[++i], MEETING_MAX_PEERS, &value) || value < 2) {
                fprintf(stderr, "Invalid P2P maximum: %s (expected 2..%d)\n", argv[i],
                        MEETING_MAX_PEERS);
                return 1;
            }
            p2p_max = (unsigned)value;
        } else if (strcmp(argv[i], "--downgrade-delay-ms") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], UINT32_MAX, &downgrade_delay_ms)) {
                fprintf(stderr, "Invalid downgrade delay: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], false, &port)) {
                fprintf(stderr, "Invalid signaling port: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) {
            web_root = argv[++i];
            struct stat status;
            if (!web_root[0] || stat(web_root, &status) != 0 || !S_ISDIR(status.st_mode)) {
                fprintf(stderr, "Invalid web root: %s\n", web_root);
                return 1;
            }
        } else if ((strcmp(argv[i], "--listen-ip") == 0 || strcmp(argv[i], "--bind") == 0) &&
                   i + 1 < argc && argv[i + 1][0] && argv[i + 1][0] != '-')
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
    g_mgr.policy = policy;
    g_mgr.p2p_max = p2p_max;
    g_mgr.downgrade_delay_ms = downgrade_delay_ms;
    rtc_addr_t address;
    if (rtc_addr_from_string(&address, listen_ip, port) != RTC_OK ||
        (announced_ip && rtc_addr_from_string(&address, announced_ip, 1) != RTC_OK)) {
        fprintf(stderr, "Invalid listen or announced IP address\n");
        return 1;
    }
    if (rtc_init() != RTC_OK)
        return 1;
#ifdef MRTC_SIGNALING_SFU
    if (rtc_sctp_init() != RTC_OK || rtc_mutex_init(&g_events_mutex) != RTC_OK)
        return 1;
    if (policy != MEETING_MODE_MESH) {
        g_sfu = rtc_sfu_create(&(rtc_sfu_config_t){
            .listen_ip = listen_ip,
            .announced_ip = announced_ip,
            .port = media_port,
            .subscription_id = subscription_id,
            .max_io_sources = 1 + 2 * MEETING_MAX_ROOMS * MEETING_MAX_PEERS * MEETING_MAX_PEERS,
            .on_event = on_media_event});
        if (!g_sfu) {
            fprintf(stderr, "SFU initialization failed\n");
            rtc_sctp_cleanup();
            rtc_cleanup();
            return 1;
        }
        g_mgr.retire_fn = retire_media;
    }
#else
    (void)listen_ip;
    (void)announced_ip;
    (void)media_port;
    if (policy != MEETING_MODE_MESH) {
        fprintf(stderr, "SFU/auto require the routing API; use --mode mesh explicitly\n");
        return 1;
    }
#endif

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.iface = listen_ip;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
    struct lws_http_mount mount = {.mountpoint = "/",
                                   .origin = web_root,
                                   .def = "index.html",
                                   .origin_protocol = LWSMPRO_FILE,
                                   .mountpoint_len = 1};
    if (web_root)
        info.mounts = &mount;

    struct lws_context *ctx = lws_create_context(&info);
    if (!ctx) {
        fprintf(stderr, "Failed to create lws context\n");
        return 1;
    }
    g_context = ctx;
    lws_sul_schedule(ctx, 0, &g_tick, control_tick, 10000);

    fprintf(stderr, "mrtc_server listening on %s:%u mode=%s\n", listen_ip, port,
            policy == MEETING_MODE_AUTO  ? "auto"
            : policy == MEETING_MODE_SFU ? "sfu"
                                         : "mesh");

    while (!interrupted) {
        meeting_tick(&g_mgr, rtc_time_ms());
#ifdef MRTC_SIGNALING_SFU
        drain_media_events();
#endif
        if (atomic_exchange(&g_pending, false))
            lws_callback_on_writable_all_protocol(ctx, &protocols[1]);
        lws_service(ctx, 10);
    }

    lws_sul_cancel(&g_tick);
    lws_context_destroy(ctx);
    g_context = NULL;
#ifdef MRTC_SIGNALING_SFU
    rtc_sfu_destroy(g_sfu);
    g_sfu = NULL;
    drain_media_events();
    rtc_mutex_destroy(&g_events_mutex);
    rtc_sctp_cleanup();
#endif
    rtc_cleanup();
    meeting_manager_close(&g_mgr);
    fprintf(stderr, "Signaling server stopped\n");
    return 0;
}
