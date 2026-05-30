/*
 * signaling_client.c - WebSocket signaling client using libwebsockets.
 */
#include "signaling/signaling_client.h"
#include "signaling/signaling_msg.h"
#include <rtc/rtc_vec.h>
#include <libwebsockets.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct signaling_client {
    signaling_config_t cfg;
    struct lws_context *lws_ctx;
    struct lws *wsi;
    rtc_thread_t thread;
    _Atomic bool running;
    _Atomic bool connected;

    /* Outgoing message queue (thread-safe via mutex). FIFO of char* (owned). */
    rtc_mutex_t queue_mutex;
    rtc_vec_t queue;

    /* Parsed server URL */
    char host[128];
    char path[128];
    int port;
    bool use_ssl;
};

static void enqueue_msg(signaling_client_t *sc, char *json) {
    rtc_mutex_lock(&sc->queue_mutex);
    if (rtc_vec_push(&sc->queue, &json) != RTC_OK) {
        RTC_LOG_WARN("signaling-client: dropping outbound message (alloc failure)");
        free(json);
    }
    rtc_mutex_unlock(&sc->queue_mutex);
    if (sc->lws_ctx)
        lws_cancel_service(sc->lws_ctx);
    if (sc->wsi)
        lws_callback_on_writable(sc->wsi);
}

static char *dequeue_msg(signaling_client_t *sc) {
    char *msg = NULL;
    rtc_mutex_lock(&sc->queue_mutex);
    if (rtc_vec_len(&sc->queue) > 0) {
        msg = *(char **)rtc_vec_at(&sc->queue, 0);
        rtc_vec_remove(&sc->queue, 0);
    }
    rtc_mutex_unlock(&sc->queue_mutex);
    return msg;
}

static void dispatch_message(signaling_client_t *sc, const char *data, size_t len) {
    sig_msg_t msg;
    if (sig_msg_parse(&msg, data, len) != 0)
        return;

    switch (msg.type) {
        case SIG_MSG_JOINED:
            if (sc->cfg.on_joined) {
                const char *peers[SIG_MAX_PEERS];
                for (int i = 0; i < msg.peer_count; i++)
                    peers[i] = msg.peers[i];
                sc->cfg.on_joined(msg.peer_id, peers, msg.peer_count, sc->cfg.user_data);
            }
            break;
        case SIG_MSG_PEER_JOINED:
            if (sc->cfg.on_peer_joined)
                sc->cfg.on_peer_joined(msg.peer_id, sc->cfg.user_data);
            break;
        case SIG_MSG_PEER_LEFT:
            if (sc->cfg.on_peer_left)
                sc->cfg.on_peer_left(msg.peer_id, sc->cfg.user_data);
            break;
        case SIG_MSG_OFFER:
            if (sc->cfg.on_offer)
                sc->cfg.on_offer(msg.from, msg.sdp, sc->cfg.user_data);
            break;
        case SIG_MSG_ANSWER:
            if (sc->cfg.on_answer)
                sc->cfg.on_answer(msg.from, msg.sdp, sc->cfg.user_data);
            break;
        case SIG_MSG_CANDIDATE:
            if (sc->cfg.on_candidate)
                sc->cfg.on_candidate(msg.from, msg.candidate, sc->cfg.user_data);
            break;
        case SIG_MSG_ERROR:
            if (sc->cfg.on_error)
                sc->cfg.on_error(msg.error_msg, sc->cfg.user_data);
            break;
        default:
            break;
    }
}

static int callback_client(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                           size_t len) {
    (void)user;
    signaling_client_t *sc = (signaling_client_t *)lws_context_user(lws_get_context(wsi));
    if (!sc)
        return 0;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            sc->connected = true;
            sc->wsi = wsi;
            /* Auto-join the meeting */
            if (sc->cfg.meeting) {
                char *join = sig_msg_build_join(sc->cfg.meeting);
                if (join)
                    enqueue_msg(sc, join);
            }
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            dispatch_message(sc, (const char *)in, len);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            char *json = dequeue_msg(sc);
            if (json) {
                size_t json_len = strlen(json);
                unsigned char *buf = malloc(LWS_PRE + json_len);
                if (buf) {
                    memcpy(buf + LWS_PRE, json, json_len);
                    lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
                    free(buf);
                }
                free(json);
            }
            /* Check if more messages pending */
            rtc_mutex_lock(&sc->queue_mutex);
            size_t more = rtc_vec_len(&sc->queue);
            rtc_mutex_unlock(&sc->queue_mutex);
            if (more > 0)
                lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            fprintf(stderr, "[signaling-client] connection error: %s\n",
                    in ? (const char *)in : "unknown");
            sc->connected = false;
            sc->wsi = NULL;
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            sc->connected = false;
            sc->wsi = NULL;
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols client_protocols[] = {{
                                                            .name = "signaling",
                                                            .callback = callback_client,
                                                            .per_session_data_size = 0,
                                                            .rx_buffer_size = 8192,
                                                        },
                                                        LWS_PROTOCOL_LIST_TERM};

static int parse_url(signaling_client_t *sc, const char *url) {
    /* Parse ws://host:port/path or wss://host:port/path */
    sc->use_ssl = false;
    sc->port = 80;
    sc->path[0] = '/';
    sc->path[1] = '\0';

    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) {
        sc->use_ssl = true;
        sc->port = 443;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else
        return -1;

    /* Extract host */
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= sizeof(sc->host))
            hlen = sizeof(sc->host) - 1;
        memcpy(sc->host, p, hlen);
        sc->host[hlen] = '\0';
        sc->port = atoi(colon + 1);
        if (slash)
            snprintf(sc->path, sizeof(sc->path), "%s", slash);
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= sizeof(sc->host))
            hlen = sizeof(sc->host) - 1;
        memcpy(sc->host, p, hlen);
        sc->host[hlen] = '\0';
        snprintf(sc->path, sizeof(sc->path), "%s", slash);
    } else {
        snprintf(sc->host, sizeof(sc->host), "%s", p);
    }

    return 0;
}

static void *client_thread_fn(void *arg) {
    signaling_client_t *sc = (signaling_client_t *)arg;

    struct lws_client_connect_info cci;
    memset(&cci, 0, sizeof(cci));
    cci.context = sc->lws_ctx;
    cci.address = sc->host;
    cci.port = sc->port;
    cci.path = sc->path;
    cci.host = sc->host;
    cci.origin = sc->host;
    cci.protocol = "signaling";
    cci.ssl_connection = sc->use_ssl ? LCCSCF_USE_SSL : 0;

    sc->wsi = lws_client_connect_via_info(&cci);

    while (sc->running) {
        lws_service(sc->lws_ctx, 100);
    }

    return NULL;
}

signaling_client_t *signaling_create(const signaling_config_t *cfg) {
    if (!cfg || !cfg->server_url)
        return NULL;

    signaling_client_t *sc = calloc(1, sizeof(signaling_client_t));
    if (!sc)
        return NULL;

    sc->cfg = *cfg;
    /* Caller may pass stack-allocated strings; deep-copy the ones we hold
     * past return. Callbacks/user_data remain caller-owned by API contract. */
    sc->cfg.server_url = strdup(cfg->server_url);
    sc->cfg.meeting = cfg->meeting ? strdup(cfg->meeting) : NULL;
    if (!sc->cfg.server_url || (cfg->meeting && !sc->cfg.meeting)) {
        free((void *)sc->cfg.server_url);
        free((void *)sc->cfg.meeting);
        free(sc);
        return NULL;
    }
    if (parse_url(sc, sc->cfg.server_url) != 0) {
        free((void *)sc->cfg.server_url);
        free((void *)sc->cfg.meeting);
        free(sc);
        return NULL;
    }
    rtc_mutex_init(&sc->queue_mutex);
    rtc_vec_init(&sc->queue, sizeof(char *));

    return sc;
}

rtc_err_t signaling_connect(signaling_client_t *sc) {
    if (!sc)
        return RTC_ERR_INVALID;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = client_protocols;
    info.user = sc;

    sc->lws_ctx = lws_create_context(&info);
    if (!sc->lws_ctx)
        return RTC_ERR_GENERIC;

    sc->running = true;
    if (rtc_thread_create(&sc->thread, client_thread_fn, sc) != 0) {
        lws_context_destroy(sc->lws_ctx);
        sc->lws_ctx = NULL;
        return RTC_ERR_GENERIC;
    }

    return RTC_OK;
}

rtc_err_t signaling_send_offer(signaling_client_t *sc, const char *to, const char *sdp) {
    if (!sc || !sc->connected)
        return RTC_ERR_INVALID;
    char *msg = sig_msg_build_offer(to, sdp);
    if (!msg)
        return RTC_ERR_NOMEM;
    enqueue_msg(sc, msg);
    return RTC_OK;
}

rtc_err_t signaling_send_answer(signaling_client_t *sc, const char *to, const char *sdp) {
    if (!sc || !sc->connected)
        return RTC_ERR_INVALID;
    char *msg = sig_msg_build_answer(to, sdp);
    if (!msg)
        return RTC_ERR_NOMEM;
    enqueue_msg(sc, msg);
    return RTC_OK;
}

rtc_err_t signaling_send_candidate(signaling_client_t *sc, const char *to, const char *candidate) {
    if (!sc || !sc->connected)
        return RTC_ERR_INVALID;
    char *msg = sig_msg_build_candidate(to, candidate);
    if (!msg)
        return RTC_ERR_NOMEM;
    enqueue_msg(sc, msg);
    return RTC_OK;
}

rtc_err_t signaling_leave(signaling_client_t *sc) {
    if (!sc || !sc->connected)
        return RTC_ERR_INVALID;
    char *msg = sig_msg_build_leave();
    if (!msg)
        return RTC_ERR_NOMEM;
    enqueue_msg(sc, msg);
    return RTC_OK;
}

void signaling_destroy(signaling_client_t *sc) {
    if (!sc)
        return;
    sc->running = false;
    if (sc->lws_ctx)
        lws_cancel_service(sc->lws_ctx);
    rtc_thread_join(&sc->thread);
    if (sc->lws_ctx)
        lws_context_destroy(sc->lws_ctx);

    /* Free any remaining queued messages */
    rtc_mutex_lock(&sc->queue_mutex);
    size_t qn = rtc_vec_len(&sc->queue);
    for (size_t i = 0; i < qn; i++) {
        char *m = *(char **)rtc_vec_at(&sc->queue, i);
        free(m);
    }
    rtc_vec_free(&sc->queue);
    rtc_mutex_unlock(&sc->queue_mutex);
    rtc_mutex_destroy(&sc->queue_mutex);

    free((void *)sc->cfg.server_url);
    free((void *)sc->cfg.meeting);
    free(sc);
}
