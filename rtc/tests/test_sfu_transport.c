/*
 * test_sfu_transport.c - Logical SFU transport skeleton tests.
 */
#include <rtc/rtc.h>

#include "rtc_dtls.h"
#include "rtc_rtp.h"
#include "rtc_srtp.h"
#include "rtc_stun.h"
#include "rtc_transport_internal.h"
#include "test_harness.h"

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

static int listener_loopback_addr(rtc_listener_t *listener, rtc_addr_t *out) {
    rtc_addr_t local;
    int rc = rtc_listener_get_local_addr(listener, &local);
    if (rc != RTC_OK)
        return rc;
    uint16_t port = ntohs(((struct sockaddr_in *)&local.addr)->sin_port);
    return rtc_addr_from_string(out, "127.0.0.1", port);
}

static rtc_socket_t make_bound_sender(void) {
    rtc_socket_t sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == RTC_INVALID_SOCKET)
        return RTC_INVALID_SOCKET;

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    if (bind(sender, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        rtc_close_socket(sender);
        return RTC_INVALID_SOCKET;
    }
    if (rtc_set_nonblocking(sender) != RTC_OK) {
        rtc_close_socket(sender);
        return RTC_INVALID_SOCKET;
    }
    return sender;
}

static int send_udp_from(rtc_listener_t *listener, const uint8_t *data, size_t len,
                         rtc_socket_t sender) {
    rtc_addr_t dest;
    int rc = listener_loopback_addr(listener, &dest);
    if (rc != RTC_OK)
        return rc;
    int sent = sendto(sender, (const char *)data, (int)len, 0, (const struct sockaddr *)&dest.addr,
                      dest.len);
    return sent == (int)len ? RTC_OK : RTC_ERR_SOCKET;
}

typedef struct {
    rtc_listener_t *listener;
    rtc_socket_t sender;
} dtls_client_send_ctx_t;

static bool wait_for_transport_packets(rtc_transport_t *transport, uint64_t target, int timeout_ms);

static int dtls_client_send(const uint8_t *data, size_t len, void *user) {
    dtls_client_send_ctx_t *ctx = (dtls_client_send_ctx_t *)user;
    return send_udp_from(ctx->listener, data, len, ctx->sender);
}

static int send_udp(rtc_listener_t *listener, const uint8_t *data, size_t len) {
    rtc_socket_t sender = make_bound_sender();
    if (sender == RTC_INVALID_SOCKET)
        return RTC_ERR_SOCKET;
    int rc = send_udp_from(listener, data, len, sender);
    rtc_close_socket(sender);
    return rc;
}

static bool recv_stun_response(rtc_socket_t sender, uint8_t *buf, size_t buf_len, size_t *out_len,
                               int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        int n = recv(sender, (char *)buf, (int)buf_len, 0);
        if (n > 0) {
            *out_len = (size_t)n;
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

static bool recv_udp_packet(rtc_socket_t sender, uint8_t *buf, size_t buf_len, size_t *out_len,
                            int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        int n = recv(sender, (char *)buf, (int)buf_len, 0);
        if (n > 0) {
            *out_len = (size_t)n;
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

static bool select_transport_tuple(rtc_listener_t *listener, rtc_transport_t *transport,
                                   rtc_socket_t sender, const char *ufrag) {
    char username[64];
    snprintf(username, sizeof(username), "%s:remote", ufrag);
    rtc_stun_msg_t req;
    if (rtc_stun_build_binding_request(&req, username, NULL, 1234, true, 99, true) != RTC_OK)
        return false;
    if (send_udp_from(listener, req.buf, req.buf_len, sender) != RTC_OK)
        return false;
    if (!wait_for_transport_packets(transport, 1, 1000))
        return false;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    if (!recv_stun_response(sender, resp_buf, sizeof(resp_buf), &resp_len, 1000))
        return false;
    rtc_stun_msg_t resp;
    return rtc_stun_parse(&resp, resp_buf, resp_len) == RTC_OK &&
           resp.type == STUN_BINDING_RESPONSE;
}

static bool wait_for_dtls_connected(rtc_transport_t *transport, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED && stats.srtp_ready) {
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

static bool drive_dtls_handshake(rtc_listener_t *listener, rtc_transport_t *transport,
                                 rtc_socket_t sender, rtc_dtls_transport_t *client) {
    dtls_client_send_ctx_t send_ctx = {.listener = listener, .sender = sender};
    if (rtc_dtls_init(client, RTC_DTLS_ROLE_CLIENT, dtls_client_send, &send_ctx) != RTC_OK)
        return false;
    if (rtc_dtls_handshake(client) != RTC_OK)
        return false;

    uint8_t buf[4096];
    uint64_t deadline = rtc_time_ms() + 3000;
    while (rtc_time_ms() < deadline && client->state != RTC_DTLS_STATE_CONNECTED) {
        int n = recv(sender, (char *)buf, sizeof(buf), 0);
        if (n > 0 && rtc_dtls_recv(client, buf, (size_t)n) != RTC_OK)
            return false;
        if (wait_for_dtls_connected(transport, 1) && client->state == RTC_DTLS_STATE_CONNECTED)
            break;
        SLEEP_MS(10);
    }
    return client->state == RTC_DTLS_STATE_CONNECTED && wait_for_dtls_connected(transport, 1000);
}

static bool wait_for_producer_packets(rtc_producer_t *producer, uint64_t target, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_producer_stats_t stats;
        if (rtc_producer_get_stats(producer, &stats) == RTC_OK &&
            stats.packets_received >= target) {
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_consumer_packets(rtc_consumer_t *consumer, uint64_t target, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_consumer_stats_t stats;
        if (rtc_consumer_get_stats(consumer, &stats) == RTC_OK && stats.packets_sent >= target)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static rtc_mutex_t g_raw_rtp_mutex;
static rtc_cond_t g_raw_rtp_cond;
static int g_raw_rtp_count;
static uint32_t g_raw_rtp_ssrc;
static size_t g_raw_rtp_payload_len;
static int g_raw_rtcp_count;
static uint8_t g_raw_rtcp_pt;
static int g_app_data_count;
static uint8_t g_app_data[64];
static size_t g_app_data_len;

static void raw_rtp_callback(const rtc_rtp_packet_t *pkt, void *user) {
    (void)user;
    rtc_mutex_lock(&g_raw_rtp_mutex);
    g_raw_rtp_count++;
    g_raw_rtp_ssrc = pkt->header.ssrc;
    g_raw_rtp_payload_len = pkt->payload_len;
    rtc_cond_signal(&g_raw_rtp_cond);
    rtc_mutex_unlock(&g_raw_rtp_mutex);
}

static void reset_raw_rtp(void) {
    rtc_mutex_lock(&g_raw_rtp_mutex);
    g_raw_rtp_count = 0;
    g_raw_rtp_ssrc = 0;
    g_raw_rtp_payload_len = 0;
    rtc_mutex_unlock(&g_raw_rtp_mutex);
}

static bool wait_for_raw_rtp(int target, int timeout_ms) {
    rtc_mutex_lock(&g_raw_rtp_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_raw_rtp_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_raw_rtp_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_raw_rtp_cond, &g_raw_rtp_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_raw_rtp_mutex);
    return true;
}

static void raw_rtcp_callback(const uint8_t *data, size_t len, void *user) {
    (void)user;
    rtc_mutex_lock(&g_raw_rtp_mutex);
    g_raw_rtcp_count++;
    g_raw_rtcp_pt = len >= 2 ? data[1] : 0;
    rtc_cond_signal(&g_raw_rtp_cond);
    rtc_mutex_unlock(&g_raw_rtp_mutex);
}

static void app_data_callback(const uint8_t *data, size_t len, void *user) {
    (void)user;
    rtc_mutex_lock(&g_raw_rtp_mutex);
    if (len > sizeof(g_app_data))
        len = sizeof(g_app_data);
    memcpy(g_app_data, data, len);
    g_app_data_len = len;
    g_app_data_count++;
    rtc_cond_signal(&g_raw_rtp_cond);
    rtc_mutex_unlock(&g_raw_rtp_mutex);
}

static void reset_raw_rtcp(void) {
    rtc_mutex_lock(&g_raw_rtp_mutex);
    g_raw_rtcp_count = 0;
    g_raw_rtcp_pt = 0;
    rtc_mutex_unlock(&g_raw_rtp_mutex);
}

static void reset_app_data(void) {
    rtc_mutex_lock(&g_raw_rtp_mutex);
    g_app_data_count = 0;
    g_app_data_len = 0;
    memset(g_app_data, 0, sizeof(g_app_data));
    rtc_mutex_unlock(&g_raw_rtp_mutex);
}

static bool wait_for_raw_rtcp(int target, int timeout_ms) {
    rtc_mutex_lock(&g_raw_rtp_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_raw_rtcp_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_raw_rtp_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_raw_rtp_cond, &g_raw_rtp_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_raw_rtp_mutex);
    return true;
}

static bool wait_for_app_data(int target, int timeout_ms) {
    rtc_mutex_lock(&g_raw_rtp_mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_app_data_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_raw_rtp_mutex);
            return false;
        }
        rtc_cond_wait_timeout(&g_raw_rtp_cond, &g_raw_rtp_mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_raw_rtp_mutex);
    return true;
}

static bool wait_for_transport_packets(rtc_transport_t *transport, uint64_t target,
                                       int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.packets_received >= target) {
            return true;
        }
        SLEEP_MS(10);
    }
    return false;
}

TEST(transport_create_ice_params) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);

    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                                .enable_twcc = true,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    ASSERT(ice.username_fragment[0] != '\0');
    ASSERT(ice.password[0] != '\0');
    ASSERT_EQ(ice.mode, RTC_ICE_MODE_LITE);

    rtc_dtls_parameters_t dtls;
    ASSERT_EQ(rtc_transport_get_dtls_parameters(transport, &dtls), RTC_OK);
    ASSERT_EQ(dtls.role, RTC_TRANSPORT_DTLS_ROLE_SERVER);
    ASSERT(dtls.fingerprint[0] != '\0');

    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(transport, &stats), RTC_OK);
    ASSERT(!stats.closed);

    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_receives_stun_by_ufrag) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);

    char username[64];
    snprintf(username, sizeof(username), "%s:remote", ice.username_fragment);
    rtc_stun_msg_t req;
    ASSERT_EQ(rtc_stun_build_binding_request(&req, username, NULL, 1234, true, 99, true), RTC_OK);
    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT_EQ(send_udp_from(listener, req.buf, req.buf_len, sender), RTC_OK);
    ASSERT(wait_for_transport_packets(transport, 1, 1000));

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT(recv_stun_response(sender, resp_buf, sizeof(resp_buf), &resp_len, 1000));
    rtc_stun_msg_t resp;
    ASSERT_EQ(rtc_stun_parse(&resp, resp_buf, resp_len), RTC_OK);
    ASSERT_EQ(resp.type, STUN_BINDING_RESPONSE);

    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(transport, &stats), RTC_OK);
    ASSERT(stats.selected_tuple_valid);

    const uint8_t rtp[] = {0x80, 0x60, 0x00, 0x01, 0, 0, 0, 1, 0x12, 0x34, 0x56, 0x78};
    ASSERT_EQ(send_udp_from(listener, rtp, sizeof(rtp), sender), RTC_OK);
    ASSERT(wait_for_transport_packets(transport, 2, 1000));

    rtc_transport_close(transport);
    ASSERT_EQ(rtc_transport_get_stats(transport, &stats), RTC_OK);
    ASSERT(stats.closed);

    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_dtls_handshake_exports_srtp) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);

    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, sender, ice.username_fragment));

    rtc_dtls_transport_t client;
    ASSERT(drive_dtls_handshake(listener, transport, sender, &client));

    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(transport, &stats), RTC_OK);
    ASSERT(stats.dtls_packets_received > 0);
    ASSERT(stats.srtp_ready);

    rtc_dtls_close(&client);
    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_routes_srtp_to_producer) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, sender, ice.username_fragment));

    rtc_dtls_transport_t client;
    ASSERT(drive_dtls_handshake(listener, transport, sender, &client));
    ASSERT_EQ(rtc_dtls_export_srtp_keys(&client), RTC_OK);

    rtc_producer_t *producer =
        rtc_transport_produce(transport, &(rtc_producer_options_t){
                                             .kind = RTC_MEDIA_KIND_VIDEO,
                                             .rtp =
                                                 {
                                                     .ssrc = 0x12345678,
                                                     .codec_count = 1,
                                                     .codecs = {{
                                                         .kind = RTC_MEDIA_KIND_VIDEO,
                                                         .payload_type = 96,
                                                         .clock_rate = 90000,
                                                         .mime_type = "video/VP8",
                                                     }},
                                                 },
                                             .label = "camera",
                                         });
    ASSERT(producer != NULL);

    rtc_srtp_ctx_t client_srtp;
    ASSERT_EQ(rtc_srtp_init(&client_srtp, client.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                            client.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN),
              RTC_OK);

    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
    rtc_rtp_packet_t pkt;
    ASSERT_EQ(rtc_rtp_build(&pkt, 96, 77, 9000, 0x12345678, true, payload, sizeof(payload)),
              RTC_OK);
    uint8_t wire[1500];
    memcpy(wire, pkt.buf, pkt.buf_len);
    size_t wire_len = pkt.buf_len;
    ASSERT_EQ(rtc_srtp_protect(&client_srtp, wire, &wire_len, sizeof(wire)), RTC_OK);

    ASSERT_EQ(send_udp_from(listener, wire, wire_len, sender), RTC_OK);
    ASSERT(wait_for_producer_packets(producer, 1, 1000));

    rtc_producer_stats_t stats;
    ASSERT_EQ(rtc_producer_get_stats(producer, &stats), RTC_OK);
    ASSERT_EQ(stats.packets_received, 1);
    ASSERT_EQ(stats.bytes_received, sizeof(payload));

    rtc_srtp_close(&client_srtp);
    rtc_producer_destroy(producer);
    rtc_dtls_close(&client);
    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_invokes_raw_rtp_handler) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, sender, ice.username_fragment));

    rtc_dtls_transport_t client;
    ASSERT(drive_dtls_handshake(listener, transport, sender, &client));
    ASSERT_EQ(rtc_dtls_export_srtp_keys(&client), RTC_OK);

    reset_raw_rtp();
    rtc_transport_on_rtp(transport, raw_rtp_callback, NULL);

    rtc_srtp_ctx_t client_srtp;
    ASSERT_EQ(rtc_srtp_init(&client_srtp, client.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                            client.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN),
              RTC_OK);

    const uint8_t payload[] = {0x01, 0x02, 0x03};
    rtc_rtp_packet_t pkt;
    ASSERT_EQ(rtc_rtp_build(&pkt, 96, 1, 1234, 0xAABBCCDD, false, payload, sizeof(payload)),
              RTC_OK);
    uint8_t wire[1500];
    memcpy(wire, pkt.buf, pkt.buf_len);
    size_t wire_len = pkt.buf_len;
    ASSERT_EQ(rtc_srtp_protect(&client_srtp, wire, &wire_len, sizeof(wire)), RTC_OK);
    ASSERT_EQ(send_udp_from(listener, wire, wire_len, sender), RTC_OK);
    ASSERT(wait_for_raw_rtp(1, 1000));
    ASSERT_EQ(g_raw_rtp_ssrc, 0xAABBCCDD);
    ASSERT_EQ(g_raw_rtp_payload_len, sizeof(payload));

    rtc_srtp_close(&client_srtp);
    rtc_dtls_close(&client);
    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_invokes_raw_rtcp_handler) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, sender, ice.username_fragment));

    rtc_dtls_transport_t client;
    ASSERT(drive_dtls_handshake(listener, transport, sender, &client));
    ASSERT_EQ(rtc_dtls_export_srtp_keys(&client), RTC_OK);

    reset_raw_rtcp();
    rtc_transport_on_rtcp(transport, raw_rtcp_callback, NULL);

    rtc_srtp_ctx_t client_srtp;
    ASSERT_EQ(rtc_srtp_init(&client_srtp, client.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                            client.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN),
              RTC_OK);
    uint8_t rr[64] = {0x80, 201, 0x00, 0x01, 0x12, 0x34, 0x56, 0x78};
    size_t rr_len = 8;
    ASSERT_EQ(rtc_srtp_protect_rtcp(&client_srtp, rr, &rr_len, sizeof(rr)), RTC_OK);
    ASSERT_EQ(send_udp_from(listener, rr, rr_len, sender), RTC_OK);
    ASSERT(wait_for_raw_rtcp(1, 1000));
    ASSERT_EQ(g_raw_rtcp_pt, 201);

    rtc_srtp_close(&client_srtp);
    rtc_dtls_close(&client);
    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_sends_and_receives_app_data) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    rtc_socket_t sender = make_bound_sender();
    ASSERT(sender != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, sender, ice.username_fragment));

    rtc_dtls_transport_t client;
    ASSERT(drive_dtls_handshake(listener, transport, sender, &client));

    const uint8_t server_msg[] = {0xA1, 0xA2, 0xA3};
    ASSERT_EQ(rtc_transport_send_data(transport, server_msg, sizeof(server_msg)), RTC_OK);
    uint8_t wire[2048];
    size_t wire_len = 0;
    ASSERT(recv_udp_packet(sender, wire, sizeof(wire), &wire_len, 1000));
    ASSERT_EQ(rtc_dtls_recv(&client, wire, wire_len), RTC_OK);
    uint8_t app_buf[32];
    int app_len = SSL_read(client.ssl, app_buf, sizeof(app_buf));
    ASSERT_EQ(app_len, (int)sizeof(server_msg));
    ASSERT_MEM_EQ(app_buf, server_msg, sizeof(server_msg));

    reset_app_data();
    rtc_transport_on_data(transport, app_data_callback, NULL);
    const uint8_t client_msg[] = {0xB1, 0xB2, 0xB3, 0xB4};
    ASSERT(SSL_write(client.ssl, client_msg, sizeof(client_msg)) > 0);
    int pending;
    while ((pending = BIO_read(client.wbio, wire, sizeof(wire))) > 0) {
        ASSERT_EQ(send_udp_from(listener, wire, (size_t)pending, sender), RTC_OK);
    }
    ASSERT(wait_for_app_data(1, 1000));
    ASSERT_EQ(g_app_data_len, sizeof(client_msg));
    ASSERT_MEM_EQ(g_app_data, client_msg, sizeof(client_msg));

    rtc_dtls_close(&client);
    rtc_close_socket(sender);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(transport_forwards_producer_rtp_to_consumer) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);

    rtc_transport_t *pub_transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    rtc_transport_t *sub_transport =
        rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                .listener = listener,
                                                .ice_mode = RTC_ICE_MODE_LITE,
                                            });
    ASSERT(pub_transport != NULL);
    ASSERT(sub_transport != NULL);

    rtc_ice_parameters_t pub_ice, sub_ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(pub_transport, &pub_ice), RTC_OK);
    ASSERT_EQ(rtc_transport_get_ice_parameters(sub_transport, &sub_ice), RTC_OK);

    rtc_socket_t pub_sock = make_bound_sender();
    rtc_socket_t sub_sock = make_bound_sender();
    ASSERT(pub_sock != RTC_INVALID_SOCKET);
    ASSERT(sub_sock != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, pub_transport, pub_sock, pub_ice.username_fragment));
    ASSERT(select_transport_tuple(listener, sub_transport, sub_sock, sub_ice.username_fragment));

    rtc_dtls_transport_t pub_client, sub_client;
    ASSERT(drive_dtls_handshake(listener, pub_transport, pub_sock, &pub_client));
    ASSERT(drive_dtls_handshake(listener, sub_transport, sub_sock, &sub_client));
    ASSERT_EQ(rtc_dtls_export_srtp_keys(&pub_client), RTC_OK);
    ASSERT_EQ(rtc_dtls_export_srtp_keys(&sub_client), RTC_OK);

    rtc_producer_t *producer =
        rtc_transport_produce(pub_transport, &(rtc_producer_options_t){
                                                 .kind = RTC_MEDIA_KIND_VIDEO,
                                                 .rtp =
                                                     {
                                                         .ssrc = 0x12345678,
                                                         .codec_count = 1,
                                                         .codecs = {{
                                                             .kind = RTC_MEDIA_KIND_VIDEO,
                                                             .payload_type = 96,
                                                             .clock_rate = 90000,
                                                             .mime_type = "video/VP8",
                                                         }},
                                                     },
                                                 .label = "camera",
                                             });
    ASSERT(producer != NULL);
    rtc_consumer_t *consumer = rtc_transport_consume(sub_transport, &(rtc_consumer_options_t){
                                                                        .producer = producer,
                                                                        .paused = false,
                                                                    });
    ASSERT(consumer != NULL);

    rtc_srtp_ctx_t pub_send;
    rtc_srtp_ctx_t sub_recv;
    ASSERT_EQ(rtc_srtp_init(&pub_send, pub_client.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                            pub_client.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN),
              RTC_OK);
    ASSERT_EQ(rtc_srtp_init(&sub_recv, sub_client.srtp_server_key, RTC_SRTP_MASTER_KEY_LEN,
                            sub_client.srtp_server_salt, RTC_SRTP_MASTER_SALT_LEN),
              RTC_OK);

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    rtc_rtp_packet_t pkt;
    ASSERT_EQ(rtc_rtp_build(&pkt, 96, 77, 9000, 0x12345678, true, payload, sizeof(payload)),
              RTC_OK);
    uint8_t wire[1500];
    memcpy(wire, pkt.buf, pkt.buf_len);
    size_t wire_len = pkt.buf_len;
    ASSERT_EQ(rtc_srtp_protect(&pub_send, wire, &wire_len, sizeof(wire)), RTC_OK);
    ASSERT_EQ(send_udp_from(listener, wire, wire_len, pub_sock), RTC_OK);

    ASSERT(wait_for_producer_packets(producer, 1, 1000));
    ASSERT(wait_for_consumer_packets(consumer, 1, 1000));

    uint8_t recv_buf[1500];
    size_t recv_len = 0;
    ASSERT(recv_udp_packet(sub_sock, recv_buf, sizeof(recv_buf), &recv_len, 1000));
    ASSERT_EQ(rtc_srtp_unprotect(&sub_recv, recv_buf, &recv_len), RTC_OK);
    rtc_rtp_packet_t recv_pkt;
    ASSERT_EQ(rtc_rtp_parse(&recv_pkt, recv_buf, recv_len), RTC_OK);
    ASSERT_EQ(recv_pkt.header.payload_type, 96);
    ASSERT(recv_pkt.header.ssrc != 0x12345678);
    ASSERT_MEM_EQ(recv_pkt.payload, payload, sizeof(payload));

    rtc_srtp_close(&pub_send);
    rtc_srtp_close(&sub_recv);
    rtc_consumer_destroy(consumer);
    rtc_producer_destroy(producer);
    rtc_dtls_close(&pub_client);
    rtc_dtls_close(&sub_client);
    rtc_close_socket(pub_sock);
    rtc_close_socket(sub_sock);
    rtc_transport_destroy(pub_transport);
    rtc_transport_destroy(sub_transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

TEST(router_rejects_closed_create) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);

    rtc_router_close(router);
    ASSERT(rtc_router_create_transport(router, &(rtc_transport_config_t){.listener = listener}) ==
           NULL);

    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

int main(void) {
    rtc_init();
    rtc_mutex_init(&g_raw_rtp_mutex);
    rtc_cond_init(&g_raw_rtp_cond);

    RUN_TEST(transport_create_ice_params);
    RUN_TEST(transport_receives_stun_by_ufrag);
    RUN_TEST(transport_dtls_handshake_exports_srtp);
    RUN_TEST(transport_routes_srtp_to_producer);
    RUN_TEST(transport_invokes_raw_rtp_handler);
    RUN_TEST(transport_invokes_raw_rtcp_handler);
    RUN_TEST(transport_sends_and_receives_app_data);
    RUN_TEST(transport_forwards_producer_rtp_to_consumer);
    RUN_TEST(router_rejects_closed_create);

    rtc_cond_destroy(&g_raw_rtp_cond);
    rtc_mutex_destroy(&g_raw_rtp_mutex);
    rtc_cleanup();
    TEST_SUMMARY();
}