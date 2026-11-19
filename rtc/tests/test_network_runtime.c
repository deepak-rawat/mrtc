#include <rtc/rtc.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_transport.h>
#include "rtc_dtls.h"
#include "rtc_listener_internal.h"
#include "rtc_stun.h"
#include "rtc_srtp.h"
#include "test_harness.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_transport_t *transport;
    _Atomic unsigned data_count;
    _Atomic unsigned rtp_count;
    bool owns_network;
} endpoint_t;

static void received_data(const uint8_t *data, size_t len, void *user) {
    endpoint_t *endpoint = user;
    if (len == 4 && memcmp(data, "test", 4) == 0)
        atomic_fetch_add(&endpoint->data_count, 1);
}

static void received_rtp(const rtc_rtp_packet_t *packet, void *user) {
    endpoint_t *endpoint = user;
    if (packet->payload_len == 4 && memcmp(packet->payload, "rtp!", 4) == 0)
        atomic_fetch_add(&endpoint->rtp_count, 1);
}

static bool endpoint_create(endpoint_t *endpoint, rtc_ice_mode_t mode, uint32_t consent_ms) {
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->owns_network = true;
    endpoint->worker = rtc_worker_create(NULL);
    if (!endpoint->worker)
        return false;
    endpoint->listener = rtc_listener_create(endpoint->worker, NULL);
    if (!endpoint->listener)
        return false;
    endpoint->transport = rtc_transport_create(
        endpoint->worker, &(rtc_transport_config_t){.listener = endpoint->listener,
                                                    .ice_mode = mode,
                                                    .consent_timeout_ms = consent_ms});
    if (!endpoint->transport)
        return false;
    rtc_transport_on_data(endpoint->transport, received_data, endpoint);
    return true;
}

static void endpoint_close(endpoint_t *endpoint) {
    rtc_transport_destroy(endpoint->transport);
    if (endpoint->owns_network) {
        rtc_listener_destroy(endpoint->listener);
        rtc_worker_destroy(endpoint->worker);
    }
}

static bool endpoint_candidate(endpoint_t *endpoint, rtc_transport_candidate_t *candidate) {
    rtc_addr_t local;
    uint16_t port;
    char ip[64];
    if (rtc_listener_get_local_addr(endpoint->listener, &local) != RTC_OK ||
        rtc_addr_to_string(&local, ip, sizeof(ip), &port) != RTC_OK)
        return false;
    memset(candidate, 0, sizeof(*candidate));
    strcpy(candidate->address, "127.0.0.1");
    strcpy(candidate->protocol, "udp");
    candidate->port = port;
    return true;
}

static bool exchange_ice(endpoint_t *a, endpoint_t *b, bool relay) {
    rtc_ice_parameters_t ai, bi;
    rtc_transport_candidate_t ac, bc;
    if (rtc_transport_get_ice_parameters(a->transport, &ai) != RTC_OK ||
        rtc_transport_get_ice_parameters(b->transport, &bi) != RTC_OK)
        return false;
    if (relay) {
        if (rtc_transport_get_relay_candidate(a->transport, &ac) != RTC_OK ||
            rtc_transport_get_relay_candidate(b->transport, &bc) != RTC_OK)
            return false;
    } else if (!endpoint_candidate(a, &ac) || !endpoint_candidate(b, &bc)) {
        return false;
    }
    return rtc_transport_set_remote_ice_parameters(a->transport, &bi) == RTC_OK &&
           rtc_transport_set_remote_ice_parameters(b->transport, &ai) == RTC_OK &&
           rtc_transport_add_remote_candidate(a->transport, &bc) == RTC_OK &&
           rtc_transport_add_remote_candidate(b->transport, &ac) == RTC_OK;
}

static bool exchange_dtls(endpoint_t *a, endpoint_t *b, bool mismatch) {
    rtc_dtls_parameters_t ad, bd;
    if (rtc_transport_set_dtls_role(b->transport, RTC_TRANSPORT_DTLS_ROLE_SERVER) != RTC_OK ||
        rtc_transport_get_dtls_parameters(a->transport, &ad) != RTC_OK ||
        rtc_transport_get_dtls_parameters(b->transport, &bd) != RTC_OK)
        return false;
    if (mismatch)
        bd.fingerprint[0] = bd.fingerprint[0] == '0' ? '1' : '0';
    for (size_t i = 0; bd.fingerprint[i]; i++) {
        if (bd.fingerprint[i] >= 'A' && bd.fingerprint[i] <= 'F')
            bd.fingerprint[i] = (char)(bd.fingerprint[i] - 'A' + 'a');
    }
    return rtc_transport_set_remote_dtls_parameters(a->transport, &bd) == RTC_OK &&
           rtc_transport_set_remote_dtls_parameters(b->transport, &ad) == RTC_OK;
}

static bool wait_selected(endpoint_t *a, endpoint_t *b) {
    uint64_t deadline = rtc_time_ms() + 5000;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t as, bs;
        rtc_transport_get_stats(a->transport, &as);
        rtc_transport_get_stats(b->transport, &bs);
        if (as.selected_tuple_valid && bs.selected_tuple_valid)
            return true;
        SLEEP_MS(10);
    }
    rtc_transport_stats_t as, bs;
    rtc_transport_get_stats(a->transport, &as);
    rtc_transport_get_stats(b->transport, &bs);
    printf("    ICE timeout: received=%llu/%llu selected=%d/%d roles=%d/%d\n",
           (unsigned long long)as.packets_received, (unsigned long long)bs.packets_received,
           as.selected_tuple_valid, bs.selected_tuple_valid, as.ice_controlling,
           bs.ice_controlling);
    return false;
}

static bool wait_dtls(endpoint_t *endpoint, rtc_transport_dtls_state_t state) {
    uint64_t deadline = rtc_time_ms() + 5000;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        rtc_transport_get_stats(endpoint->transport, &stats);
        if (stats.dtls_state == state)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

TEST(runtime_mdns_candidates_wait_for_authenticated_peer_reflexive_path) {
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 30000));
    ASSERT(endpoint_create(&b, RTC_ICE_MODE_FULL, 30000));
    rtc_ice_parameters_t ai, bi;
    ASSERT_EQ(rtc_transport_get_ice_parameters(a.transport, &ai), RTC_OK);
    ASSERT_EQ(rtc_transport_get_ice_parameters(b.transport, &bi), RTC_OK);
    ASSERT_EQ(rtc_transport_set_remote_ice_parameters(a.transport, &bi), RTC_OK);
    ASSERT_EQ(rtc_transport_set_remote_ice_parameters(b.transport, &ai), RTC_OK);
    ASSERT_EQ(rtc_transport_set_ice_controlling(a.transport, true), RTC_OK);
    ASSERT_EQ(rtc_transport_set_ice_controlling(b.transport, false), RTC_OK);
    ASSERT(exchange_dtls(&a, &b, false));
    rtc_transport_candidate_t numeric;
    ASSERT(endpoint_candidate(&a, &numeric));
    ASSERT_EQ(rtc_transport_add_remote_candidate(b.transport, &numeric), RTC_OK);
    rtc_ice_candidate_t obfuscated;
    ASSERT_EQ(rtc_sdp_parse_candidate_line("candidate:mdns 1 udp 2122260223 browser.local 50000 "
                                           "typ host",
                                           &obfuscated),
              RTC_OK);
    rtc_transport_candidate_t converted;
    ASSERT_EQ(rtc_transport_candidate_from_ice(&converted, &obfuscated), RTC_OK);
    ASSERT_EQ(rtc_transport_add_remote_candidate(a.transport, &converted), RTC_OK);
    ASSERT_EQ(rtc_sdp_parse_candidate_line("candidate:tcp 1 tcp 1518280447 127.0.0.1 9 typ host "
                                           "tcptype active",
                                           &obfuscated),
              RTC_OK);
    ASSERT_EQ(rtc_transport_candidate_from_ice(&converted, &obfuscated), RTC_OK);
    ASSERT_EQ(rtc_transport_add_remote_candidate(a.transport, &converted), RTC_OK);
    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(a.transport, &stats), RTC_OK);
    ASSERT_EQ(stats.remote_candidates_deferred, 1);
    ASSERT_EQ(stats.remote_candidates_ineligible, 1);
    ASSERT(!stats.selected_tuple_valid && !stats.srtp_ready);
    ASSERT_EQ(rtc_transport_start_ice(b.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    ASSERT_EQ(rtc_transport_start_dtls(a.transport), RTC_OK);
    ASSERT(wait_dtls(&a, RTC_TRANSPORT_DTLS_CONNECTED));
    ASSERT(wait_dtls(&b, RTC_TRANSPORT_DTLS_CONNECTED));
    rtc_transport_on_rtp(b.transport, received_rtp, &b);
    rtc_rtp_packet_t packet;
    ASSERT_EQ(rtc_rtp_build(&packet, 96, 7, 90000, 0x11223344, true, (const uint8_t *)"rtp!", 4),
              RTC_OK);
    size_t wire_len = packet.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(a.transport, packet.buf, &wire_len, sizeof(packet.buf)),
              RTC_OK);
    uint64_t deadline = rtc_time_ms() + 1000;
    while (!atomic_load(&b.rtp_count) && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    ASSERT_EQ(atomic_load(&b.rtp_count), 1);
    endpoint_close(&a);
    endpoint_close(&b);
}

TEST(runtime_roles_restart_and_consent) {
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 1200));
    ASSERT(endpoint_create(&b, RTC_ICE_MODE_FULL, 1200));
    ASSERT(exchange_ice(&a, &b, false));
    ASSERT(exchange_dtls(&a, &b, false));
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT_EQ(rtc_transport_start_ice(b.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    rtc_transport_stats_t as, bs;
    rtc_transport_get_stats(a.transport, &as);
    rtc_transport_get_stats(b.transport, &bs);
    ASSERT(as.ice_controlling != bs.ice_controlling);
    ASSERT_EQ(rtc_transport_start_dtls(a.transport), RTC_OK);
    ASSERT(wait_dtls(&a, RTC_TRANSPORT_DTLS_CONNECTED));
    ASSERT(wait_dtls(&b, RTC_TRANSPORT_DTLS_CONNECTED));
    rtc_ice_parameters_t old_ice, new_ice;
    rtc_transport_get_ice_parameters(a.transport, &old_ice);
    ASSERT_EQ(rtc_transport_restart_ice(a.transport), RTC_OK);
    rtc_transport_get_ice_parameters(a.transport, &new_ice);
    ASSERT(strcmp(old_ice.password, new_ice.password) != 0);
    ASSERT(exchange_ice(&a, &b, false));
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT_EQ(rtc_transport_start_ice(b.transport), RTC_OK);
    SLEEP_MS(500);
    rtc_transport_get_stats(a.transport, &as);
    ASSERT(as.selected_tuple_valid && as.srtp_ready);
    ASSERT_EQ(as.dtls_state, RTC_TRANSPORT_DTLS_CONNECTED);
    ASSERT_EQ(rtc_transport_send_data(a.transport, (const uint8_t *)"test", 4), RTC_OK);
    uint64_t deadline = rtc_time_ms() + 1000;
    while (!atomic_load(&b.data_count) && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    ASSERT_EQ(atomic_load(&b.data_count), 1);
    rtc_transport_get_stats(b.transport, &bs);
    ASSERT(!bs.ice_restarting);
    rtc_transport_close(b.transport);
    SLEEP_MS(1600);
    rtc_transport_get_stats(a.transport, &as);
    ASSERT(as.consent_expired);
    ASSERT(!as.selected_tuple_valid);
    ASSERT_EQ(rtc_transport_send_data(a.transport, (const uint8_t *)"test", 4), RTC_ERR_INVALID);
    endpoint_close(&a);
    endpoint_close(&b);
}

TEST(runtime_fingerprint_mismatch) {
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 0));
    ASSERT(endpoint_create(&b, RTC_ICE_MODE_LITE, 0));
    ASSERT(exchange_ice(&a, &b, false));
    ASSERT(exchange_dtls(&a, &b, true));
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    int rc = rtc_transport_start_dtls(a.transport);
    ASSERT(rc == RTC_OK || rc == RTC_ERR_SSL);
    ASSERT(wait_dtls(&a, RTC_TRANSPORT_DTLS_FAILED));
    rtc_transport_stats_t stats;
    rtc_transport_get_stats(a.transport, &stats);
    ASSERT(!stats.srtp_ready);
    endpoint_close(&a);
    endpoint_close(&b);
}

TEST(runtime_checked_wire_retransmit) {
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 0));
    ASSERT(endpoint_create(&b, RTC_ICE_MODE_LITE, 0));
    uint8_t wire[SRTP_MAX_PACKET] = {0x80};
    ASSERT_EQ(rtc_transport_retransmit_rtp(a.transport, wire, 32), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_transport_retransmit_rtp(a.transport, wire, 1), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_transport_retransmit_rtp(a.transport, wire, sizeof(wire) + 1), RTC_ERR_INVALID);
    ASSERT(exchange_ice(&a, &b, false));
    ASSERT(exchange_dtls(&a, &b, false));
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    ASSERT_EQ(rtc_transport_start_dtls(a.transport), RTC_OK);
    ASSERT(wait_dtls(&a, RTC_TRANSPORT_DTLS_CONNECTED));
    ASSERT(wait_dtls(&b, RTC_TRANSPORT_DTLS_CONNECTED));
    rtc_transport_on_rtp(b.transport, received_rtp, &b);
    rtc_rtp_packet_t packet;
    ASSERT_EQ(rtc_rtp_build(&packet, 111, 65535, 90000, 0x12345678, false, (const uint8_t *)"rtp!",
                            4),
              RTC_OK);
    memcpy(wire, packet.buf, packet.buf_len);
    size_t wire_len = packet.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(a.transport, wire, &wire_len, sizeof(wire)), RTC_OK);
    uint64_t deadline = rtc_time_ms() + 1000;
    while (!atomic_load(&b.rtp_count) && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    ASSERT_EQ(atomic_load(&b.rtp_count), 1);
    uint8_t cached[SRTP_MAX_PACKET];
    memcpy(cached, wire, wire_len);
    rtc_transport_stats_t before, after;
    ASSERT_EQ(rtc_transport_get_stats(b.transport, &before), RTC_OK);
    ASSERT_EQ(rtc_transport_retransmit_rtp(a.transport, wire, wire_len), RTC_OK);
    ASSERT_MEM_EQ(wire, cached, wire_len);
    deadline = rtc_time_ms() + 1000;
    do {
        ASSERT_EQ(rtc_transport_get_stats(b.transport, &after), RTC_OK);
        if (after.packets_received > before.packets_received)
            break;
        SLEEP_MS(10);
    } while (rtc_time_ms() < deadline);
    ASSERT(after.packets_received > before.packets_received);
    ASSERT_EQ(atomic_load(&b.rtp_count), 1);
    wire[0] = 0;
    ASSERT_EQ(rtc_transport_retransmit_rtp(a.transport, wire, wire_len), RTC_ERR_INVALID);
    rtc_transport_close(a.transport);
    ASSERT_EQ(rtc_transport_retransmit_rtp(a.transport, cached, wire_len), RTC_ERR_INVALID);
    endpoint_close(&a);
    endpoint_close(&b);
}

TEST(runtime_controlled_role_conflict_and_missing_pin) {
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 0));
    ASSERT(endpoint_create(&b, RTC_ICE_MODE_FULL, 0));
    ASSERT(exchange_ice(&a, &b, false));
    ASSERT_EQ(rtc_transport_set_ice_controlling(a.transport, false), RTC_OK);
    ASSERT_EQ(rtc_transport_set_ice_controlling(b.transport, false), RTC_OK);
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT_EQ(rtc_transport_start_ice(b.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    rtc_transport_stats_t as, bs;
    rtc_transport_get_stats(a.transport, &as);
    rtc_transport_get_stats(b.transport, &bs);
    ASSERT(as.ice_controlling != bs.ice_controlling);
    ASSERT_EQ(rtc_transport_start_dtls(a.transport), RTC_ERR_INVALID);
    rtc_dtls_parameters_t invalid = {.role = RTC_TRANSPORT_DTLS_ROLE_SERVER};
    strcpy(invalid.fingerprint, "not-a-sha256-fingerprint");
    ASSERT_EQ(rtc_transport_set_remote_dtls_parameters(a.transport, &invalid), RTC_ERR_INVALID);
    endpoint_close(&a);
    endpoint_close(&b);
}

TEST(runtime_rejects_unauthenticated_nomination) {
    endpoint_t a;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_LITE, 0));
    rtc_ice_parameters_t ice;
    ASSERT_EQ(rtc_transport_get_ice_parameters(a.transport, &ice), RTC_OK);
    rtc_transport_candidate_t target;
    ASSERT(endpoint_candidate(&a, &target));
    rtc_addr_t addr;
    ASSERT_EQ(rtc_addr_from_string(&addr, target.address, target.port), RTC_OK);
    rtc_socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT(sock != RTC_INVALID_SOCKET);
    char username[300];
    snprintf(username, sizeof(username), "%s:attacker", ice.username_fragment);
    rtc_stun_msg_t request;
    ASSERT_EQ(rtc_stun_build_binding_request(&request, username, "wrong", 123, true, 123, true),
              RTC_OK);
    ASSERT(sendto(sock, (const char *)request.buf, (int)request.buf_len, 0,
                  (const struct sockaddr *)&addr.addr, addr.len) > 0);
    SLEEP_MS(100);
    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(a.transport, &stats), RTC_OK);
    ASSERT(!stats.selected_tuple_valid);
    rtc_close_socket(sock);
    endpoint_close(&a);
}

static void destroy_transport_on_rtp(const rtc_rtp_packet_t *packet, void *user) {
    (void)packet;
    endpoint_t *endpoint = user;
    rtc_transport_t *transport = endpoint->transport;
    endpoint->transport = NULL;
    rtc_transport_destroy(transport);
    atomic_fetch_add(&endpoint->rtp_count, 1);
}

TEST(runtime_destroy_during_packet_callback) {
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 0));
    ASSERT(endpoint_create(&b, RTC_ICE_MODE_LITE, 0));
    ASSERT(exchange_ice(&a, &b, false));
    ASSERT(exchange_dtls(&a, &b, false));
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    ASSERT_EQ(rtc_transport_start_dtls(a.transport), RTC_OK);
    ASSERT(wait_dtls(&a, RTC_TRANSPORT_DTLS_CONNECTED));
    ASSERT(wait_dtls(&b, RTC_TRANSPORT_DTLS_CONNECTED));
    rtc_transport_on_rtp(b.transport, destroy_transport_on_rtp, &b);
    rtc_rtp_packet_t packet;
    ASSERT_EQ(rtc_rtp_build(&packet, 96, 1, 90000, 123, true, (const uint8_t *)"rtp!", 4), RTC_OK);
    size_t len = packet.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(a.transport, packet.buf, &len, sizeof(packet.buf)), RTC_OK);
    uint64_t deadline = rtc_time_ms() + 1000;
    while (!atomic_load(&b.rtp_count) && rtc_time_ms() < deadline)
        SLEEP_MS(1);
    ASSERT_EQ(atomic_load(&b.rtp_count), 1);
    endpoint_close(&b);
    endpoint_close(&a);
}

TEST(runtime_rejects_invalid_turn_address_lists) {
    endpoint_t endpoint;
    ASSERT(endpoint_create(&endpoint, RTC_ICE_MODE_FULL, 0));
    rtc_addr_t address;
    ASSERT_EQ(rtc_addr_from_string(&address, "127.0.0.1", 10), RTC_OK);
    rtc_turn_server_t server = {.host = "127.0.0.1",
                                .port = 9,
                                .username = "mrtc",
                                .credential = "runtime",
                                .addresses = &address,
                                .address_count = RTC_TURN_MAX_ADDRESSES + 1};
    ASSERT_EQ(rtc_transport_set_turn_server_ex(endpoint.transport, &server), RTC_ERR_INVALID);
    server.address_count = 1;
    ASSERT_EQ(rtc_transport_set_turn_server_ex(endpoint.transport, &server), RTC_ERR_INVALID);
    server.addresses = NULL;
    ASSERT_EQ(rtc_transport_set_turn_server_ex(endpoint.transport, &server), RTC_ERR_INVALID);
    rtc_transport_stats_t state;
    ASSERT_EQ(rtc_transport_get_stats(endpoint.transport, &state), RTC_OK);
    ASSERT_EQ(state.turn_address_attempts, 0);
    endpoint_close(&endpoint);
}

TEST(runtime_relay_only_coturn) {
    const char *port_string = getenv("MRTC_TEST_TURN_PORT");
    ASSERT(port_string != NULL);
    uint16_t port = (uint16_t)strtoul(port_string, NULL, 10);
    endpoint_t a, b;
    ASSERT(endpoint_create(&a, RTC_ICE_MODE_FULL, 0));
    memset(&b, 0, sizeof(b));
    b.worker = a.worker;
    b.listener = a.listener;
    b.transport = rtc_transport_create(
        b.worker, &(rtc_transport_config_t){.listener = b.listener, .ice_mode = RTC_ICE_MODE_FULL});
    ASSERT(b.transport != NULL);
    rtc_transport_on_data(b.transport, received_data, &b);
    const char *protocol = getenv("MRTC_TEST_TURN_TRANSPORT");
    char hostname[] = "localhost", username_text[] = "mrtc", credential_text[] = "runtime";
    char ca_file[1024] = {0};
    const char *ca = getenv("MRTC_TEST_TURN_CA");
    if (ca) {
        ASSERT(strlen(ca) < sizeof(ca_file));
        strcpy(ca_file, ca);
    }
    rtc_turn_server_t server = {.host = "127.0.0.1",
                                .port = port,
                                .username = username_text,
                                .credential = credential_text,
                                .protocol = protocol && !strcmp(protocol, "tls")   ? RTC_TURN_TLS
                                            : protocol && !strcmp(protocol, "tcp") ? RTC_TURN_TCP
                                                                                   : RTC_TURN_UDP,
                                .ca_file = ca ? ca_file : NULL};
    bool failover = getenv("MRTC_TEST_TURN_FAILOVER") != NULL;
    bool exhausted = getenv("MRTC_TEST_TURN_EXHAUST") != NULL;
    bool stall = getenv("MRTC_TEST_TURN_STALL") != NULL;
    bool dns = getenv("MRTC_TEST_TURN_DNS") != NULL;
    bool four = getenv("MRTC_TEST_TURN_FOUR") != NULL;
    ASSERT(!stall || failover);
    ASSERT(!four || (failover && !exhausted && server.protocol == RTC_TURN_UDP));
    if (dns)
        server.host = hostname;
    rtc_addr_t addresses[RTC_TURN_MAX_ADDRESSES];
    if (failover) {
        ASSERT_EQ(rtc_addr_from_string(&addresses[0],
                                       server.protocol == RTC_TURN_UDP ? "127.0.0.2" : "::1", port),
                  RTC_OK);
        ASSERT_EQ(rtc_addr_from_string(&addresses[1], exhausted ? "127.0.0.3" : "127.0.0.1", port),
                  RTC_OK);
        server.host = hostname;
        server.addresses = addresses;
        server.address_count = 2;
        if (four) {
            ASSERT_EQ(rtc_addr_from_string(&addresses[1], "127.0.0.3", port), RTC_OK);
            ASSERT_EQ(rtc_addr_from_string(&addresses[2], "127.0.0.4", port), RTC_OK);
            ASSERT_EQ(rtc_addr_from_string(&addresses[3], "127.0.0.1", port), RTC_OK);
            server.address_count = RTC_TURN_MAX_ADDRESSES;
        }
    }
    rtc_socket_t stalled = RTC_INVALID_SOCKET;
    if (stall) {
        stalled = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        ASSERT(stalled != RTC_INVALID_SOCKET);
        int v6_only = 1;
        ASSERT_EQ(setsockopt(stalled, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6_only,
                             sizeof(v6_only)),
                  0);
        ASSERT_EQ(bind(stalled, (struct sockaddr *)&addresses[0].addr, addresses[0].len), 0);
        ASSERT_EQ(listen(stalled, 4), 0);
    }
    uint64_t allocation_started = rtc_time_ms();
    ASSERT_EQ(rtc_transport_set_turn_server_ex(a.transport, &server), RTC_OK);
    ASSERT_EQ(rtc_transport_set_turn_server_ex(b.transport, &server), RTC_OK);
    if (stall) {
        rtc_transport_destroy(b.transport);
        b.transport = rtc_transport_create(
            b.worker,
            &(rtc_transport_config_t){.listener = b.listener, .ice_mode = RTC_ICE_MODE_FULL});
        ASSERT(b.transport != NULL);
        rtc_transport_on_data(b.transport, received_data, &b);
        ASSERT_EQ(rtc_transport_set_turn_server_ex(b.transport, &server), RTC_OK);
    }
    memset(addresses, 0, sizeof(addresses));
    memset(hostname, 'x', sizeof(hostname) - 1);
    memset(username_text, 'x', sizeof(username_text) - 1);
    memset(credential_text, 'x', sizeof(credential_text) - 1);
    memset(ca_file, 'x', sizeof(ca_file) - 1);
    if (exhausted) {
        rtc_ice_parameters_t ai, bi;
        rtc_transport_candidate_t host;
        ASSERT_EQ(rtc_transport_get_ice_parameters(a.transport, &ai), RTC_OK);
        ASSERT_EQ(rtc_transport_get_ice_parameters(b.transport, &bi), RTC_OK);
        ASSERT_EQ(rtc_transport_set_remote_ice_parameters(a.transport, &bi), RTC_OK);
        ASSERT_EQ(rtc_transport_set_ice_controlling(a.transport, false), RTC_OK);
        ASSERT(endpoint_candidate(&a, &host));
        endpoint_close(&b);
        uint64_t end = rtc_time_ms() + 10000;
        rtc_transport_stats_t state;
        do {
            ASSERT_EQ(rtc_transport_get_stats(a.transport, &state), RTC_OK);
            if (state.turn_failed)
                break;
            SLEEP_MS(10);
        } while (rtc_time_ms() < end);
        ASSERT(state.turn_failed);
        ASSERT_EQ(state.turn_address_attempts, 2);
        rtc_transport_candidate_t relay;
        ASSERT(rtc_transport_get_relay_candidate(a.transport, &relay) != RTC_OK);
        rtc_socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT(sock != RTC_INVALID_SOCKET);
        rtc_addr_t target;
        ASSERT_EQ(rtc_addr_from_string(&target, "127.0.0.1", host.port), RTC_OK);
        char username[RTC_ICE_UFRAG_MAX * 2 + 2];
        snprintf(username, sizeof(username), "%s:%s", ai.username_fragment, bi.username_fragment);
        rtc_stun_msg_t request;
        ASSERT_EQ(rtc_stun_build_binding_request(&request, username, ai.password, 123, true,
                                                 UINT64_MAX, true),
                  RTC_OK);
        ASSERT(sendto(sock, (const char *)request.buf, (int)request.buf_len, 0,
                      (struct sockaddr *)&target.addr, target.len) > 0);
        SLEEP_MS(100);
        ASSERT_EQ(rtc_transport_get_stats(a.transport, &state), RTC_OK);
        ASSERT(!state.selected_tuple_valid);
        rtc_close_socket(sock);
        endpoint_close(&a);
        return;
    }
    rtc_transport_on_rtp(b.transport, received_rtp, &b);
    rtc_transport_candidate_t ac, bc;
    bool reject = getenv("MRTC_TEST_TURN_EXPECT_REJECT") != NULL;
    uint64_t deadline = rtc_time_ms() + (reject ? 2000 : four ? 15000 : stall ? 8000 : 5000);
    bool allocated = false;
    while (rtc_time_ms() < deadline) {
        if (rtc_transport_get_relay_candidate(a.transport, &ac) == RTC_OK &&
            rtc_transport_get_relay_candidate(b.transport, &bc) == RTC_OK) {
            allocated = true;
            break;
        }
        SLEEP_MS(10);
    }
    if (reject) {
        ASSERT(!allocated);
        if (failover) {
            rtc_transport_stats_t state;
            ASSERT_EQ(rtc_transport_get_stats(a.transport, &state), RTC_OK);
            ASSERT_EQ(state.turn_address_attempts, 2);
            ASSERT(state.turn_failed);
        }
        endpoint_close(&b);
        endpoint_close(&a);
        return;
    }
    ASSERT(allocated);
    if (stall) {
        ASSERT(rtc_time_ms() - allocation_started >= 5000);
        rtc_close_socket(stalled);
    }
    rtc_transport_stats_t state;
    ASSERT_EQ(rtc_transport_get_stats(a.transport, &state), RTC_OK);
    unsigned attempts = state.turn_address_attempts;
    if (dns)
        ASSERT(attempts >= 1 && attempts <= RTC_TURN_MAX_ADDRESSES);
    else
        ASSERT_EQ(attempts, failover ? server.address_count : 1);
    ASSERT(!state.turn_failed);
    ASSERT(ac.port != bc.port);
    ASSERT(exchange_ice(&a, &b, true));
    ASSERT(exchange_dtls(&a, &b, false));
    ASSERT_EQ(rtc_transport_start_ice(a.transport), RTC_OK);
    ASSERT_EQ(rtc_transport_start_ice(b.transport), RTC_OK);
    ASSERT(wait_selected(&a, &b));
    ASSERT_EQ(rtc_transport_start_dtls(a.transport), RTC_OK);
    ASSERT(wait_dtls(&a, RTC_TRANSPORT_DTLS_CONNECTED));
    ASSERT(wait_dtls(&b, RTC_TRANSPORT_DTLS_CONNECTED));
    rtc_rtp_packet_t media;
    ASSERT_EQ(rtc_rtp_build(&media, 96, 12345, 90000, 0x1234, true, (const uint8_t *)"rtp!", 4),
              RTC_OK);
    size_t wire_len = media.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(a.transport, media.buf, &wire_len, sizeof(media.buf)), RTC_OK);
    deadline = rtc_time_ms() + 1000;
    while (!atomic_load(&b.rtp_count) && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    ASSERT_EQ(atomic_load(&b.rtp_count), 1);
    ASSERT_EQ(rtc_transport_send_data(a.transport, (const uint8_t *)"test", 4), RTC_OK);
    deadline = rtc_time_ms() + 1000;
    while (!atomic_load(&b.data_count) && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    ASSERT_EQ(atomic_load(&b.data_count), 1);
    SLEEP_MS(failover || dns ? 5500 : 3000);
    ASSERT_EQ(rtc_transport_send_data(a.transport, (const uint8_t *)"test", 4), RTC_OK);
    deadline = rtc_time_ms() + 1000;
    while (atomic_load(&b.data_count) < 2 && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    ASSERT_EQ(atomic_load(&b.data_count), 2);
    ASSERT_EQ(rtc_transport_get_stats(a.transport, &state), RTC_OK);
    ASSERT_EQ(state.turn_address_attempts, attempts);
    rtc_transport_on_rtp(b.transport, destroy_transport_on_rtp, &b);
    ASSERT_EQ(rtc_rtp_build(&media, 96, 12346, 93000, 0x1234, true, (const uint8_t *)"rtp!", 4),
              RTC_OK);
    wire_len = media.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(a.transport, media.buf, &wire_len, sizeof(media.buf)), RTC_OK);
    deadline = rtc_time_ms() + 1000;
    while (atomic_load(&b.rtp_count) < 2 && rtc_time_ms() < deadline)
        SLEEP_MS(1);
    ASSERT_EQ(atomic_load(&b.rtp_count), 2);
    endpoint_close(&b);
    endpoint_close(&a);
}

TEST(runtime_sdp_negotiation_fields) {
    rtc_sdp_t sdp = {0}, parsed = {0};
    sdp.media_count = 2;
    sdp.ice_lite = true;
    sdp.media[0].media_type = RTC_MEDIA_VIDEO;
    sdp.media[0].direction = 2;
    sdp.media[0].payload_type = 96;
    sdp.media[0].clockrate = 90000;
    strcpy(sdp.media[0].codec_name, "VP8");
    sdp.media[1].media_type = RTC_MEDIA_APPLICATION;
    sdp.media[1].mid_index = 1;
    sdp.media[1].sctp_port = 6000;
    sdp.media[1].max_message_size = 65535;
    memset(sdp.ice_ufrag, 'u', 128);
    memset(sdp.ice_pwd, 'p', 128);
    rtc_ice_candidate_t candidate = {
        .type = ICE_CANDIDATE_HOST, .component = 1, .priority = 2130706431};
    strcpy(candidate.foundation, "1");
    ASSERT_EQ(rtc_addr_from_string(&candidate.addr, "127.0.0.1", 40000), RTC_OK);
    ASSERT_EQ(rtc_sdp_add_candidate(&sdp, &candidate), RTC_OK);
    ASSERT_EQ(rtc_sdp_generate(&sdp), RTC_OK);
    ASSERT(strstr(sdp.raw, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel") != NULL);
    ASSERT(strstr(sdp.raw, "a=candidate:") < strstr(sdp.raw, "m=application"));
    ASSERT_EQ(rtc_sdp_parse(&parsed, sdp.raw, sdp.raw_len), RTC_OK);
    ASSERT_EQ(parsed.media[0].direction, 2);
    ASSERT(parsed.ice_lite);
    ASSERT_EQ(parsed.media[1].sctp_port, 6000);
    ASSERT_EQ(parsed.media[1].max_message_size, 65535);
    ASSERT_EQ(strlen(parsed.ice_ufrag), 128);
    ASSERT_EQ(strlen(parsed.ice_pwd), 128);
    rtc_sdp_close(&parsed);
    rtc_sdp_close(&sdp);
}

int main(int argc, char **argv) {
    rtc_init();
    rtc_set_log_level(RTC_LOG_WARN);
    if (argc > 1 && strcmp(argv[1], "--turn") == 0) {
        if (!getenv("MRTC_TEST_TURN_PORT")) {
            fprintf(stderr, "SKIP: controlled coturn fixture unavailable\n");
            rtc_cleanup();
            return 77;
        }
        RUN_TEST(runtime_relay_only_coturn);
    } else {
        RUN_TEST(runtime_mdns_candidates_wait_for_authenticated_peer_reflexive_path);
        RUN_TEST(runtime_roles_restart_and_consent);
        RUN_TEST(runtime_fingerprint_mismatch);
        RUN_TEST(runtime_checked_wire_retransmit);
        RUN_TEST(runtime_controlled_role_conflict_and_missing_pin);
        RUN_TEST(runtime_rejects_unauthenticated_nomination);
        RUN_TEST(runtime_sdp_negotiation_fields);
        RUN_TEST(runtime_rejects_invalid_turn_address_lists);
        RUN_TEST(runtime_destroy_during_packet_callback);
    }
    rtc_cleanup();
    TEST_SUMMARY();
}
