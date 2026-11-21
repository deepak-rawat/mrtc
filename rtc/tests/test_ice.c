#include <rtc/rtc.h>
#include "rtc_ice.h"
#include "rtc_worker_internal.h"
#include "test_harness.h"

typedef struct {
    rtc_ice_agent_t ice;
    rtc_listener_t *listener;
    rtc_mutex_t mutex;
    atomic_bool closed;
    rtc_stun_msg_t sent;
    rtc_addr_t destination;
    unsigned sends;
} ice_fixture_t;

static ice_fixture_t fixture;
static rtc_worker_t *worker;

static int capture_send(const uint8_t *data, size_t len, const rtc_addr_t *to, void *user) {
    ice_fixture_t *f = user;
    int rc = rtc_stun_parse(&f->sent, data, len);
    if (rc == RTC_OK) {
        f->destination = *to;
        f->sends++;
    }
    return rc;
}

static int prepare_peer(const rtc_addr_t *to, void *user) {
    (void)to;
    (void)user;
    return RTC_OK;
}

static void receive_packet(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                           const rtc_addr_t *from, void *user) {
    ice_fixture_t *f = user;
    if (type == RTC_PKT_STUN)
        rtc_ice_handle_stun(&f->ice, data, len, from);
}

static void run_case(void (*body)(void), rtc_ice_mode_t mode) {
    memset(&fixture, 0, sizeof(fixture));
    atomic_init(&fixture.closed, false);
    ASSERT_EQ(rtc_mutex_init(&fixture.mutex), RTC_OK);
    fixture.listener = rtc_listener_create(worker, NULL);
    if (!fixture.listener) {
        rtc_mutex_destroy(&fixture.mutex);
        ASSERT(fixture.listener != NULL);
    }
    rtc_ice_config_t config = {.worker = worker,
                               .listener = fixture.listener,
                               .send_mutex = &fixture.mutex,
                               .closed = &fixture.closed,
                               .send = capture_send,
                               .prepare_peer = prepare_peer,
                               .on_packet = receive_packet,
                               .user = &fixture,
                               .mode = mode};
    int rc = rtc_ice_init(&fixture.ice, &config);
    if (rc == RTC_OK)
        rc = rtc_ice_bind(&fixture.ice);
    if (rc == RTC_OK)
        body();
    atomic_store(&fixture.closed, true);
    rtc_ice_close(&fixture.ice);
    rtc_listener_destroy(fixture.listener);
    rtc_mutex_destroy(&fixture.mutex);
    ASSERT_EQ(rc, RTC_OK);
}

static rtc_ice_parameters_t remote_parameters(void) {
    rtc_ice_parameters_t remote = {.mode = RTC_ICE_MODE_FULL};
    strcpy(remote.username_fragment, "remoteFrag");
    strcpy(remote.password, "remotePassword0123456789");
    return remote;
}

static rtc_transport_candidate_t candidate(uint16_t port) {
    rtc_transport_candidate_t result = {.port = port};
    strcpy(result.address, "127.0.0.1");
    strcpy(result.protocol, "udp");
    strcpy(result.username_fragment, "remoteFrag");
    return result;
}

static int binding_response(rtc_stun_msg_t *response, const rtc_stun_msg_t *request,
                            const rtc_addr_t *from, const char *password) {
    memset(response, 0, sizeof(*response));
    int rc = rtc_stun_build_binding_response(request->buf, request->buf_len, from, response->buf,
                                             sizeof(response->buf), &response->buf_len);
    if (rc != RTC_OK)
        return rc;
    memcpy(response->txn_id, request->txn_id, STUN_TXN_ID_SIZE);
    response->type = STUN_BINDING_RESPONSE;
    return rtc_stun_finalize(response, password);
}

static void authenticated_response_and_local_restart(void) {
    rtc_ice_agent_t *ice = &fixture.ice;
    rtc_ice_parameters_t remote = remote_parameters();
    rtc_transport_candidate_t peer = candidate(40000);
    ASSERT_EQ(rtc_ice_set_remote_parameters(ice, &remote), RTC_OK);
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    ASSERT_EQ(rtc_ice_start(ice), RTC_OK);
    rtc_stun_msg_t request = fixture.sent, response;
    rtc_addr_t source = fixture.destination, wrong_source;
    ASSERT_EQ(rtc_addr_from_string(&wrong_source, "127.0.0.1", 40001), RTC_OK);
    ASSERT_EQ(binding_response(&response, &request, &source, "wrong-password"), RTC_OK);
    rtc_ice_handle_stun(ice, response.buf, response.buf_len, &source);
    ASSERT(!atomic_load(&ice->selected_remote_valid));
    ASSERT(ice->txn_registered);

    ASSERT_EQ(binding_response(&response, &request, &source, remote.password), RTC_OK);
    rtc_ice_handle_stun(ice, response.buf, response.buf_len, &wrong_source);
    ASSERT(!atomic_load(&ice->selected_remote_valid));
    ASSERT(ice->txn_registered);
    rtc_stun_msg_t wrong_request = request;
    wrong_request.buf[8] ^= 1;
    wrong_request.txn_id[0] ^= 1;
    ASSERT_EQ(binding_response(&response, &wrong_request, &source, remote.password), RTC_OK);
    rtc_ice_handle_stun(ice, response.buf, response.buf_len, &source);
    ASSERT(!atomic_load(&ice->selected_remote_valid));
    ASSERT(ice->txn_registered);

    ASSERT_EQ(binding_response(&response, &request, &source, remote.password), RTC_OK);
    rtc_ice_handle_stun(ice, response.buf, response.buf_len, &source);
    ASSERT(rtc_ice_accepts_media(ice, &source));
    ASSERT(!rtc_ice_accepts_media(ice, &wrong_source));
    ASSERT(!ice->txn_registered);
    rtc_ice_parameters_t before, after;
    rtc_ice_get_parameters(ice, &before);
    ASSERT_EQ(rtc_ice_restart(ice), RTC_OK);
    rtc_ice_get_parameters(ice, &after);
    ASSERT(strcmp(before.username_fragment, after.username_fragment) != 0);
    ASSERT(strcmp(before.password, after.password) != 0);
    ASSERT(ice->restarting && !ice->remote_ice_valid);
    ASSERT_EQ(ice->remote_candidate_count, 0);
    ASSERT(rtc_ice_can_send(ice));
    rtc_ice_handle_stun(ice, response.buf, response.buf_len, &source);
    ASSERT(ice->restarting);
}

TEST(ice_authenticated_response_and_local_restart) {
    run_case(authenticated_response_and_local_restart, RTC_ICE_MODE_FULL);
}

static void authenticated_nomination(void) {
    rtc_ice_agent_t *ice = &fixture.ice;
    rtc_ice_parameters_t remote = remote_parameters();
    ASSERT_EQ(rtc_ice_set_remote_parameters(ice, &remote), RTC_OK);
    ASSERT_EQ(rtc_ice_set_controlling(ice, true), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_ice_start(ice), RTC_ERR_INVALID);
    rtc_addr_t source;
    ASSERT_EQ(rtc_addr_from_string(&source, "127.0.0.1", 40000), RTC_OK);
    char username[RTC_ICE_UFRAG_MAX * 2 + 2];
    snprintf(username, sizeof(username), "%s:%s", ice->ufrag, remote.username_fragment);
    rtc_stun_msg_t request;
    ASSERT_EQ(rtc_stun_build_binding_request(&request, username, "wrong-password", 123, true, 123,
                                             true),
              RTC_OK);
    rtc_ice_handle_stun(ice, request.buf, request.buf_len, &source);
    ASSERT_EQ(fixture.sends, 0);
    ASSERT(!atomic_load(&ice->selected_remote_valid));

    ASSERT_EQ(rtc_stun_build_binding_request(&request, username, ice->pwd, 123, false, 123, true),
              RTC_OK);
    rtc_ice_handle_stun(ice, request.buf, request.buf_len, &source);
    ASSERT_EQ(fixture.sends, 1);
    ASSERT(!atomic_load(&ice->selected_remote_valid));
    ASSERT(ice->remote_candidate_valid);
    ASSERT_EQ(rtc_stun_verify_integrity(fixture.sent.buf, fixture.sent.buf_len, ice->pwd), RTC_OK);

    ASSERT_EQ(rtc_stun_build_binding_request(&request, username, ice->pwd, 123, true, 123, true),
              RTC_OK);
    rtc_ice_handle_stun(ice, request.buf, request.buf_len, &source);
    ASSERT(rtc_ice_accepts_media(ice, &source));
    ice->consent_expired = true;
    ASSERT(!rtc_ice_accepts_media(ice, &source));
    unsigned sends = fixture.sends;
    rtc_ice_handle_stun(ice, request.buf, request.buf_len, &source);
    ASSERT_EQ(fixture.sends, sends);
}

TEST(ice_authenticated_nomination) {
    run_case(authenticated_nomination, RTC_ICE_MODE_LITE);
}

static void candidate_rotation_and_remote_restart(void) {
    rtc_ice_agent_t *ice = &fixture.ice;
    rtc_ice_parameters_t remote = remote_parameters();
    ASSERT_EQ(rtc_ice_set_remote_parameters(ice, &remote), RTC_OK);
    rtc_transport_candidate_t peer = candidate(40000);
    strcpy(peer.protocol, "tcp");
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    strcpy(peer.protocol, "udp");
    strcpy(peer.address, "browser.local");
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    strcpy(peer.address, "::1");
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_INET), RTC_OK);
    ASSERT_EQ(ice->remote_candidate_count, 0);
    ASSERT_EQ(ice->remote_candidates_deferred, 1);
    ASSERT_EQ(ice->remote_candidates_ineligible, 2);

    peer = candidate(40000);
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    peer.port = 40001;
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    ASSERT_EQ(ice->remote_candidate_count, 2);
    ASSERT_EQ(rtc_ice_start(ice), RTC_OK);
    rtc_addr_t first = fixture.destination;
    ASSERT_EQ(rtc_ice_start(ice), RTC_OK);
    ASSERT(memcmp(&first, &fixture.destination, sizeof(first)) != 0);
    ASSERT(ice->txn_registered);

    strcpy(remote.username_fragment, "newRemoteFrag");
    strcpy(remote.password, "newRemotePassword0123456789");
    ASSERT_EQ(rtc_ice_set_remote_parameters(ice, &remote), RTC_OK);
    ASSERT(ice->restarting && !ice->txn_registered);
    ASSERT_EQ(ice->remote_candidate_count, 0);
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    ASSERT_EQ(ice->remote_candidate_count, 0);
    strcpy(peer.username_fragment, remote.username_fragment);
    ASSERT_EQ(rtc_ice_add_remote_candidate(ice, &peer, AF_UNSPEC), RTC_OK);
    ASSERT_EQ(ice->remote_candidate_count, 1);
    ASSERT_EQ(rtc_ice_start(ice), RTC_OK);
    ASSERT_EQ(rtc_stun_verify_integrity(fixture.sent.buf, fixture.sent.buf_len, remote.password),
              RTC_OK);
}

TEST(ice_candidate_rotation_and_remote_restart) {
    run_case(candidate_rotation_and_remote_restart, RTC_ICE_MODE_FULL);
}

static void run_suite(void *user) {
    (void)user;
    RUN_TEST(ice_authenticated_response_and_local_restart);
    RUN_TEST(ice_authenticated_nomination);
    RUN_TEST(ice_candidate_rotation_and_remote_restart);
}

int main(void) {
    if (rtc_init() != RTC_OK)
        return 1;
    rtc_set_log_level(RTC_LOG_WARN);
    worker = rtc_worker_create(NULL);
    if (!worker) {
        rtc_cleanup();
        return 1;
    }
    int rc = rtc_worker_invoke(worker, run_suite, NULL);
    rtc_worker_destroy(worker);
    rtc_cleanup();
    if (rc != RTC_OK)
        return 1;
    TEST_SUMMARY();
}
