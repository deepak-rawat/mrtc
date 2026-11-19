#include "rtc_turn_runtime.h"
#include "rtc_worker_internal.h"

#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#ifndef _WIN32
#  include <netinet/tcp.h>
#endif

#define TURN_STREAM_BUFFER   65536
#define TURN_STREAM_QUEUE_MS 500

struct rtc_turn_runtime {
    rtc_worker_t *worker;
    rtc_packet_io_t io;
    rtc_turn_client_t client;
    char host[TURN_HOST_MAX], username[TURN_USERNAME_MAX], credential[TURN_CREDENTIAL_MAX];
    rtc_packet_io_recv_fn receive;
    void *user;
    rtc_worker_timer_t timer;
    rtc_stun_msg_t request;
    uint16_t method;
    unsigned attempts;
    unsigned challenges;
    uint64_t refresh_ms;
    _Atomic uint64_t allocation_expires_ms;
    rtc_addr_t peer;
    bool peer_set;
    bool bound;
    _Atomic bool failed;
    _Atomic bool closed;
    _Atomic unsigned refs;
    rtc_mutex_t mutex;
    bool stream;
    bool io_ready;
    bool io_registered;
    bool connecting;
    bool handshaking;
    bool read_wants_write;
    bool flush_posted;
    rtc_socket_t socket;
    SSL_CTX *tls_context;
    SSL *tls;
    uint8_t input[TURN_STREAM_BUFFER];
    size_t input_len;
    uint8_t output[TURN_STREAM_BUFFER];
    size_t output_len;
    size_t write_len;
    uint64_t queued_ms;
    uint64_t connect_deadline;
    rtc_worker_timer_t stream_timer;
};

static void stream_service(rtc_turn_runtime_t *turn, bool readiness);

static void turn_release(rtc_turn_runtime_t *turn) {
    if (atomic_fetch_sub(&turn->refs, 1) == 1) {
        rtc_mutex_destroy(&turn->mutex);
        OPENSSL_cleanse(turn->credential, sizeof(turn->credential));
        OPENSSL_cleanse(turn->client.lt_key, sizeof(turn->client.lt_key));
        free(turn);
    }
}

static void turn_fail(rtc_turn_runtime_t *turn, const char *reason) {
    rtc_mutex_lock(&turn->mutex);
    bool first = !atomic_exchange(&turn->failed, true);
    turn->bound = false;
    rtc_mutex_unlock(&turn->mutex);
    if (first)
        RTC_LOG_ERR("TURN: %s", reason);
    if (turn->io_registered) {
        rtc_worker_remove_io(turn->worker,
                             turn->stream ? turn->socket : rtc_packet_io_get_socket(&turn->io));
        turn->io_registered = false;
    }
}

static bool socket_pending(void) {
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EALREADY ||
           errno == EINTR;
#endif
}

static void stream_interest(rtc_turn_runtime_t *turn, int events) {
    if (turn->io_registered && rtc_worker_update_io(turn->worker, turn->socket, events) != RTC_OK)
        turn_fail(turn, "stream readiness update failed");
}

static void stream_flush_task(void *user) {
    rtc_turn_runtime_t *turn = user;
    rtc_mutex_lock(&turn->mutex);
    turn->flush_posted = false;
    rtc_mutex_unlock(&turn->mutex);
    if (!atomic_load(&turn->closed))
        stream_service(turn, false);
    turn_release(turn);
}

/* Caller holds mutex; posting rather than invoking avoids a transport-send
 * mutex -> worker -> transport-send mutex deadlock. */
static int stream_queue_locked(rtc_turn_runtime_t *turn, const uint8_t *data, size_t len) {
    if (atomic_load(&turn->closed) || atomic_load(&turn->failed))
        return RTC_ERR_INVALID;
    if (len > sizeof(turn->output) - turn->output_len)
        return RTC_ERR_NOMEM;
    if (!turn->output_len)
        turn->queued_ms = rtc_time_ms();
    memcpy(turn->output + turn->output_len, data, len);
    turn->output_len += len;
    if (!turn->flush_posted) {
        atomic_fetch_add(&turn->refs, 1);
        turn->flush_posted = true;
        int rc = rtc_worker_post(turn->worker, stream_flush_task, turn);
        if (rc != RTC_OK) {
            turn->flush_posted = false;
            turn->output_len -= len;
            atomic_fetch_sub(&turn->refs, 1);
            return rc;
        }
    }
    return RTC_OK;
}

static int turn_wire_send(rtc_turn_runtime_t *turn, const uint8_t *data, size_t len) {
    if (!turn->stream)
        return rtc_packet_io_send(&turn->io, data, len, &turn->client.server_addr);
    rtc_mutex_lock(&turn->mutex);
    int rc = stream_queue_locked(turn, data, len);
    rtc_mutex_unlock(&turn->mutex);
    return rc;
}

static bool turn_same_addr(const rtc_addr_t *a, const rtc_addr_t *b) {
    char ai[64], bi[64];
    uint16_t ap, bp;
    return rtc_addr_to_string(a, ai, sizeof(ai), &ap) == RTC_OK &&
           rtc_addr_to_string(b, bi, sizeof(bi), &bp) == RTC_OK && ap == bp && strcmp(ai, bi) == 0;
}

static int turn_build(rtc_turn_runtime_t *turn, uint16_t method, uint32_t lifetime) {
    rtc_turn_client_t *c = &turn->client;
    rtc_stun_msg_t *r = &turn->request;
    int rc = rtc_stun_build_request(r, method, NULL, NULL);
    if (rc != RTC_OK)
        return rc;
    if (c->has_credentials) {
        if (rtc_stun_add_username(r, turn->username) != RTC_OK ||
            rtc_stun_add_realm(r, c->realm) != RTC_OK || rtc_stun_add_nonce(r, c->nonce) != RTC_OK)
            return RTC_ERR_INVALID;
    }
    if (method == STUN_METHOD_ALLOCATE)
        rtc_stun_add_requested_transport(r, 17);
    if (method == STUN_METHOD_REFRESH)
        rtc_stun_add_lifetime(r, lifetime);
    if (method == STUN_METHOD_CREATE_PERM || method == STUN_METHOD_CHANNEL_BIND)
        rtc_stun_add_xor_peer_address(r, &turn->peer);
    if (method == STUN_METHOD_CHANNEL_BIND)
        rtc_stun_add_channel_number(r, TURN_CHANNEL_MIN);
    rc = rtc_stun_finalize_key(r, c->has_credentials ? c->lt_key : NULL,
                               c->has_credentials ? sizeof(c->lt_key) : 0);
    if (rc != RTC_OK)
        return rc;
    turn->method = method;
    turn->attempts = 0;
    return RTC_OK;
}

static void turn_tick(void *user);

static void turn_arm(rtc_turn_runtime_t *turn, uint64_t delay) {
    if (turn->timer != RTC_WORKER_TIMER_INVALID)
        rtc_worker_cancel_timer(turn->worker, turn->timer);
    turn->timer = rtc_worker_add_timer(turn->worker, rtc_time_ms() + delay, turn_tick, turn);
    if (turn->timer == RTC_WORKER_TIMER_INVALID)
        turn_fail(turn, "transaction timer allocation failed");
}

static int turn_send_request(rtc_turn_runtime_t *turn) {
    RTC_LOG_DBG("TURN sending method 0x%04x", turn->method);
    int rc = turn_wire_send(turn, turn->request.buf, turn->request.buf_len);
    turn->attempts++;
    turn_arm(turn, 500);
    if (rc != RTC_OK)
        RTC_LOG_ERR("TURN request send failed: %d", rc);
    return rc;
}

static void turn_tick(void *user) {
    rtc_turn_runtime_t *turn = user;
    turn->timer = RTC_WORKER_TIMER_INVALID;
    if (turn->failed)
        return;
    if (turn->client.allocated && rtc_time_ms() >= atomic_load(&turn->allocation_expires_ms)) {
        turn_fail(turn, "allocation expired");
        return;
    }
    if (turn->method) {
        if (turn->attempts >= (turn->stream ? 20u : 7u)) {
            turn_fail(turn, "transaction deadline exceeded");
            return;
        }
        if (turn->stream) {
            turn->attempts++;
            turn_arm(turn, 500);
            return;
        }
        (void)turn_send_request(turn);
        return;
    }
    if (rtc_time_ms() >= turn->refresh_ms) {
        if (turn_build(turn, STUN_METHOD_REFRESH, 600) == RTC_OK)
            (void)turn_send_request(turn);
    } else {
        turn_arm(turn, turn->refresh_ms - rtc_time_ms());
    }
}

static rtc_pkt_type_t turn_packet_type(const uint8_t *data, size_t len) {
    if (!len)
        return RTC_PKT_UNKNOWN;
    if (data[0] <= 3)
        return RTC_PKT_STUN;
    if (data[0] >= 20 && data[0] <= 63)
        return RTC_PKT_DTLS;
    if (data[0] >= 128 && data[0] <= 191 && len >= 2)
        return data[1] >= 192 && data[1] <= 223 ? RTC_PKT_RTCP : RTC_PKT_RTP;
    return RTC_PKT_UNKNOWN;
}

static void turn_receive(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                         const rtc_addr_t *from, void *user) {
    rtc_turn_runtime_t *turn = user;
    rtc_turn_client_t *c = &turn->client;
    if (!turn_same_addr(from, &c->server_addr) || turn->failed)
        return;
    if (type == RTC_PKT_CHANNEL_DATA) {
        uint16_t channel;
        const uint8_t *payload;
        size_t size;
        if (turn->bound &&
            rtc_turn_parse_channel_data(data, len, &channel, &payload, &size) == RTC_OK &&
            channel == TURN_CHANNEL_MIN)
            turn->receive(turn_packet_type(payload, size), payload, size, &turn->peer, turn->user);
        return;
    }
    rtc_stun_msg_t response;
    if (type != RTC_PKT_STUN || rtc_stun_parse(&response, data, len) != RTC_OK || !turn->method ||
        memcmp(response.txn_id, turn->request.txn_id, STUN_TXN_ID_SIZE) != 0)
        return;
    int code = 0;
    if (rtc_stun_get_error_code(&response, &code) == RTC_OK) {
        if ((code != 401 && code != 438) || ++turn->challenges > 3 ||
            (code == 401 && c->has_credentials) ||
            rtc_stun_get_realm(&response, c->realm, sizeof(c->realm)) != RTC_OK ||
            rtc_stun_get_nonce(&response, c->nonce, sizeof(c->nonce)) != RTC_OK ||
            rtc_stun_long_term_key(turn->username, c->realm, turn->credential, c->lt_key) !=
                RTC_OK) {
            turn_fail(turn, "server authentication challenge rejected");
            return;
        }
        c->has_credentials = true;
        if (turn_build(turn, turn->method, 600) == RTC_OK)
            (void)turn_send_request(turn);
        return;
    }
    if (response.type != (uint16_t)(turn->method | 0x0100) || !c->has_credentials ||
        rtc_stun_verify_integrity_key(data, len, c->lt_key, sizeof(c->lt_key)) != RTC_OK)
        return;
    uint16_t method = turn->method;
    RTC_LOG_DBG("TURN received method 0x%04x response", method);
    turn->method = 0;
    turn->challenges = 0;
    if (method == STUN_METHOD_ALLOCATE || method == STUN_METHOD_REFRESH) {
        if (rtc_stun_get_lifetime(&response, &c->lifetime) != RTC_OK || !c->lifetime ||
            (method == STUN_METHOD_ALLOCATE &&
             rtc_stun_get_relayed_address(&response, &c->relay_addr) != RTC_OK)) {
            turn_fail(turn, "invalid allocation response");
            return;
        }
        c->allocated = true;
        atomic_store(&turn->allocation_expires_ms, rtc_time_ms() + (uint64_t)c->lifetime * 1000);
        uint64_t refresh = (uint64_t)c->lifetime * 500;
        if (refresh > 120000)
            refresh = 120000;
        turn->refresh_ms = rtc_time_ms() + refresh;
        if (turn->peer_set) {
            if (turn_build(turn, STUN_METHOD_CREATE_PERM, 0) == RTC_OK)
                (void)turn_send_request(turn);
            return;
        }
    } else if (method == STUN_METHOD_CREATE_PERM) {
        if (turn_build(turn, STUN_METHOD_CHANNEL_BIND, 0) == RTC_OK)
            (void)turn_send_request(turn);
        return;
    } else if (method == STUN_METHOD_CHANNEL_BIND) {
        rtc_mutex_lock(&turn->mutex);
        turn->bound = true;
        rtc_mutex_unlock(&turn->mutex);
    }
    turn_arm(turn, turn->refresh_ms > rtc_time_ms() ? turn->refresh_ms - rtc_time_ms() : 1);
}

static void stream_timeout(void *user) {
    rtc_turn_runtime_t *turn = user;
    turn->stream_timer = RTC_WORKER_TIMER_INVALID;
    turn_fail(turn, "stream connection or queued-write deadline exceeded");
}

static void stream_update_deadline(rtc_turn_runtime_t *turn) {
    if (turn->stream_timer != RTC_WORKER_TIMER_INVALID) {
        rtc_worker_cancel_timer(turn->worker, turn->stream_timer);
        turn->stream_timer = RTC_WORKER_TIMER_INVALID;
    }
    rtc_mutex_lock(&turn->mutex);
    uint64_t deadline = turn->connecting || turn->handshaking ? turn->connect_deadline
                        : turn->output_len ? turn->queued_ms + TURN_STREAM_QUEUE_MS
                                           : 0;
    rtc_mutex_unlock(&turn->mutex);
    if (deadline && !atomic_load(&turn->failed)) {
        turn->stream_timer = rtc_worker_add_timer(turn->worker, deadline, stream_timeout, turn);
        if (turn->stream_timer == RTC_WORKER_TIMER_INVALID)
            turn_fail(turn, "stream deadline timer allocation failed");
    }
}

static int stream_flush(rtc_turn_runtime_t *turn) {
    for (unsigned batch = 0; batch < RTC_PACKET_IO_RECV_BATCH; batch++) {
        rtc_mutex_lock(&turn->mutex);
        if (!turn->output_len) {
            rtc_mutex_unlock(&turn->mutex);
            return 1;
        }
        if (!turn->write_len)
            turn->write_len = turn->output_len;
        size_t written = 0;
        int error = 0;
        bool pending = false;
        if (turn->tls) {
            ERR_clear_error();
            int rc = SSL_write_ex(turn->tls, turn->output, turn->write_len, &written);
            if (rc != 1) {
                error = SSL_get_error(turn->tls, rc);
                pending = error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
            }
        } else {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            int count =
                (int)send(turn->socket, (const char *)turn->output, (int)turn->write_len, flags);
            if (count > 0)
                written = (size_t)count;
            else {
                error = SSL_ERROR_WANT_WRITE;
                pending = count < 0 && socket_pending();
            }
        }
        if (written) {
            turn->output_len -= written;
            memmove(turn->output, turn->output + written, turn->output_len);
            turn->write_len = 0;
        }
        rtc_mutex_unlock(&turn->mutex);
        if (!written) {
            if (!pending) {
                turn_fail(turn, "stream write failed");
                return -1;
            }
            stream_interest(turn, error == SSL_ERROR_WANT_READ ? RTC_POLLER_EV_READ
                                                               : RTC_POLLER_EV_WRITE);
            return 0;
        }
    }
    stream_interest(turn, RTC_POLLER_EV_WRITE);
    return 0;
}

static int stream_parse(rtc_turn_runtime_t *turn) {
    size_t offset = 0;
    while (turn->input_len - offset >= 4) {
        const uint8_t *frame = turn->input + offset;
        size_t length;
        int rc = rtc_turn_stream_frame_length(frame, turn->input_len - offset, &length);
        if (rc == RTC_ERR_TIMEOUT)
            break;
        if (rc != RTC_OK)
            return rc;
        bool channel = frame[0] >= 0x40 && frame[0] <= 0x7f;
        turn_receive(channel ? RTC_PKT_CHANNEL_DATA : RTC_PKT_STUN, frame, length,
                     &turn->client.server_addr, turn);
        if (atomic_load(&turn->failed) || atomic_load(&turn->closed))
            return RTC_ERR_GENERIC;
        offset += length;
    }
    turn->input_len -= offset;
    memmove(turn->input, turn->input + offset, turn->input_len);
    return RTC_OK;
}

static int stream_read(rtc_turn_runtime_t *turn) {
    for (unsigned batch = 0; batch < RTC_PACKET_IO_RECV_BATCH; batch++) {
        size_t received = 0;
        int error = 0;
        bool pending = false;
        size_t capacity = sizeof(turn->input) - turn->input_len;
        if (!capacity) {
            turn_fail(turn, "stream receive buffer exhausted");
            return -1;
        }
        if (turn->tls) {
            ERR_clear_error();
            int rc = SSL_read_ex(turn->tls, turn->input + turn->input_len, capacity, &received);
            if (rc != 1) {
                error = SSL_get_error(turn->tls, rc);
                pending = error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
            }
        } else {
            int count =
                (int)recv(turn->socket, (char *)turn->input + turn->input_len, (int)capacity, 0);
            if (count > 0)
                received = (size_t)count;
            else {
                error = SSL_ERROR_WANT_READ;
                pending = count < 0 && socket_pending();
            }
        }
        if (!received) {
            if (!pending) {
                turn_fail(turn, "server closed or failed the stream");
                return -1;
            }
            turn->read_wants_write = error == SSL_ERROR_WANT_WRITE;
            if (turn->read_wants_write)
                stream_interest(turn, RTC_POLLER_EV_WRITE);
            return turn->read_wants_write ? 0 : 1;
        }
        turn->read_wants_write = false;
        turn->input_len += received;
        if (stream_parse(turn) != RTC_OK) {
            turn_fail(turn, "invalid TURN stream framing");
            return -1;
        }
    }
    return 1;
}

static void stream_service(rtc_turn_runtime_t *turn, bool readiness) {
    if (atomic_load(&turn->failed) || atomic_load(&turn->closed))
        return;
    if (turn->connecting) {
        if (!readiness)
            return;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(turn->socket, SOL_SOCKET, SO_ERROR, (char *)&error, &size) != 0 || error) {
            turn_fail(turn, "TCP connection failed");
            return;
        }
        turn->connecting = false;
    }
    if (turn->handshaking) {
        ERR_clear_error();
        int result = SSL_connect(turn->tls);
        if (result != 1) {
            int error = SSL_get_error(turn->tls, result);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                turn_fail(turn, "TLS handshake or certificate verification failed");
                return;
            }
            stream_interest(turn, error == SSL_ERROR_WANT_READ ? RTC_POLLER_EV_READ
                                                               : RTC_POLLER_EV_WRITE);
            return;
        }
        if (SSL_get_verify_result(turn->tls) != X509_V_OK) {
            turn_fail(turn, "TLS certificate verification failed");
            return;
        }
        turn->handshaking = false;
        rtc_mutex_lock(&turn->mutex);
        turn->queued_ms = rtc_time_ms();
        rtc_mutex_unlock(&turn->mutex);
    }
    stream_update_deadline(turn);
    if (atomic_load(&turn->failed))
        return;
    if (turn->read_wants_write && stream_read(turn) <= 0)
        return;
    if (stream_flush(turn) <= 0) {
        stream_update_deadline(turn);
        return;
    }
    if (stream_read(turn) <= 0)
        return;
    rtc_mutex_lock(&turn->mutex);
    bool output = turn->output_len != 0;
    rtc_mutex_unlock(&turn->mutex);
    stream_interest(turn, RTC_POLLER_EV_READ | (output ? RTC_POLLER_EV_WRITE : 0));
    stream_update_deadline(turn);
}

static void turn_io_ready(rtc_socket_t fd, void *user) {
    (void)fd;
    rtc_turn_runtime_t *turn = user;
    atomic_fetch_add(&turn->refs, 1);
    if (turn->stream)
        stream_service(turn, true);
    else
        rtc_packet_io_drain(&turn->io);
    turn_release(turn);
}

static int stream_init(rtc_turn_runtime_t *turn, const rtc_turn_config_t *cfg) {
    turn->socket = socket(turn->client.server_addr.addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (turn->socket == RTC_INVALID_SOCKET || rtc_set_nonblocking(turn->socket) != RTC_OK)
        return RTC_ERR_SOCKET;
    int no_delay = 1;
    if (setsockopt(turn->socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay,
                   sizeof(no_delay)) != 0)
        return RTC_ERR_SOCKET;
#ifdef SO_NOSIGPIPE
    int no_signal = 1;
    if (setsockopt(turn->socket, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal)) != 0)
        return RTC_ERR_SOCKET;
#endif
    if (cfg->protocol == RTC_TURN_TLS) {
        turn->tls_context = SSL_CTX_new(TLS_client_method());
        if (!turn->tls_context ||
            !SSL_CTX_set_min_proto_version(turn->tls_context, TLS1_2_VERSION) ||
            (cfg->ca_file ? SSL_CTX_load_verify_locations(turn->tls_context, cfg->ca_file, NULL)
                          : SSL_CTX_set_default_verify_paths(turn->tls_context)) != 1)
            return RTC_ERR_SSL;
        SSL_CTX_set_verify(turn->tls_context, SSL_VERIFY_PEER, NULL);
        turn->tls = SSL_new(turn->tls_context);
        if (!turn->tls || SSL_set_fd(turn->tls, (int)turn->socket) != 1)
            return RTC_ERR_SSL;
        SSL_set_mode(turn->tls,
                     SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        rtc_addr_t numeric;
        if (rtc_addr_from_string(&numeric, turn->host, 1) == RTC_OK) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(turn->tls), turn->host) != 1)
                return RTC_ERR_SSL;
        } else if (X509_VERIFY_PARAM_set1_host(SSL_get0_param(turn->tls), turn->host, 0) != 1 ||
                   SSL_set_tlsext_host_name(turn->tls, turn->host) != 1) {
            return RTC_ERR_SSL;
        }
        turn->handshaking = true;
    }
    int connected = connect(turn->socket, (struct sockaddr *)&turn->client.server_addr.addr,
                            turn->client.server_addr.len);
    if (connected != 0 && !socket_pending())
        return RTC_ERR_SOCKET;
    turn->connecting = connected != 0;
    int rc = rtc_worker_add_io_events(
        turn->worker, turn->socket, RTC_POLLER_EV_READ | RTC_POLLER_EV_WRITE, turn_io_ready, turn);
    if (rc != RTC_OK)
        return rc;
    turn->io_registered = true;
    turn->connect_deadline = rtc_time_ms() + 5000;
    stream_update_deadline(turn);
    return atomic_load(&turn->failed) ? RTC_ERR_GENERIC : RTC_OK;
}

rtc_turn_runtime_t *rtc_turn_runtime_create(rtc_worker_t *worker, const rtc_turn_config_t *cfg,
                                            rtc_packet_io_recv_fn receive, void *user) {
    if (!worker || !cfg || !cfg->server_host || !cfg->username || !cfg->credential || !receive)
        return NULL;
    rtc_turn_runtime_t *turn = calloc(1, sizeof(*turn));
    if (!turn)
        return NULL;
    turn->worker = worker;
    turn->receive = receive;
    turn->user = user;
    turn->timer = RTC_WORKER_TIMER_INVALID;
    turn->stream_timer = RTC_WORKER_TIMER_INVALID;
    turn->socket = RTC_INVALID_SOCKET;
    atomic_init(&turn->refs, 1);
    turn->stream = cfg->protocol != RTC_TURN_UDP;
    if (strlen(cfg->server_host) >= sizeof(turn->host) ||
        strlen(cfg->username) >= sizeof(turn->username) ||
        strlen(cfg->credential) >= sizeof(turn->credential)) {
        free(turn);
        return NULL;
    }
    strcpy(turn->host, cfg->server_host);
    strcpy(turn->username, cfg->username);
    strcpy(turn->credential, cfg->credential);
    rtc_turn_config_t owned = {.server_host = turn->host,
                               .server_port = cfg->server_port ? cfg->server_port : 3478,
                               .username = turn->username,
                               .credential = turn->credential,
                               .protocol = cfg->protocol,
                               .ca_file = cfg->ca_file,
                               .resolved_address = cfg->resolved_address};
    if (rtc_mutex_init(&turn->mutex) != RTC_OK) {
        free(turn);
        return NULL;
    }
    if (!turn->stream && rtc_packet_io_init(&turn->io, turn_receive, turn) != RTC_OK) {
        rtc_mutex_destroy(&turn->mutex);
        free(turn);
        return NULL;
    }
    turn->io_ready = !turn->stream;
    int rc = rtc_turn_init(&turn->client, &turn->io, &owned);
    if (rc == RTC_OK) {
        rc = turn->stream ? stream_init(turn, &owned)
                          : rtc_worker_add_io(worker, rtc_packet_io_get_socket(&turn->io),
                                              turn_io_ready, turn);
        if (rc == RTC_OK)
            turn->io_registered = true;
    }
    turn->client.cfg.resolved_address = NULL;
    turn->client.cfg.ca_file = NULL;
    if (rc != RTC_OK || turn_build(turn, STUN_METHOD_ALLOCATE, 0) != RTC_OK) {
        RTC_LOG_ERR("TURN allocation initialization failed: %d", rc);
        rtc_turn_runtime_destroy(turn);
        return NULL;
    }
    if (turn_send_request(turn) != RTC_OK) {
        rtc_turn_runtime_destroy(turn);
        return NULL;
    }
    return turn;
}

bool rtc_turn_runtime_allocated(const rtc_turn_runtime_t *turn) {
    return turn && turn->client.allocated;
}

bool rtc_turn_runtime_failed(const rtc_turn_runtime_t *turn) {
    return turn && atomic_load(&turn->failed);
}

void rtc_turn_runtime_abort(rtc_turn_runtime_t *turn) {
    if (turn)
        turn_fail(turn, "allocation address deadline exceeded");
}

int rtc_turn_runtime_candidate(rtc_turn_runtime_t *turn, rtc_addr_t *relay) {
    if (!turn || !relay || !turn->client.allocated || turn->failed ||
        rtc_time_ms() >= atomic_load(&turn->allocation_expires_ms))
        return RTC_ERR_INVALID;
    *relay = turn->client.relay_addr;
    return RTC_OK;
}

int rtc_turn_runtime_set_peer(rtc_turn_runtime_t *turn, const rtc_addr_t *peer) {
    if (!turn || !peer || turn->failed)
        return RTC_ERR_INVALID;
    if (turn->peer_set && turn_same_addr(peer, &turn->peer))
        return RTC_OK;
    if (turn->method && turn->method != STUN_METHOD_ALLOCATE)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&turn->mutex);
    turn->peer = *peer;
    turn->peer_set = true;
    turn->bound = false;
    rtc_mutex_unlock(&turn->mutex);
    if (turn->client.allocated) {
        if (turn_build(turn, STUN_METHOD_CREATE_PERM, 0) != RTC_OK)
            return RTC_ERR_GENERIC;
        return turn_send_request(turn);
    }
    return RTC_OK;
}

int rtc_turn_runtime_send(rtc_turn_runtime_t *turn, const uint8_t *data, size_t len,
                          const rtc_addr_t *peer) {
    if (!turn || !data || !peer)
        return RTC_ERR_INVALID;
    uint8_t frame[RTC_PACKET_IO_BUF_SIZE];
    size_t size;
    rtc_mutex_lock(&turn->mutex);
    int rc = RTC_ERR_INVALID;
    if (turn->bound && !turn->failed && turn_same_addr(peer, &turn->peer) &&
        rtc_time_ms() < atomic_load(&turn->allocation_expires_ms)) {
        rc = rtc_turn_build_channel_data(frame, sizeof(frame), TURN_CHANNEL_MIN, data, len, &size);
        if (rc == RTC_OK && turn->stream) {
            size_t padded = (size + 3) & ~(size_t)3;
            if (padded > sizeof(frame))
                rc = RTC_ERR_NOMEM;
            else {
                memset(frame + size, 0, padded - size);
                rc = stream_queue_locked(turn, frame, padded);
            }
        } else if (rc == RTC_OK) {
            rc = rtc_packet_io_send(&turn->io, frame, size, &turn->client.server_addr);
        }
    }
    rtc_mutex_unlock(&turn->mutex);
    return rc;
}

void rtc_turn_runtime_destroy(rtc_turn_runtime_t *turn) {
    if (!turn)
        return;
    if (turn->timer != RTC_WORKER_TIMER_INVALID)
        rtc_worker_cancel_timer(turn->worker, turn->timer);
    if (turn->stream_timer != RTC_WORKER_TIMER_INVALID)
        rtc_worker_cancel_timer(turn->worker, turn->stream_timer);
    atomic_store(&turn->closed, true);
    if (!turn->stream && turn->client.has_credentials &&
        turn_build(turn, STUN_METHOD_REFRESH, 0) == RTC_OK)
        (void)rtc_packet_io_send(&turn->io, turn->request.buf, turn->request.buf_len,
                                 &turn->client.server_addr);
    if (turn->io_registered)
        rtc_worker_remove_io(turn->worker,
                             turn->stream ? turn->socket : rtc_packet_io_get_socket(&turn->io));
    turn->io_registered = false;
    SSL_free(turn->tls);
    SSL_CTX_free(turn->tls_context);
    if (turn->socket != RTC_INVALID_SOCKET)
        rtc_close_socket(turn->socket);
    if (turn->io_ready)
        rtc_packet_io_close(&turn->io);
    turn_release(turn);
}
