/*
 * test_sfu_sender.c - RTP sender over logical transport tests.
 */
#include <rtc/rtc.h>

#include "rtc_peer_internal.h"
#include "rtc_srtp.h"
#include "rtc_stun.h"
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
    int sent = sendto(sender, (const char *)data, (int)len, 0,
                      (const struct sockaddr *)&dest.addr, dest.len);
    return sent == (int)len ? RTC_OK : RTC_ERR_SOCKET;
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

typedef struct {
    rtc_listener_t *listener;
    rtc_socket_t sender;
} dtls_client_send_ctx_t;

static int dtls_client_send(const uint8_t *data, size_t len, void *user) {
    dtls_client_send_ctx_t *ctx = (dtls_client_send_ctx_t *)user;
    return send_udp_from(ctx->listener, data, len, ctx->sender);
}

static bool wait_for_transport_packets(rtc_transport_t *transport, uint64_t target,
                                       int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.packets_received >= target)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_dtls_connected(rtc_transport_t *transport, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK &&
            stats.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED && stats.srtp_ready)
            return true;
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
    return wait_for_transport_packets(transport, 1, 1000);
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

TEST(sender_send_over_logical_transport) {
    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    rtc_router_t *router = rtc_router_create(worker, NULL);
    ASSERT(router != NULL);
    rtc_transport_t *transport = rtc_router_create_transport(router, &(rtc_transport_config_t){
                                                                       .listener = listener,
                                                                       .ice_mode = RTC_ICE_MODE_LITE,
                                                                   });
    ASSERT(transport != NULL);

    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &ice), RTC_OK);
    rtc_socket_t remote = make_bound_sender();
    ASSERT(remote != RTC_INVALID_SOCKET);
    ASSERT(select_transport_tuple(listener, transport, remote, ice.username_fragment));

    rtc_dtls_transport_t client;
    ASSERT(drive_dtls_handshake(listener, transport, remote, &client));
    ASSERT_EQ(rtc_dtls_export_srtp_keys(&client), RTC_OK);

    rtc_codec_t vp8;
    memset(&vp8, 0, sizeof(vp8));
    vp8.payload_type = 96;
    memcpy(vp8.mime_type, "video/VP8", sizeof("video/VP8"));
    vp8.clock_rate = 90000;

    struct rtc_rtp_transceiver transceiver;
    rtc_rtp_transceiver_init_slot(&transceiver, 0, RTC_KIND_VIDEO, &vp8);
    rtc_rtp_sender_attach_logical(&transceiver.sender, transport);

    const uint8_t payload[] = {0xFE, 0xED, 0xFA, 0xCE};
    ASSERT_EQ(rtc_rtp_sender_send(&transceiver.sender, payload, sizeof(payload), 3000, true),
              RTC_OK);

    uint8_t buf[1500];
    size_t len = 0;
    ASSERT(recv_udp_packet(remote, buf, sizeof(buf), &len, 1000));

    rtc_srtp_ctx_t recv_ctx;
    ASSERT_EQ(rtc_srtp_init(&recv_ctx, client.srtp_server_key, RTC_SRTP_MASTER_KEY_LEN,
                            client.srtp_server_salt, RTC_SRTP_MASTER_SALT_LEN),
              RTC_OK);
    ASSERT_EQ(rtc_srtp_unprotect(&recv_ctx, buf, &len), RTC_OK);

    rtc_rtp_packet_t parsed;
    ASSERT_EQ(rtc_rtp_parse(&parsed, buf, len), RTC_OK);
    ASSERT_EQ(parsed.header.payload_type, 96);
    ASSERT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_MEM_EQ(parsed.payload, payload, sizeof(payload));

    rtc_srtp_close(&recv_ctx);
    rtc_rtp_transceiver_close_resources(&transceiver);
    rtc_dtls_close(&client);
    rtc_close_socket(remote);
    rtc_transport_destroy(transport);
    rtc_router_destroy(router);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
}

int main(void) {
    rtc_init();
    RUN_TEST(sender_send_over_logical_transport);
    rtc_cleanup();
    TEST_SUMMARY();
}