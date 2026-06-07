/*
 * rtc_transport.c - Logical endpoint transport skeleton.
 */
#include "rtc_transport_internal.h"

#include "rtc_listener_internal.h"
#include "rtc_sdp.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct rtc_transport {
    rtc_router_t *router;
    rtc_listener_t *listener;
    char ice_ufrag[ICE_UFRAG_LEN];
    char ice_pwd[ICE_PWD_LEN];
    rtc_ice_mode_t ice_mode;
    bool enable_sctp;
    bool enable_twcc;
    uint32_t initial_outgoing_bitrate_bps;
    _Atomic bool closed;
    _Atomic uint64_t packets_received;
    _Atomic uint64_t bytes_received;
};

static void transport_on_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                const rtc_addr_t *from, void *user) {
    (void)type;
    (void)data;
    (void)from;
    rtc_transport_t *transport = (rtc_transport_t *)user;
    if (atomic_load_explicit(&transport->closed, memory_order_acquire))
        return;
    atomic_fetch_add_explicit(&transport->packets_received, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&transport->bytes_received, (uint64_t)len, memory_order_relaxed);
}

static void transport_copy_ice_parameters(rtc_transport_t *transport,
                                          rtc_ice_parameters_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->username_fragment, transport->ice_ufrag, sizeof(out->username_fragment));
    memcpy(out->password, transport->ice_pwd, sizeof(out->password));
    out->mode = transport->ice_mode;
}

rtc_transport_t *rtc_transport_create_internal(rtc_router_t *router,
                                               const rtc_transport_config_t *cfg) {
    if (!router || !cfg || !cfg->listener)
        return NULL;

    rtc_transport_t *transport = (rtc_transport_t *)calloc(1, sizeof(*transport));
    if (!transport)
        return NULL;

    transport->router = router;
    transport->listener = cfg->listener;
    transport->ice_mode = cfg->ice_mode;
    transport->enable_sctp = cfg->enable_sctp;
    transport->enable_twcc = cfg->enable_twcc;
    transport->initial_outgoing_bitrate_bps = cfg->initial_outgoing_bitrate_bps;
    if (transport->ice_mode != RTC_ICE_MODE_FULL && transport->ice_mode != RTC_ICE_MODE_LITE)
        transport->ice_mode = RTC_ICE_MODE_LITE;

    if (rtc_random_string(transport->ice_ufrag, sizeof(transport->ice_ufrag)) != RTC_OK ||
        rtc_random_string(transport->ice_pwd, sizeof(transport->ice_pwd)) != RTC_OK) {
        free(transport);
        return NULL;
    }

    int rc = rtc_listener_register_ufrag(transport->listener, transport->ice_ufrag,
                                         transport_on_packet, transport);
    if (rc != RTC_OK) {
        free(transport);
        return NULL;
    }
    return transport;
}

int rtc_transport_get_ice_parameters(rtc_transport_t *transport, rtc_ice_parameters_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    transport_copy_ice_parameters(transport, out);
    return RTC_OK;
}

int rtc_transport_get_stats(rtc_transport_t *transport, rtc_transport_stats_t *out) {
    if (!transport || !out)
        return RTC_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->closed = atomic_load_explicit(&transport->closed, memory_order_acquire);
    out->ice_mode = transport->ice_mode;
    out->packets_received =
        atomic_load_explicit(&transport->packets_received, memory_order_relaxed);
    out->bytes_received = atomic_load_explicit(&transport->bytes_received, memory_order_relaxed);
    return RTC_OK;
}

void rtc_transport_close(rtc_transport_t *transport) {
    if (!transport)
        return;
    bool was_closed = atomic_exchange_explicit(&transport->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;
    if (transport->listener)
        rtc_listener_unregister_ufrag(transport->listener, transport->ice_ufrag);
}

void rtc_transport_destroy(rtc_transport_t *transport) {
    if (!transport)
        return;
    rtc_transport_close(transport);
    free(transport);
}