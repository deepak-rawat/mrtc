/*
 * rtc_data_channel.c - Lightweight data channel over DTLS.
 *
 * Wire format for each message:
 *   [1 byte: channel_id] [1 byte: msg_type] [2 bytes: length BE] [payload]
 *
 * Channel open handshake:
 *   Creator  → Remote: OPEN message with label in payload
 *   Remote   → Creator: ACK message
 *   Both sides transition to OPEN state.
 */
#include "rtc/rtc_data_channel.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct rtc_data_channel {
    char label[RTC_DC_MAX_LABEL];
    uint16_t id;
    rtc_data_channel_state_t state;
    bool locally_created;

    /* Callbacks */
    rtc_on_dc_open_fn on_open;
    void *on_open_user;
    rtc_on_dc_close_fn on_close;
    void *on_close_user;
    rtc_on_dc_message_fn on_message;
    void *on_message_user;

    /* Back-pointer to manager for sending */
    rtc_dc_manager_t *manager;
};

/* ---------- Internal helpers ---------- */

static int dc_send_message(rtc_data_channel_t *dc, uint8_t msg_type, const uint8_t *payload,
                           size_t len) {
    if (!dc || !dc->manager || !dc->manager->send_fn)
        return RTC_ERR_INVALID;
    if (len > RTC_DC_MAX_MSG_SIZE)
        return RTC_ERR_INVALID;

    uint8_t header[RTC_DC_HEADER_SIZE];
    header[0] = (uint8_t)(dc->id & 0xFF);
    header[1] = msg_type;
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)(len & 0xFF);

    /* Send header + payload as one message.
     * For simplicity, copy into a single buffer. */
    size_t total = RTC_DC_HEADER_SIZE + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return RTC_ERR_NOMEM;

    memcpy(buf, header, RTC_DC_HEADER_SIZE);
    if (len > 0 && payload)
        memcpy(buf + RTC_DC_HEADER_SIZE, payload, len);

    int rc = dc->manager->send_fn(buf, total, dc->manager->send_user);
    free(buf);
    return rc;
}

static rtc_data_channel_t *dc_find_by_id(rtc_dc_manager_t *mgr, uint16_t id) {
    return (rtc_data_channel_t *)rtc_u32_map_get(&mgr->channels, (uint32_t)id);
}

/* ---------- Data Channel API ---------- */

int rtc_data_channel_send(rtc_data_channel_t *dc, const uint8_t *data, size_t len) {
    if (!dc)
        return RTC_ERR_INVALID;
    if (dc->state != RTC_DC_OPEN)
        return RTC_ERR_INVALID;
    return dc_send_message(dc, RTC_DC_MSG_DATA, data, len);
}

int rtc_data_channel_send_text(rtc_data_channel_t *dc, const char *text) {
    if (!dc || !text)
        return RTC_ERR_INVALID;
    return rtc_data_channel_send(dc, (const uint8_t *)text, strlen(text));
}

void rtc_data_channel_close(rtc_data_channel_t *dc) {
    if (!dc)
        return;
    if (dc->state == RTC_DC_CLOSED)
        return;

    /* Send CLOSE message if connected */
    if (dc->state == RTC_DC_OPEN) {
        dc->state = RTC_DC_CLOSING;
        dc_send_message(dc, RTC_DC_MSG_CLOSE, NULL, 0);
    }

    dc->state = RTC_DC_CLOSED;
    if (dc->on_close)
        dc->on_close(dc->on_close_user);
}

void rtc_data_channel_on_open(rtc_data_channel_t *dc, rtc_on_dc_open_fn fn, void *user) {
    if (!dc)
        return;
    dc->on_open = fn;
    dc->on_open_user = user;
}

void rtc_data_channel_on_close(rtc_data_channel_t *dc, rtc_on_dc_close_fn fn, void *user) {
    if (!dc)
        return;
    dc->on_close = fn;
    dc->on_close_user = user;
}

void rtc_data_channel_on_message(rtc_data_channel_t *dc, rtc_on_dc_message_fn fn, void *user) {
    if (!dc)
        return;
    dc->on_message = fn;
    dc->on_message_user = user;
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

/* ---------- Data Channel Manager ---------- */

int rtc_dc_manager_init(rtc_dc_manager_t *mgr, rtc_dc_send_fn send_fn, void *send_user) {
    if (!mgr)
        return RTC_ERR_INVALID;
    memset(mgr, 0, sizeof(*mgr));
    if (rtc_u32_map_init(&mgr->channels) != RTC_OK)
        return RTC_ERR_NOMEM;
    mgr->send_fn = send_fn;
    mgr->send_user = send_user;
    mgr->next_id = 0; /* even IDs for offerer, odd for answerer (simplified) */
    return RTC_OK;
}

rtc_data_channel_t *rtc_dc_manager_create_channel(rtc_dc_manager_t *mgr, const char *label,
                                                  const rtc_data_channel_init_t *opts) {
    if (!mgr || rtc_u32_map_len(&mgr->channels) >= RTC_DC_MAX_CHANNELS)
        return NULL;

    rtc_data_channel_t *dc = (rtc_data_channel_t *)calloc(1, sizeof(*dc));
    if (!dc)
        return NULL;

    if (label) {
        size_t llen = strlen(label);
        if (llen >= RTC_DC_MAX_LABEL)
            llen = RTC_DC_MAX_LABEL - 1;
        memcpy(dc->label, label, llen);
        dc->label[llen] = '\0';
    }

    if (opts && opts->id >= 0) {
        dc->id = (uint16_t)opts->id;
    } else {
        dc->id = (uint16_t)mgr->next_id;
        mgr->next_id += 2; /* skip by 2 to leave room for remote channels */
    }

    dc->state = RTC_DC_CONNECTING;
    dc->locally_created = true;
    dc->manager = mgr;

    if (rtc_u32_map_set(&mgr->channels, (uint32_t)dc->id, dc) != RTC_OK) {
        free(dc);
        return NULL;
    }

    RTC_LOG_INFO("Data channel created: label=\"%s\" id=%u", dc->label, dc->id);

    /* If DTLS is already connected, send OPEN immediately */
    if (mgr->dtls_connected) {
        dc_send_message(dc, RTC_DC_MSG_OPEN, (const uint8_t *)dc->label, strlen(dc->label));
    }

    return dc;
}

void rtc_dc_manager_on_channel(rtc_dc_manager_t *mgr, rtc_dc_on_channel_fn fn, void *user) {
    if (!mgr)
        return;
    mgr->on_channel = fn;
    mgr->on_channel_user = user;
}

int rtc_dc_manager_on_dtls_connected(rtc_dc_manager_t *mgr) {
    if (!mgr)
        return RTC_ERR_INVALID;
    mgr->dtls_connected = true;

    /* Send OPEN for all pending locally-created channels */
    rtc_u32_map_iter_t it = {0};
    uint32_t id;
    void *val;
    while (rtc_u32_map_next(&mgr->channels, &it, &id, &val)) {
        rtc_data_channel_t *dc = (rtc_data_channel_t *)val;
        if (dc && dc->locally_created && dc->state == RTC_DC_CONNECTING) {
            dc_send_message(dc, RTC_DC_MSG_OPEN, (const uint8_t *)dc->label, strlen(dc->label));
        }
    }

    return RTC_OK;
}

int rtc_dc_manager_recv(rtc_dc_manager_t *mgr, const uint8_t *data, size_t len) {
    if (!mgr || !data)
        return RTC_ERR_INVALID;
    if (len < RTC_DC_HEADER_SIZE)
        return RTC_ERR_INVALID;

    uint16_t channel_id = data[0];
    uint8_t msg_type = data[1];
    uint16_t payload_len = (uint16_t)((uint16_t)data[2] << 8 | data[3]);

    if (RTC_DC_HEADER_SIZE + payload_len > len)
        return RTC_ERR_INVALID;

    const uint8_t *payload = data + RTC_DC_HEADER_SIZE;

    switch (msg_type) {
        case RTC_DC_MSG_OPEN: {
            /* Remote is opening a new channel */
            rtc_data_channel_t *dc = dc_find_by_id(mgr, channel_id);
            if (dc) {
                /* Channel already exists (negotiated), just transition */
                dc->state = RTC_DC_OPEN;
                /* Send ACK */
                dc_send_message(dc, RTC_DC_MSG_ACK, NULL, 0);
                if (dc->on_open)
                    dc->on_open(dc->on_open_user);
            } else {
                /* Create new channel for remote peer */
                if (rtc_u32_map_len(&mgr->channels) >= RTC_DC_MAX_CHANNELS)
                    return RTC_ERR_NOMEM;

                dc = (rtc_data_channel_t *)calloc(1, sizeof(*dc));
                if (!dc)
                    return RTC_ERR_NOMEM;

                dc->id = channel_id;
                dc->locally_created = false;
                dc->manager = mgr;
                dc->state = RTC_DC_OPEN;

                /* Extract label from payload */
                if (payload_len > 0) {
                    size_t llen = payload_len;
                    if (llen >= RTC_DC_MAX_LABEL)
                        llen = RTC_DC_MAX_LABEL - 1;
                    memcpy(dc->label, payload, llen);
                    dc->label[llen] = '\0';
                }

                if (rtc_u32_map_set(&mgr->channels, (uint32_t)dc->id, dc) != RTC_OK) {
                    free(dc);
                    return RTC_ERR_NOMEM;
                }

                /* Send ACK */
                dc_send_message(dc, RTC_DC_MSG_ACK, NULL, 0);

                RTC_LOG_INFO("Remote data channel opened: label=\"%s\" id=%u", dc->label, dc->id);

                /* Notify application */
                if (mgr->on_channel)
                    mgr->on_channel(dc, mgr->on_channel_user);
            }
            break;
        }

        case RTC_DC_MSG_ACK: {
            rtc_data_channel_t *dc = dc_find_by_id(mgr, channel_id);
            if (dc && dc->state == RTC_DC_CONNECTING) {
                dc->state = RTC_DC_OPEN;
                RTC_LOG_INFO("Data channel ACK received: label=\"%s\" id=%u", dc->label, dc->id);
                if (dc->on_open)
                    dc->on_open(dc->on_open_user);
            }
            break;
        }

        case RTC_DC_MSG_DATA: {
            rtc_data_channel_t *dc = dc_find_by_id(mgr, channel_id);
            if (dc && dc->state == RTC_DC_OPEN && dc->on_message) {
                dc->on_message(payload, payload_len, dc->on_message_user);
            }
            break;
        }

        case RTC_DC_MSG_CLOSE: {
            rtc_data_channel_t *dc = dc_find_by_id(mgr, channel_id);
            if (dc && dc->state != RTC_DC_CLOSED) {
                dc->state = RTC_DC_CLOSED;
                RTC_LOG_INFO("Data channel closed by remote: label=\"%s\" id=%u", dc->label,
                             dc->id);
                if (dc->on_close)
                    dc->on_close(dc->on_close_user);
            }
            break;
        }

        default:
            RTC_LOG_WARN("Unknown data channel message type: %u", msg_type);
            break;
    }

    return RTC_OK;
}

void rtc_dc_manager_close(rtc_dc_manager_t *mgr) {
    if (!mgr)
        return;
    rtc_u32_map_iter_t it = {0};
    uint32_t id;
    void *val;
    while (rtc_u32_map_next(&mgr->channels, &it, &id, &val)) {
        rtc_data_channel_t *dc = (rtc_data_channel_t *)val;
        if (dc) {
            dc->state = RTC_DC_CLOSED;
            free(dc);
        }
    }
    rtc_u32_map_free(&mgr->channels);
    mgr->dtls_connected = false;
}
