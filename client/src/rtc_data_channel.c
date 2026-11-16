#include "rtc_data_channel_internal.h"
#include "rtc/rtc_sctp.h"

#include <usrsctp.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static atomic_uint_fast64_t dc_timer_time;
static rtc_mutex_t dc_stack_mutex;

typedef struct dc_event {
    struct dc_event *next;
    void *data;
    size_t len;
    struct sctp_rcvinfo info;
    int flags;
    bool outgoing;
} dc_event_t;

static void dc_queue_event(rtc_dc_manager_t *mgr, dc_event_t *event) {
    dc_event_t *tail = mgr->events_tail;
    if (tail)
        tail->next = event;
    else
        mgr->events_head = event;
    mgr->events_tail = event;
}

static void dc_process_events(rtc_dc_manager_t *mgr);

#define DC_PPID_DCEP         50
#define DC_PPID_TEXT         51
#define DC_PPID_BINARY       53
#define DC_PPID_EMPTY_TEXT   56
#define DC_PPID_EMPTY_BINARY 57
#define DC_QUEUE_LIMIT       (1024 * 1024)

typedef struct dc_message {
    struct dc_message *next;
    size_t len;
    uint32_t ppid;
    uint8_t data[];
} dc_message_t;

struct rtc_data_channel {
    char label[RTC_DC_MAX_LABEL];
    char protocol[RTC_DC_MAX_LABEL];
    _Atomic uint16_t id;
    _Atomic rtc_data_channel_state_t state;
    bool locally_created;
    bool automatic_id;
    bool ordered;
    bool negotiated;
    int max_retransmits;
    int max_packet_life;
    bool open_sent;
    bool reset_sent;
    rtc_on_dc_open_fn on_open;
    void *on_open_user;
    rtc_on_dc_close_fn on_close;
    void *on_close_user;
    rtc_on_dc_message_fn on_message;
    void *on_message_user;
    rtc_on_dc_buffered_amount_low_fn on_buffered_low;
    void *on_buffered_low_user;
    _Atomic uint64_t buffered_amount;
    _Atomic uint64_t buffered_amount_low_thresh;
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t bytes_received;
    dc_message_t *head;
    dc_message_t *tail;
    uint8_t *incoming;
    size_t incoming_len;
    rtc_dc_manager_t *manager;
};

static void dc_request_reset(rtc_data_channel_t *dc);

static rtc_data_channel_t *dc_find(rtc_dc_manager_t *mgr, uint16_t id) {
    return rtc_u32_map_get(&mgr->channels, id);
}

static int dc_dispatch(rtc_dc_manager_t *mgr, void (*fn)(void *), void *arg) {
    if (mgr->dispatch)
        return mgr->dispatch(mgr->dispatch_owner, fn, arg);
    fn(arg);
    return RTC_OK;
}

static void dc_open(rtc_data_channel_t *dc) {
    if (dc->state != RTC_DC_CONNECTING)
        return;
    dc->state = RTC_DC_OPEN;
    if (dc->on_open)
        dc->on_open(dc->on_open_user);
}

static void dc_closed(rtc_data_channel_t *dc) {
    if (dc->state == RTC_DC_CLOSED)
        return;
    dc->state = RTC_DC_CLOSED;
    if (dc->on_close)
        dc->on_close(dc->on_close_user);
}

static int dc_transmit(rtc_data_channel_t *dc, const void *data, size_t len, uint32_t ppid) {
    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = dc->id;
    spa.sendv_sndinfo.snd_ppid = htonl(ppid);
    if (ppid != DC_PPID_DCEP) {
        if (!dc->ordered)
            spa.sendv_sndinfo.snd_flags = SCTP_UNORDERED;
        if (dc->max_retransmits >= 0 || dc->max_packet_life >= 0) {
            spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
            spa.sendv_prinfo.pr_policy =
                dc->max_retransmits >= 0 ? SCTP_PR_SCTP_RTX : SCTP_PR_SCTP_TTL;
            spa.sendv_prinfo.pr_value =
                (uint32_t)(dc->max_retransmits >= 0 ? dc->max_retransmits : dc->max_packet_life);
        }
    }
    uint8_t empty = 0;
    rtc_mutex_lock(&dc_stack_mutex);
    ssize_t n = usrsctp_sendv(dc->manager->socket, len ? data : &empty, len ? len : 1, NULL, 0,
                              &spa, sizeof(spa), SCTP_SENDV_SPA, 0);
    int error = errno;
    rtc_mutex_unlock(&dc_stack_mutex);
    if (n < 0)
        return error == EWOULDBLOCK || error == EAGAIN ? RTC_ERR_NOMEM : RTC_ERR_GENERIC;
    return RTC_OK;
}

static int dc_send_open(rtc_data_channel_t *dc) {
    if (dc->negotiated) {
        dc_open(dc);
        return RTC_OK;
    }
    uint8_t data[12 + 2 * RTC_DC_MAX_LABEL];
    size_t label_len = strlen(dc->label), protocol_len = strlen(dc->protocol);
    memset(data, 0, 12);
    data[0] = 3;
    data[1] = (uint8_t)((dc->ordered ? 0 : 0x80) | (dc->max_retransmits >= 0   ? 1
                                                    : dc->max_packet_life >= 0 ? 2
                                                                               : 0));
    uint32_t reliability = htonl((uint32_t)(dc->max_retransmits >= 0   ? dc->max_retransmits
                                            : dc->max_packet_life >= 0 ? dc->max_packet_life
                                                                       : 0));
    memcpy(data + 4, &reliability, 4);
    data[8] = (uint8_t)(label_len >> 8);
    data[9] = (uint8_t)label_len;
    data[10] = (uint8_t)(protocol_len >> 8);
    data[11] = (uint8_t)protocol_len;
    memcpy(data + 12, dc->label, label_len);
    memcpy(data + 12 + label_len, dc->protocol, protocol_len);
    int rc = dc_transmit(dc, data, 12 + label_len + protocol_len, DC_PPID_DCEP);
    if (rc == RTC_OK)
        dc->open_sent = true;
    return rc;
}

static void dc_flush(rtc_data_channel_t *dc) {
    if (dc->state == RTC_DC_CONNECTING && !dc->open_sent)
        (void)dc_send_open(dc);
    while ((dc->state == RTC_DC_OPEN || dc->state == RTC_DC_CLOSING) && dc->head) {
        dc_message_t *msg = dc->head;
        if (dc_transmit(dc, msg->data, msg->len, msg->ppid) != RTC_OK)
            break;
        uint64_t before = dc->buffered_amount;
        dc->buffered_amount -= msg->len;
        dc->head = msg->next;
        if (!dc->head)
            dc->tail = NULL;
        free(msg);
        if (before > dc->buffered_amount_low_thresh &&
            dc->buffered_amount <= dc->buffered_amount_low_thresh && dc->on_buffered_low)
            dc->on_buffered_low(dc->on_buffered_low_user);
    }
    if (dc->state == RTC_DC_CLOSING && !dc->head)
        dc_request_reset(dc);
}

static void dc_each_flush(rtc_dc_manager_t *mgr) {
    if (!mgr->association_up)
        return;
    rtc_u32_map_iter_t it = {0};
    uint32_t id;
    void *value;
    while (rtc_u32_map_next(&mgr->channels, &it, &id, &value))
        dc_flush(value);
}

static int dc_output(void *addr, void *buffer, size_t len, uint8_t tos, uint8_t df) {
    (void)tos;
    (void)df;
    rtc_dc_manager_t *mgr = addr;
    if (!mgr || !mgr->dtls_connected)
        return EIO;
    dc_event_t *event = calloc(1, sizeof(*event));
    if (!event)
        return ENOMEM;
    event->data = malloc(len);
    if (!event->data) {
        free(event);
        return ENOMEM;
    }
    memcpy(event->data, buffer, len);
    event->len = len;
    event->outgoing = true;
    dc_queue_event(mgr, event);
    return 0;
}

int rtc_dc_global_init(void) {
    if (rtc_mutex_init(&dc_stack_mutex) != RTC_OK)
        return RTC_ERR_GENERIC;
    usrsctp_init_nothreads(0, dc_output, NULL);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    atomic_store(&dc_timer_time, rtc_time_ms());
    return RTC_OK;
}

void rtc_dc_global_cleanup(void) {
    if (usrsctp_finish() != 0)
        RTC_LOG_WARN("SCTP cleanup deferred: associations still draining");
    else
        rtc_mutex_destroy(&dc_stack_mutex);
}

static void dc_dcep(rtc_dc_manager_t *mgr, uint16_t sid, const uint8_t *data, size_t len) {
    if (!len)
        return;
    rtc_data_channel_t *dc = dc_find(mgr, sid);
    if (data[0] == 2 && len == 1) {
        if (dc && dc->locally_created && !dc->negotiated && dc->open_sent)
            dc_open(dc);
        return;
    }
    if (data[0] != 3 || len < 12 || dc || (sid % 2) == (mgr->dtls_client ? 0 : 1) ||
        rtc_u32_map_len(&mgr->channels) >= RTC_DC_MAX_CHANNELS)
        return;
    unsigned type = data[1] & 0x7f;
    size_t label_len = ((size_t)data[8] << 8) | data[9];
    size_t protocol_len = ((size_t)data[10] << 8) | data[11];
    if (type > 2 || label_len >= RTC_DC_MAX_LABEL || protocol_len >= RTC_DC_MAX_LABEL ||
        12 + label_len + protocol_len != len)
        return;
    dc = calloc(1, sizeof(*dc));
    if (!dc)
        return;
    dc->manager = mgr;
    dc->id = sid;
    dc->ordered = !(data[1] & 0x80);
    uint32_t reliability;
    memcpy(&reliability, data + 4, 4);
    reliability = ntohl(reliability);
    if (reliability > 65535) {
        free(dc);
        return;
    }
    dc->max_retransmits = type == 1 ? (int)reliability : -1;
    dc->max_packet_life = type == 2 ? (int)reliability : -1;
    memcpy(dc->label, data + 12, label_len);
    memcpy(dc->protocol, data + 12 + label_len, protocol_len);
    dc->state = RTC_DC_OPEN;
    if (rtc_u32_map_set(&mgr->channels, sid, dc) != RTC_OK) {
        free(dc);
        return;
    }
    uint8_t ack = 2;
    if (dc_transmit(dc, &ack, 1, DC_PPID_DCEP) != RTC_OK) {
        dc_closed(dc);
        return;
    }
    if (mgr->on_channel)
        mgr->on_channel(dc, mgr->on_channel_user);
}

static void dc_process_event(rtc_dc_manager_t *mgr, void *data, size_t len,
                             struct sctp_rcvinfo info, int flags) {
    if (flags & MSG_NOTIFICATION) {
        union sctp_notification *n = data;
        if (len >= sizeof(n->sn_header) && n->sn_header.sn_type == SCTP_ASSOC_CHANGE &&
            len >= sizeof(struct sctp_assoc_change)) {
            if (n->sn_assoc_change.sac_state == SCTP_COMM_UP) {
                mgr->association_up = true;
                dc_each_flush(mgr);
            } else if (n->sn_assoc_change.sac_state == SCTP_COMM_LOST ||
                       n->sn_assoc_change.sac_state == SCTP_SHUTDOWN_COMP) {
                mgr->association_up = false;
                rtc_u32_map_iter_t it = {0};
                uint32_t id;
                void *value;
                while (rtc_u32_map_next(&mgr->channels, &it, &id, &value))
                    dc_closed(value);
            }
        } else if (len >= sizeof(struct sctp_stream_reset_event) &&
                   n->sn_header.sn_type == SCTP_STREAM_RESET_EVENT &&
                   !(n->sn_strreset_event.strreset_flags &
                     (SCTP_STREAM_RESET_DENIED | SCTP_STREAM_RESET_FAILED))) {
            size_t count = (len - sizeof(struct sctp_stream_reset_event)) / sizeof(uint16_t);
            for (size_t i = 0; i < count; i++) {
                rtc_data_channel_t *dc = dc_find(mgr, n->sn_strreset_event.strreset_stream_list[i]);
                if (dc) {
                    if ((n->sn_strreset_event.strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) &&
                        dc->state != RTC_DC_CLOSING)
                        rtc_data_channel_close(dc);
                    if (n->sn_strreset_event.strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN)
                        dc_closed(dc);
                }
            }
        }
    } else {
        uint32_t ppid = ntohl(info.rcv_ppid);
        rtc_data_channel_t *dc = dc_find(mgr, info.rcv_sid);
        if (ppid == DC_PPID_DCEP && (flags & MSG_EOR)) {
            dc_dcep(mgr, info.rcv_sid, data, len);
        } else if (dc && dc->state == RTC_DC_OPEN &&
                   (ppid == DC_PPID_TEXT || ppid == DC_PPID_BINARY || ppid == DC_PPID_EMPTY_TEXT ||
                    ppid == DC_PPID_EMPTY_BINARY)) {
            size_t amount = ppid == DC_PPID_EMPTY_TEXT || ppid == DC_PPID_EMPTY_BINARY ? 0 : len;
            if (dc->incoming_len + amount <= RTC_DC_MAX_MSG_SIZE) {
                uint8_t *buf = realloc(dc->incoming, dc->incoming_len + amount + 1);
                if (buf) {
                    dc->incoming = buf;
                    memcpy(buf + dc->incoming_len, data, amount);
                    dc->incoming_len += amount;
                    if (flags & MSG_EOR) {
                        dc->bytes_received += dc->incoming_len;
                        if (dc->on_message)
                            dc->on_message(buf, dc->incoming_len, dc->on_message_user);
                        dc->incoming_len = 0;
                    }
                } else
                    rtc_data_channel_close(dc);
            } else {
                rtc_data_channel_close(dc);
            }
        }
    }
}

static int dc_receive(struct socket *sock, union sctp_sockstore addr, void *data, size_t len,
                      struct sctp_rcvinfo info, int flags, void *user) {
    (void)sock;
    (void)addr;
    if (!data)
        return 1;
    dc_event_t *event = calloc(1, sizeof(*event));
    if (!event) {
        free(data);
        return 1;
    }
    event->data = data;
    event->len = len;
    event->info = info;
    event->flags = flags;
    dc_queue_event(user, event);
    return 1;
}

static void dc_process_events(rtc_dc_manager_t *mgr) {
    /* Global SCTP timers may run on another worker. Callbacks only enqueue;
     * DTLS I/O and application callbacks are drained on this endpoint's worker. */
    for (unsigned budget = 0; budget < 10000; budget++) {
        rtc_mutex_lock(&dc_stack_mutex);
        dc_event_t *event = mgr->events_head;
        if (event) {
            mgr->events_head = event->next;
            if (!mgr->events_head)
                mgr->events_tail = NULL;
        }
        rtc_mutex_unlock(&dc_stack_mutex);
        if (!event)
            break;
        if (event->outgoing)
            (void)mgr->send_fn(event->data, event->len, mgr->send_user);
        else
            dc_process_event(mgr, event->data, event->len, event->info, event->flags);
        free(event->data);
        free(event);
    }
}
int rtc_dc_manager_init(rtc_dc_manager_t *mgr, rtc_dc_send_fn fn, void *user) {
    if (!mgr || !fn)
        return RTC_ERR_INVALID;
    memset(mgr, 0, sizeof(*mgr));
    if (rtc_u32_map_init(&mgr->channels) != RTC_OK)
        return RTC_ERR_NOMEM;
    mgr->send_fn = fn;
    mgr->send_user = user;
    mgr->remote_port = 5000;
    mgr->max_message_size = RTC_DC_MAX_MSG_SIZE;
    return RTC_OK;
}

void rtc_dc_manager_set_dispatch(rtc_dc_manager_t *mgr, rtc_dc_dispatch_fn fn, void *owner) {
    mgr->dispatch = fn;
    mgr->dispatch_owner = owner;
}

int rtc_dc_manager_connect(rtc_dc_manager_t *mgr, bool client) {
    if (!mgr || mgr->socket)
        return RTC_ERR_INVALID;
    mgr->dtls_client = client;
    mgr->dtls_connected = true;
    /* Stream parity belongs to the DTLS role, not the SDP offerer. */
    rtc_data_channel_t *automatic[RTC_DC_MAX_CHANNELS];
    size_t count = 0;
    rtc_u32_map_iter_t it = {0};
    uint32_t id;
    void *value;
    while (rtc_u32_map_next(&mgr->channels, &it, &id, &value)) {
        rtc_data_channel_t *dc = value;
        if (dc->automatic_id)
            automatic[count++] = dc;
        else if (!dc->negotiated && dc->id % 2 != (client ? 0 : 1))
            return RTC_ERR_INVALID;
    }
    for (size_t i = 0; i < count; i++)
        rtc_u32_map_remove(&mgr->channels, automatic[i]->id);
    mgr->next_id = client ? 0 : 1;
    for (size_t i = 0; i < count; i++) {
        while (dc_find(mgr, (uint16_t)mgr->next_id))
            mgr->next_id += 2;
        automatic[i]->id = (uint16_t)mgr->next_id;
        mgr->next_id += 2;
        if (rtc_u32_map_set(&mgr->channels, automatic[i]->id, automatic[i]) != RTC_OK)
            return RTC_ERR_NOMEM;
    }
    rtc_mutex_lock(&dc_stack_mutex);
    usrsctp_register_address(mgr);
    mgr->socket = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, dc_receive, NULL, 0, mgr);
    if (!mgr->socket)
        goto fail;
    usrsctp_set_non_blocking(mgr->socket, 1);
    int enabled = 1;
    if (usrsctp_setsockopt(mgr->socket, IPPROTO_SCTP, SCTP_NODELAY, &enabled, sizeof(enabled)) < 0)
        goto fail;
    struct sctp_assoc_value reset = {.assoc_id = SCTP_ALL_ASSOC,
                                     .assoc_value = SCTP_ENABLE_RESET_STREAM_REQ};
    if (usrsctp_setsockopt(mgr->socket, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &reset,
                           sizeof(reset)) < 0)
        goto fail;
    struct sctp_initmsg init = {.sinit_num_ostreams = 65535, .sinit_max_instreams = 65535};
    if (usrsctp_setsockopt(mgr->socket, IPPROTO_SCTP, SCTP_INITMSG, &init, sizeof(init)) < 0)
        goto fail;
    uint16_t events[] = {SCTP_ASSOC_CHANGE, SCTP_STREAM_RESET_EVENT};
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        struct sctp_event event = {.se_assoc_id = SCTP_ALL_ASSOC, .se_type = events[i], .se_on = 1};
        if (usrsctp_setsockopt(mgr->socket, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) < 0)
            goto fail;
    }
    struct sockaddr_conn address;
    memset(&address, 0, sizeof(address));
    address.sconn_family = AF_CONN;
#ifdef __APPLE__
    address.sconn_len = sizeof(address);
#endif
    address.sconn_port = htons(5000);
    address.sconn_addr = mgr;
    if (usrsctp_bind(mgr->socket, (struct sockaddr *)&address, sizeof(address)) < 0)
        goto fail;
    address.sconn_port = htons(mgr->remote_port);
    if (usrsctp_connect(mgr->socket, (struct sockaddr *)&address, sizeof(address)) < 0 &&
        errno != EINPROGRESS)
        goto fail;
    rtc_mutex_unlock(&dc_stack_mutex);
    return RTC_OK;
fail:
    if (mgr->socket)
        usrsctp_close(mgr->socket);
    mgr->socket = NULL;
    usrsctp_deregister_address(mgr);
    mgr->dtls_connected = false;
    rtc_mutex_unlock(&dc_stack_mutex);
    return RTC_ERR_GENERIC;
}

int rtc_dc_manager_on_dtls_connected(rtc_dc_manager_t *mgr) {
    return rtc_dc_manager_connect(mgr, true);
}

void rtc_dc_manager_tick(rtc_dc_manager_t *mgr, uint32_t elapsed_ms) {
    (void)elapsed_ms;
    uint_fast64_t now = rtc_time_ms();
    uint_fast64_t before = atomic_load(&dc_timer_time);
    rtc_mutex_lock(&dc_stack_mutex);
    if (now > before && atomic_compare_exchange_strong(&dc_timer_time, &before, now))
        usrsctp_handle_timers((uint32_t)(now - before));
    rtc_mutex_unlock(&dc_stack_mutex);
    dc_process_events(mgr);
    dc_each_flush(mgr);
    dc_process_events(mgr);
}

int rtc_dc_manager_recv(rtc_dc_manager_t *mgr, const uint8_t *data, size_t len) {
    if (!mgr || !mgr->socket || !data || len < 12)
        return RTC_ERR_INVALID;
    rtc_mutex_lock(&dc_stack_mutex);
    usrsctp_conninput(mgr, data, len, 0);
    rtc_mutex_unlock(&dc_stack_mutex);
    dc_process_events(mgr);
    dc_each_flush(mgr);
    dc_process_events(mgr);
    return RTC_OK;
}

rtc_data_channel_t *rtc_dc_manager_create_channel(rtc_dc_manager_t *mgr, const char *label,
                                                  const rtc_data_channel_init_t *opts) {
    rtc_data_channel_init_t defaults = {
        .ordered = true, .max_retransmits = -1, .max_packet_life = -1, .id = -1};
    if (!opts)
        opts = &defaults;
    if (!mgr || !mgr->send_fn || !label || strlen(label) >= RTC_DC_MAX_LABEL ||
        (opts->protocol && strlen(opts->protocol) >= RTC_DC_MAX_LABEL) ||
        opts->max_retransmits < -1 || opts->max_retransmits > 65535 || opts->max_packet_life < -1 ||
        opts->max_packet_life > 65535 ||
        (opts->max_retransmits >= 0 && opts->max_packet_life >= 0) || opts->id < -1 ||
        opts->id > 65534 || (opts->negotiated && opts->id < 0) ||
        rtc_u32_map_len(&mgr->channels) >= RTC_DC_MAX_CHANNELS)
        return NULL;
    int id = opts->id;
    if (id < 0) {
        id = mgr->next_id;
        while (id < 65535 && dc_find(mgr, (uint16_t)id))
            id += 2;
    }
    if (id >= 65535 || dc_find(mgr, (uint16_t)id) ||
        (mgr->dtls_connected && !opts->negotiated && id % 2 != (mgr->dtls_client ? 0 : 1)))
        return NULL;
    rtc_data_channel_t *dc = calloc(1, sizeof(*dc));
    if (!dc)
        return NULL;
    strcpy(dc->label, label);
    if (opts->protocol)
        strcpy(dc->protocol, opts->protocol);
    dc->id = (uint16_t)id;
    dc->automatic_id = opts->id < 0;
    dc->ordered = opts->ordered;
    dc->negotiated = opts->negotiated;
    dc->max_retransmits = opts->max_retransmits;
    dc->max_packet_life = opts->max_packet_life;
    dc->locally_created = true;
    dc->manager = mgr;
    if (rtc_u32_map_set(&mgr->channels, dc->id, dc) != RTC_OK) {
        free(dc);
        return NULL;
    }
    mgr->next_id = id + 2;
    if (mgr->association_up)
        dc_flush(dc);
    return dc;
}

typedef struct {
    rtc_data_channel_t *dc;
    const uint8_t *data;
    size_t len;
    uint32_t ppid;
    int result;
} dc_send_call_t;

static void dc_send_call(void *arg) {
    dc_send_call_t *call = arg;
    rtc_data_channel_t *dc = call->dc;
    if (dc->state != RTC_DC_OPEN || call->len > dc->manager->max_message_size ||
        (!call->data && call->len)) {
        call->result = RTC_ERR_INVALID;
        return;
    }
    if (dc->buffered_amount + call->len > DC_QUEUE_LIMIT) {
        call->result = RTC_ERR_NOMEM;
        return;
    }
    int rc = dc->head ? RTC_ERR_NOMEM : dc_transmit(dc, call->data, call->len, call->ppid);
    if (rc == RTC_ERR_NOMEM) {
        dc_message_t *msg = malloc(sizeof(*msg) + call->len);
        if (!msg) {
            call->result = RTC_ERR_NOMEM;
            return;
        }
        msg->next = NULL;
        msg->len = call->len;
        msg->ppid = call->ppid;
        if (call->len)
            memcpy(msg->data, call->data, call->len);
        if (dc->tail)
            dc->tail->next = msg;
        else
            dc->head = msg;
        dc->tail = msg;
        dc->buffered_amount += call->len;
        rc = RTC_OK;
    }
    if (rc == RTC_OK)
        dc->bytes_sent += call->len;
    call->result = rc;
}

static int dc_send(rtc_data_channel_t *dc, const uint8_t *data, size_t len, uint32_t ppid) {
    if (!dc)
        return RTC_ERR_INVALID;
    dc_send_call_t call = {.dc = dc, .data = data, .len = len, .ppid = ppid};
    int rc = dc_dispatch(dc->manager, dc_send_call, &call);
    return rc == RTC_OK ? call.result : rc;
}

int rtc_data_channel_send(rtc_data_channel_t *dc, const uint8_t *data, size_t len) {
    return dc_send(dc, data, len, len ? DC_PPID_BINARY : DC_PPID_EMPTY_BINARY);
}

int rtc_data_channel_send_text(rtc_data_channel_t *dc, const char *text) {
    if (!text)
        return RTC_ERR_INVALID;
    size_t len = strlen(text);
    return dc_send(dc, (const uint8_t *)text, len, len ? DC_PPID_TEXT : DC_PPID_EMPTY_TEXT);
}

static void dc_request_reset(rtc_data_channel_t *dc) {
    if (dc->reset_sent || !dc->manager->association_up)
        return;
    size_t size = sizeof(struct sctp_reset_streams) + sizeof(uint16_t);
    struct sctp_reset_streams *request = calloc(1, size);
    if (!request)
        return;
    request->srs_assoc_id = SCTP_ALL_ASSOC;
    request->srs_flags = SCTP_STREAM_RESET_OUTGOING;
    request->srs_number_streams = 1;
    request->srs_stream_list[0] = dc->id;
    rtc_mutex_lock(&dc_stack_mutex);
    if (usrsctp_setsockopt(dc->manager->socket, IPPROTO_SCTP, SCTP_RESET_STREAMS, request,
                           (socklen_t)size) == 0)
        dc->reset_sent = true;
    rtc_mutex_unlock(&dc_stack_mutex);
    free(request);
}

static void dc_close_call(void *arg) {
    rtc_data_channel_t *dc = arg;
    if (dc->state == RTC_DC_CLOSED || dc->state == RTC_DC_CLOSING)
        return;
    if (!dc->manager->association_up) {
        dc_closed(dc);
        return;
    }
    dc->state = RTC_DC_CLOSING;
    dc_flush(dc);
}

void rtc_data_channel_close(rtc_data_channel_t *dc) {
    if (dc)
        (void)dc_dispatch(dc->manager, dc_close_call, dc);
}

void rtc_dc_manager_on_channel(rtc_dc_manager_t *mgr, rtc_dc_on_channel_fn fn, void *user) {
    mgr->on_channel = fn;
    mgr->on_channel_user = user;
}

void rtc_dc_manager_close(rtc_dc_manager_t *mgr) {
    if (!mgr || !mgr->send_fn)
        return;
    rtc_mutex_lock(&dc_stack_mutex);
    if (mgr->socket) {
        struct linger linger = {.l_onoff = 1, .l_linger = 0};
        usrsctp_setsockopt(mgr->socket, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
        usrsctp_close(mgr->socket);
        mgr->socket = NULL;
        usrsctp_deregister_address(mgr);
    }
    dc_event_t *event = mgr->events_head;
    mgr->events_head = mgr->events_tail = NULL;
    rtc_mutex_unlock(&dc_stack_mutex);
    while (event) {
        dc_event_t *next = event->next;
        if (event->outgoing)
            (void)mgr->send_fn(event->data, event->len, mgr->send_user);
        free(event->data);
        free(event);
        event = next;
    }
    rtc_u32_map_iter_t it = {0};
    uint32_t id;
    void *value;
    while (rtc_u32_map_next(&mgr->channels, &it, &id, &value)) {
        rtc_data_channel_t *dc = value;
        dc_closed(dc);
        while (dc->head) {
            dc_message_t *msg = dc->head;
            dc->head = msg->next;
            free(msg);
        }
        free(dc->incoming);
        free(dc);
    }
    rtc_u32_map_free(&mgr->channels);
    memset(mgr, 0, sizeof(*mgr));
}

typedef struct {
    rtc_data_channel_t *dc;
    int action;
    rtc_on_dc_open_fn open;
    rtc_on_dc_close_fn close;
    rtc_on_dc_message_fn message;
    rtc_on_dc_buffered_amount_low_fn low;
    void *user;
} dc_callback_call_t;

static void dc_set_callback(void *arg) {
    dc_callback_call_t *call = arg;
    rtc_data_channel_t *dc = call->dc;
    switch (call->action) {
        case 0:
            dc->on_open = call->open;
            dc->on_open_user = call->user;
            break;
        case 1:
            dc->on_close = call->close;
            dc->on_close_user = call->user;
            break;
        case 2:
            dc->on_message = call->message;
            dc->on_message_user = call->user;
            break;
        case 3:
            dc->on_buffered_low = call->low;
            dc->on_buffered_low_user = call->user;
            break;
    }
}
void rtc_data_channel_on_open(rtc_data_channel_t *dc, rtc_on_dc_open_fn fn, void *user) {
    dc_callback_call_t call = {.dc = dc, .action = 0, .open = fn, .user = user};
    if (dc)
        (void)dc_dispatch(dc->manager, dc_set_callback, &call);
}
void rtc_data_channel_on_close(rtc_data_channel_t *dc, rtc_on_dc_close_fn fn, void *user) {
    dc_callback_call_t call = {.dc = dc, .action = 1, .close = fn, .user = user};
    if (dc)
        (void)dc_dispatch(dc->manager, dc_set_callback, &call);
}
void rtc_data_channel_on_message(rtc_data_channel_t *dc, rtc_on_dc_message_fn fn, void *user) {
    dc_callback_call_t call = {.dc = dc, .action = 2, .message = fn, .user = user};
    if (dc)
        (void)dc_dispatch(dc->manager, dc_set_callback, &call);
}
const char *rtc_data_channel_label(const rtc_data_channel_t *dc) {
    return dc ? dc->label : "";
}
uint16_t rtc_data_channel_id(const rtc_data_channel_t *dc) {
    return dc ? dc->id : 0;
}
rtc_data_channel_state_t rtc_data_channel_state(const rtc_data_channel_t *dc) {
    return dc ? dc->state : RTC_DC_CLOSED;
}
uint64_t rtc_data_channel_buffered_amount(const rtc_data_channel_t *dc) {
    return dc ? dc->buffered_amount : 0;
}
uint64_t rtc_data_channel_buffered_amount_low_threshold(const rtc_data_channel_t *dc) {
    return dc ? dc->buffered_amount_low_thresh : 0;
}
void rtc_data_channel_set_buffered_amount_low_threshold(rtc_data_channel_t *dc, uint64_t value) {
    if (dc)
        dc->buffered_amount_low_thresh = value;
}
void rtc_data_channel_on_buffered_amount_low(rtc_data_channel_t *dc,
                                             rtc_on_dc_buffered_amount_low_fn fn, void *user) {
    dc_callback_call_t call = {.dc = dc, .action = 3, .low = fn, .user = user};
    if (dc)
        (void)dc_dispatch(dc->manager, dc_set_callback, &call);
}
uint64_t rtc_data_channel_bytes_sent(const rtc_data_channel_t *dc) {
    return dc ? dc->bytes_sent : 0;
}
uint64_t rtc_data_channel_bytes_received(const rtc_data_channel_t *dc) {
    return dc ? dc->bytes_received : 0;
}

rtc_sctp_t *rtc_sctp_create(rtc_sctp_send_fn send, void *user) {
    rtc_sctp_t *sctp = calloc(1, sizeof(*sctp));
    if (sctp && rtc_dc_manager_init(sctp, send, user) != RTC_OK) {
        free(sctp);
        return NULL;
    }
    return sctp;
}
void rtc_sctp_destroy(rtc_sctp_t *sctp) {
    rtc_dc_manager_close(sctp);
    free(sctp);
}
int rtc_sctp_connect(rtc_sctp_t *sctp, bool client) {
    return rtc_dc_manager_connect(sctp, client);
}
int rtc_sctp_receive(rtc_sctp_t *sctp, const uint8_t *data, size_t len) {
    return rtc_dc_manager_recv(sctp, data, len);
}
void rtc_sctp_tick(rtc_sctp_t *sctp) {
    rtc_dc_manager_tick(sctp, 0);
}
void rtc_sctp_on_channel(rtc_sctp_t *sctp, rtc_sctp_on_channel_fn fn, void *user) {
    rtc_dc_manager_on_channel(sctp, fn, user);
}
rtc_data_channel_t *rtc_sctp_create_channel(rtc_sctp_t *sctp, const char *label,
                                            const rtc_data_channel_init_t *options) {
    return rtc_dc_manager_create_channel(sctp, label, options);
}
