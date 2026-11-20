/*
 * Peer connection lifecycle tests (new WebRTC-style API).
 *
 * Tests:
 *   1. Create / close / destroy lifecycle
 *   2. Add track creates sender with correct codec
 *   3. Signaling state transitions on set_local_desc
 *   4. Create offer generates valid SDP
 *   5. ICE candidates fired via on_ice_candidate callback
 *   6. Full offer/answer exchange between two peers
 */
#include <rtc/rtc_client.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_media_session.h>
#include <rtc/rtc_transport.h>
#include <rtc/rtc_worker.h>
#include <rtc/rtc_sdp.h>
#include <rtc/rtc_rtp_ext.h>
#include "test_harness.h"
#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

TEST(peer_create_destroy) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(pc), (int)RTC_SIGNALING_STABLE);
    ASSERT_EQ((int)rtc_peer_connection_connection_state(pc), (int)RTC_CONNECTION_NEW);

    rtc_peer_connection_close(pc);
    ASSERT_EQ((int)rtc_peer_connection_connection_state(pc), (int)RTC_CONNECTION_CLOSED);

    rtc_peer_connection_destroy(pc);
    printf("    create -> close -> destroy OK\n");
}

TEST(shared_runtime_has_room_for_eight_relay_sockets) {
    rtc_config_t config = {.ice_server_count = 1};
    config.ice_servers[0] = (rtc_ice_server_t){.urls = {"turn:127.0.0.1:9?transport=udp"},
                                               .url_count = 1,
                                               .username = "test",
                                               .credential = "test"};
    rtc_peer_connection_t *peers[8] = {0};
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
        peers[i] = rtc_peer_connection_create(&config);
        ASSERT(peers[i] != NULL);
    }
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
        rtc_peer_connection_close(peers[i]);
        rtc_peer_connection_destroy(peers[i]);
    }
}

TEST(peer_turn_url_budget_and_relay_family) {
    rtc_config_t config = {.ice_server_count = 2};
    config.ice_servers[0] = (rtc_ice_server_t){.urls = {"turn:127.0.0.1:9", "turn:[::1]:9?"
                                                                            "transport=tcp"},
                                               .url_count = 2,
                                               .username = "first",
                                               .credential = "first-secret"};
    config.ice_servers[1] = (rtc_ice_server_t){.urls = {"turn:127.0.0.2:9", "turns:localhost:9"},
                                               .url_count = 2,
                                               .username = "second",
                                               .credential = "second-secret"};
    rtc_peer_connection_t *peer = rtc_peer_connection_create(&config);
    ASSERT(peer != NULL);
    rtc_peer_connection_close(peer);
    rtc_peer_connection_destroy(peer);
    config.ice_servers[1].urls[2] = "turn:127.0.0.3:9";
    config.ice_servers[1].url_count = 3;
    ASSERT(rtc_peer_connection_create(&config) == NULL);
    config.ice_servers[1].url_count = 2;
    config.ice_servers[1].relay_family = AF_INET6;
    ASSERT(rtc_peer_connection_create(&config) == NULL);
    config.ice_servers[0].relay_family = -1;
    ASSERT(rtc_peer_connection_create(&config) == NULL);
}

TEST(peer_rejects_duplicate_remote_sources_without_mutation) {
    rtc_peer_connection_t *offerer = rtc_peer_connection_create(NULL);
    rtc_peer_connection_t *answerer = rtc_peer_connection_create(NULL);
    ASSERT(offerer && answerer);
    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    ASSERT(rtc_peer_connection_add_track(offerer, RTC_KIND_AUDIO, &opus) != NULL);
    rtc_desc_t offer;
    ASSERT_EQ(rtc_peer_connection_create_offer(offerer, &offer), RTC_OK);
    rtc_sdp_t duplicate = {0};
    ASSERT_EQ(rtc_sdp_parse(&duplicate, offer.sdp, offer.sdp_len), RTC_OK);
    duplicate.media[1] = duplicate.media[0];
    duplicate.media[1].mid_index = 1;
    duplicate.media_count = 2;
    ASSERT_EQ(rtc_sdp_generate(&duplicate), RTC_OK);
    memcpy(offer.sdp, duplicate.raw, duplicate.raw_len + 1);
    offer.sdp_len = duplicate.raw_len;
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(answerer, &offer), RTC_ERR_SDP);
    ASSERT_EQ(rtc_peer_connection_signaling_state(answerer), RTC_SIGNALING_STABLE);
    ASSERT_EQ(rtc_peer_connection_connection_state(answerer), RTC_CONNECTION_NEW);
    ASSERT(rtc_peer_connection_remote_desc(answerer) == NULL);
    duplicate.media_count = 1;
    duplicate.media[0].extmaps[0].id = 15;
    ASSERT_EQ(rtc_sdp_generate(&duplicate), RTC_OK);
    memcpy(offer.sdp, duplicate.raw, duplicate.raw_len + 1);
    offer.sdp_len = duplicate.raw_len;
    ASSERT(rtc_peer_connection_set_remote_desc(answerer, &offer) != RTC_OK);
    ASSERT_EQ(rtc_peer_connection_signaling_state(answerer), RTC_SIGNALING_STABLE);
    ASSERT(rtc_peer_connection_remote_desc(answerer) == NULL);
    rtc_sdp_close(&duplicate);
    rtc_peer_connection_close(offerer);
    rtc_peer_connection_close(answerer);
    rtc_peer_connection_destroy(offerer);
    rtc_peer_connection_destroy(answerer);
}

TEST(peer_add_track) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;

    rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);
    ASSERT(sender != NULL);

    const rtc_codec_t *c = rtc_rtp_sender_get_codec(sender);
    ASSERT(c != NULL);
    ASSERT_EQ(c->payload_type, 111);
    ASSERT_EQ(c->clock_rate, 48000);
    ASSERT_EQ(c->channels, 2);
    ASSERT_EQ((int)rtc_rtp_sender_kind(sender), (int)RTC_KIND_AUDIO);

    printf("    add_track: codec pt=%d rate=%u ch=%d\n", c->payload_type, c->clock_rate,
           c->channels);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

TEST(peer_create_offer) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);

    rtc_desc_t offer;
    int rc = rtc_peer_connection_create_offer(pc, &offer);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(offer.sdp_len > 0);
    ASSERT_EQ((int)offer.type, (int)RTC_SDP_OFFER);

    /* Verify key SDP content */
    ASSERT(strstr(offer.sdp, "v=0") != NULL);
    ASSERT(strstr(offer.sdp, "a=ice-ufrag:") != NULL);
    ASSERT(strstr(offer.sdp, "opus") != NULL);

    printf("    offer: %zu bytes of SDP\n", offer.sdp_len);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

TEST(peer_signaling_states) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);

    rtc_desc_t offer;
    rtc_peer_connection_create_offer(pc, &offer);

    ASSERT_EQ((int)rtc_peer_connection_signaling_state(pc), (int)RTC_SIGNALING_STABLE);

    /* set_local_desc(offer) → have-local-offer */
    int rc = rtc_peer_connection_set_local_desc(pc, &offer);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(pc), (int)RTC_SIGNALING_HAVE_LOCAL_OFFER);

    printf("    stable → have-local-offer OK\n");

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

static int g_ice_cand_count;
static bool g_ice_complete;

static void test_on_ice_candidate(const rtc_ice_candidate_desc_t *cand, void *user) {
    (void)user;
    if (cand) {
        g_ice_cand_count++;
    } else {
        g_ice_complete = true;
    }
}

TEST(peer_ice_candidates) {
    g_ice_cand_count = 0;
    g_ice_complete = false;

    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);

    rtc_peer_connection_on_ice_candidate(pc, test_on_ice_candidate, NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);

    rtc_desc_t offer;
    rtc_peer_connection_create_offer(pc, &offer);
    rtc_peer_connection_set_local_desc(pc, &offer);

    /* Candidates should have been fired synchronously in set_local_desc */
    ASSERT(g_ice_cand_count > 0);
    ASSERT(g_ice_complete);

    printf("    %d ICE candidate(s) fired, end-of-candidates received\n", g_ice_cand_count);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

TEST(peer_add_ice_candidate) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_ice_candidate_desc_t cand;
    memset(&cand, 0, sizeof(cand));
    strcpy(cand.candidate, "candidate:H0 1 udp 2130706431 127.0.0.1 50000 typ host");
    strcpy(cand.mid, "0");
    cand.mid_index = 0;

    ASSERT_EQ(rtc_peer_connection_add_ice_candidate(pc, &cand), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_add_ice_candidate(pc, NULL), RTC_OK);

    strcpy(cand.candidate, "candidate:S0 1 udp 1694498815 203.0.113.10 40000 typ srflx "
                           "raddr 10.0.0.2 rport 50000");
    ASSERT_EQ(rtc_peer_connection_add_ice_candidate(pc, &cand), RTC_OK);

    strcpy(cand.candidate, "candidate:bad");
    ASSERT(rtc_peer_connection_add_ice_candidate(pc, &cand) != RTC_OK);

    printf("    trickle candidate accepted and invalid candidate rejected\n");

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

static bool sdp_get_ice_ufrag(const char *sdp, char *out, size_t out_len) {
    const char *p = strstr(sdp, "a=ice-ufrag:");
    if (!p || !out || out_len == 0)
        return false;
    p += strlen("a=ice-ufrag:");
    const char *end = strpbrk(p, "\r\n");
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return len > 0;
}

static bool sdp_get_first_candidate_port(const char *sdp, uint16_t *out_port) {
    const char *p = strstr(sdp, "a=candidate:");
    if (!p || !out_port)
        return false;

    char foundation[32] = {0};
    char transport[8] = {0};
    char ip[64] = {0};
    char type[16] = {0};
    int component = 0;
    unsigned int priority = 0;
    unsigned int port = 0;
    int parsed = sscanf(p, "a=candidate:%31s %d %7s %u %63s %u typ %15s", foundation, &component,
                        transport, &priority, ip, &port, type);
    if (parsed != 7 || port == 0 || port > 65535)
        return false;
    *out_port = (uint16_t)port;
    return true;
}

TEST(peer_shared_runtime_listener) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc1 = rtc_peer_connection_create(&config);
    rtc_peer_connection_t *pc2 = rtc_peer_connection_create(&config);
    ASSERT(pc1 != NULL);
    ASSERT(pc2 != NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_peer_connection_add_track(pc1, RTC_KIND_AUDIO, &opus);
    rtc_peer_connection_add_track(pc2, RTC_KIND_AUDIO, &opus);

    rtc_desc_t offer1;
    rtc_desc_t offer2;
    ASSERT_EQ(rtc_peer_connection_create_offer(pc1, &offer1), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_create_offer(pc2, &offer2), RTC_OK);

    uint16_t port1 = 0;
    uint16_t port2 = 0;
    ASSERT(sdp_get_first_candidate_port(offer1.sdp, &port1));
    ASSERT(sdp_get_first_candidate_port(offer2.sdp, &port2));
    ASSERT_EQ(port1, port2);

    char ufrag1[32];
    char ufrag2[32];
    ASSERT(sdp_get_ice_ufrag(offer1.sdp, ufrag1, sizeof(ufrag1)));
    ASSERT(sdp_get_ice_ufrag(offer2.sdp, ufrag2, sizeof(ufrag2)));
    ASSERT(strcmp(ufrag1, ufrag2) != 0);

    printf("    shared runtime port=%u ufrag=%s/%s\n", (unsigned)port1, ufrag1, ufrag2);

    rtc_peer_connection_close(pc1);
    rtc_peer_connection_close(pc2);
    rtc_peer_connection_destroy(pc1);
    rtc_peer_connection_destroy(pc2);
}

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_transport_t *transport;
} peer_remote_env_t;

static bool make_remote_env(peer_remote_env_t *env) {
    memset(env, 0, sizeof(*env));
    env->worker = rtc_worker_create(NULL);
    if (!env->worker)
        return false;
    env->listener = rtc_listener_create(env->worker, NULL);
    if (!env->listener)
        return false;
    env->transport = rtc_transport_create(env->worker, &(rtc_transport_config_t){
                                                           .listener = env->listener,
                                                           .ice_mode = RTC_ICE_MODE_LITE,
                                                       });
    return env->transport != NULL;
}

static void close_remote_env(peer_remote_env_t *env) {
    rtc_transport_destroy(env->transport);
    rtc_listener_destroy(env->listener);
    rtc_worker_destroy(env->worker);
}

static bool add_loopback_candidate(rtc_sdp_t *sdp, rtc_listener_t *listener) {
    rtc_addr_t local;
    if (rtc_listener_get_local_addr(listener, &local) != RTC_OK)
        return false;
    uint16_t port = ntohs(((struct sockaddr_in *)&local.addr)->sin_port);

    rtc_ice_candidate_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.type = ICE_CANDIDATE_HOST;
    candidate.component = 1;
    candidate.priority = 2130706431u;
    memcpy(candidate.foundation, "H0", sizeof("H0"));
    if (rtc_addr_from_string(&candidate.addr, "127.0.0.1", port) != RTC_OK)
        return false;
    return rtc_sdp_add_candidate(sdp, &candidate) == RTC_OK;
}

static bool build_runtime_answer(peer_remote_env_t *remote, const rtc_desc_t *offer,
                                 rtc_desc_t *answer) {
    rtc_sdp_t parsed_offer;
    memset(&parsed_offer, 0, sizeof(parsed_offer));
    if (rtc_sdp_parse(&parsed_offer, offer->sdp, offer->sdp_len) != RTC_OK)
        return false;
    rtc_dtls_parameters_t remote_dtls = {.role = RTC_TRANSPORT_DTLS_ROLE_CLIENT};
    snprintf(remote_dtls.fingerprint, sizeof(remote_dtls.fingerprint), "%s",
             parsed_offer.fingerprint);
    rtc_ice_parameters_t remote_ice = {.mode = RTC_ICE_MODE_FULL};
    snprintf(remote_ice.username_fragment, sizeof(remote_ice.username_fragment), "%s",
             parsed_offer.ice_ufrag);
    snprintf(remote_ice.password, sizeof(remote_ice.password), "%s", parsed_offer.ice_pwd);
    if (rtc_transport_set_remote_dtls_parameters(remote->transport, &remote_dtls) != RTC_OK ||
        rtc_transport_set_remote_ice_parameters(remote->transport, &remote_ice) != RTC_OK) {
        rtc_sdp_close(&parsed_offer);
        return false;
    }

    rtc_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp.type = RTC_SDP_ANSWER;
    sdp.setup = RTC_SETUP_PASSIVE;
    sdp.media_type = parsed_offer.media_type;
    sdp.payload_type = parsed_offer.payload_type;
    sdp.clockrate = parsed_offer.clockrate;
    sdp.channels = parsed_offer.channels;
    memcpy(sdp.codec_name, parsed_offer.codec_name, sizeof(sdp.codec_name));
    sdp.media_count = parsed_offer.media_count;
    if (sdp.media_count > 0)
        memcpy(sdp.media, parsed_offer.media, sizeof(rtc_sdp_media_t) * (size_t)sdp.media_count);
    for (int i = 0; i < sdp.media_count; i++) {
        rtc_sdp_media_t *m = &sdp.media[i];
        m->direction = m->direction == 1 ? 2 : m->direction == 2 ? 1 : m->direction;
        for (int j = 0; j < m->extmap_count; j++)
            m->extmaps[j].direction = RTC_SDP_EXT_SENDRECV;
        rtc_sdp_media_answer_extmaps(m, &parsed_offer.media[i]);
    }

    rtc_ice_parameters_t ice;
    rtc_dtls_parameters_t dtls;
    bool ok = rtc_transport_get_ice_parameters(remote->transport, &ice) == RTC_OK &&
              rtc_transport_get_dtls_parameters(remote->transport, &dtls) == RTC_OK;
    if (ok) {
        memcpy(sdp.ice_ufrag, ice.username_fragment, sizeof(sdp.ice_ufrag));
        memcpy(sdp.ice_pwd, ice.password, sizeof(sdp.ice_pwd));
        memcpy(sdp.fingerprint, dtls.fingerprint, sizeof(sdp.fingerprint));
        ok = add_loopback_candidate(&sdp, remote->listener) && rtc_sdp_generate(&sdp) == RTC_OK;
    }

    if (ok) {
        memset(answer, 0, sizeof(*answer));
        answer->type = RTC_SDP_ANSWER;
        memcpy(answer->sdp, sdp.raw, sdp.raw_len);
        answer->sdp_len = sdp.raw_len;
    }

    rtc_sdp_close(&sdp);
    rtc_sdp_close(&parsed_offer);
    return ok;
}

static bool wait_for_runtime_connected(rtc_transport_t *transport, int timeout_ms) {
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

static _Atomic rtc_connection_state_t g_alice_state;

static void alice_on_conn(rtc_connection_state_t state, void *user) {
    (void)user;
    g_alice_state = state;
}

static void count_received_frame(const uint8_t *data, size_t len, uint16_t seq, uint32_t timestamp,
                                 uint32_t ssrc, bool marker, void *user) {
    (void)data;
    (void)len;
    (void)seq;
    (void)timestamp;
    (void)ssrc;
    (void)marker;
    atomic_uint *frames = user;
    atomic_fetch_add(frames, 1);
}

static void receive_runtime_track(rtc_rtp_receiver_t *receiver, void *user) {
    rtc_rtp_receiver_on_frame(receiver, count_received_frame, user);
}

typedef struct {
    atomic_uint packets, feedback;
    atomic_bool mid, cc;
} extension_wire_t;

static void extension_wire_packet(const rtc_rtp_packet_t *packet, void *user) {
    extension_wire_t *wire = user;
    rtc_rtp_ext_t ext[8];
    size_t count = 8;
    bool mid = false, cc = false;
    if (packet->ext_data &&
        rtc_rtp_ext_parse_body(packet->ext_data, packet->ext_len, ext, &count) == RTC_OK) {
        mid = rtc_rtp_ext_find(ext, count, 4) != NULL;
        cc = rtc_rtp_ext_find(ext, count, 5) != NULL;
    }
    atomic_store(&wire->mid, mid);
    atomic_store(&wire->cc, cc);
    atomic_fetch_add(&wire->packets, 1);
}

static void extension_wire_feedback(const uint8_t *data, size_t len, void *user) {
    extension_wire_t *wire = user;
    for (size_t off = 0; off + 4 <= len;) {
        size_t size = ((size_t)data[off + 2] * 256 + data[off + 3] + 1) * 4;
        if (size > len - off)
            break;
        if (data[off + 1] == RTCP_PT_RTPFB && (data[off] & 31) == 15)
            atomic_fetch_add(&wire->feedback, 1);
        off += size;
    }
}

TEST(peer_extension_directions_govern_encrypted_wire_and_feedback) {
    for (int direction = RTC_SDP_EXT_SENDONLY; direction <= RTC_SDP_EXT_INACTIVE; direction++) {
        rtc_peer_connection_t *peer = rtc_peer_connection_create(NULL);
        ASSERT(peer);
        peer_remote_env_t remote;
        ASSERT(make_remote_env(&remote));
        extension_wire_t wire = {0};
        rtc_transport_on_rtp(remote.transport, extension_wire_packet, &wire);
        rtc_transport_on_rtcp(remote.transport, extension_wire_feedback, &wire);
        atomic_uint received = 0;
        rtc_peer_connection_on_track(peer, receive_runtime_track, &received);
        rtc_codec_t opus = {
            .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
        rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(peer, RTC_KIND_AUDIO, &opus);
        ASSERT(sender);
        rtc_desc_t offer, answer;
        ASSERT_EQ(rtc_peer_connection_create_offer(peer, &offer), RTC_OK);
        rtc_sdp_t parsed = {0};
        ASSERT_EQ(rtc_sdp_parse(&parsed, offer.sdp, offer.sdp_len), RTC_OK);
        for (int i = 0; i < parsed.media[0].extmap_count; i++)
            parsed.media[0].extmaps[i].direction = (rtc_sdp_ext_direction_t)direction;
        ASSERT_EQ(rtc_sdp_generate(&parsed), RTC_OK);
        memcpy(offer.sdp, parsed.raw, parsed.raw_len + 1);
        offer.sdp_len = parsed.raw_len;
        rtc_sdp_close(&parsed);
        ASSERT_EQ(rtc_peer_connection_set_local_desc(peer, &offer), RTC_OK);
        ASSERT(build_runtime_answer(&remote, &offer, &answer));
        memset(&parsed, 0, sizeof(parsed));
        ASSERT_EQ(rtc_sdp_parse(&parsed, answer.sdp, answer.sdp_len), RTC_OK);
        parsed.media[0].ssrc = 0x44556677;
        ASSERT_EQ(rtc_sdp_generate(&parsed), RTC_OK);
        memcpy(answer.sdp, parsed.raw, parsed.raw_len + 1);
        answer.sdp_len = parsed.raw_len;
        rtc_sdp_close(&parsed);
        ASSERT_EQ(rtc_peer_connection_set_remote_desc(peer, &answer), RTC_OK);
        uint64_t deadline = rtc_time_ms() + 3000;
        while (rtc_peer_connection_connection_state(peer) != RTC_CONNECTION_CONNECTED &&
               rtc_time_ms() < deadline)
            SLEEP_MS(5);
        ASSERT_EQ(rtc_peer_connection_connection_state(peer), RTC_CONNECTION_CONNECTED);
        ASSERT(wait_for_runtime_connected(remote.transport, 1000));
        const uint8_t audio[] = {0xf8, 0xff, 0xfe};
        ASSERT_EQ(rtc_rtp_sender_send(sender, audio, sizeof(audio), 960, true), RTC_OK);
        deadline = rtc_time_ms() + 1000;
        while (!atomic_load(&wire.packets) && rtc_time_ms() < deadline)
            SLEEP_MS(5);
        ASSERT_EQ(atomic_load(&wire.packets), 1);
        ASSERT_EQ(atomic_load(&wire.mid), direction == RTC_SDP_EXT_SENDONLY);
#ifdef MRTC_ENABLE_TWCC
        ASSERT_EQ(atomic_load(&wire.cc), direction == RTC_SDP_EXT_SENDONLY);
#endif
        rtc_rtp_ext_t ext[2];
        rtc_rtp_ext_make_string(&ext[0], 4, "0");
        rtc_rtp_ext_make_transport_cc(&ext[1], 5, 7);
        rtc_rtp_packet_t packet;
        ASSERT_EQ(rtc_rtp_build_with_ext(&packet, 111, 1, 960, 0x44556677, true, ext, 2, audio,
                                         sizeof(audio)),
                  RTC_OK);
        size_t len = packet.buf_len;
        ASSERT_EQ(rtc_transport_send_rtp(remote.transport, packet.buf, &len, sizeof(packet.buf)),
                  RTC_OK);
        deadline = rtc_time_ms() + 1000;
        while (!atomic_load(&received) && rtc_time_ms() < deadline)
            SLEEP_MS(5);
        ASSERT_EQ(atomic_load(&received), 1);
        SLEEP_MS(180);
#ifdef MRTC_ENABLE_TWCC
        ASSERT_EQ(atomic_load(&wire.feedback) > 0, direction == RTC_SDP_EXT_RECVONLY);
#endif
        rtc_peer_connection_close(peer);
        rtc_peer_connection_destroy(peer);
        close_remote_env(&remote);
    }
}

typedef struct {
    rtc_transport_t *transport;
    int rc;
} source_burst_t;

static void send_source_burst(void *user) {
    source_burst_t *burst = user;
    const uint8_t audio[] = {0xf8, 0xff, 0xfe};
    for (unsigned i = 0; i < RTC_MEDIA_SESSION_RETIRED_MAX + 2; i++) {
        rtc_rtp_packet_t packet;
        burst->rc = rtc_rtp_build(&packet, 111, 1, 960, 0x10000 + i, true, audio, sizeof(audio));
        if (burst->rc != RTC_OK)
            return;
        size_t len = packet.buf_len;
        burst->rc = rtc_transport_send_rtp(burst->transport, packet.buf, &len, sizeof(packet.buf));
        if (burst->rc != RTC_OK)
            return;
    }
}

TEST(peer_connects_and_surfaces_source_retirement_exhaustion) {
    g_alice_state = RTC_CONNECTION_NEW;

    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *alice = rtc_peer_connection_create(&config);
    ASSERT(alice != NULL);
    peer_remote_env_t remote;
    ASSERT(make_remote_env(&remote));

    rtc_peer_connection_on_connection_state(alice, alice_on_conn, NULL);
    atomic_uint received = 0;
    rtc_peer_connection_on_track(alice, receive_runtime_track, &received);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;

    rtc_peer_connection_add_track(alice, RTC_KIND_AUDIO, &opus);

    /* Alice creates offer */
    rtc_desc_t offer;
    int rc = rtc_peer_connection_create_offer(alice, &offer);
    ASSERT_EQ(rc, RTC_OK);

    rtc_peer_connection_set_local_desc(alice, &offer);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(alice), (int)RTC_SIGNALING_HAVE_LOCAL_OFFER);

    rtc_desc_t answer;
    ASSERT(build_runtime_answer(&remote, &offer, &answer));

    /* Alice receives answer → connection starts automatically */
    rtc_peer_connection_set_remote_desc(alice, &answer);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(alice), (int)RTC_SIGNALING_STABLE);

    /* Wait for both sides to connect (up to 10 seconds) */
    for (int i = 0; i < 200; i++) {
        if (g_alice_state == RTC_CONNECTION_CONNECTED &&
            wait_for_runtime_connected(remote.transport, 1))
            break;
        if (g_alice_state == RTC_CONNECTION_FAILED)
            break;
        SLEEP_MS(50);
    }

    printf("    alice: %d  remote_dtls=%d\n", (int)g_alice_state,
           (int)wait_for_runtime_connected(remote.transport, 1));
    ASSERT_EQ((int)g_alice_state, (int)RTC_CONNECTION_CONNECTED);
    ASSERT(wait_for_runtime_connected(remote.transport, 100));

    printf("    peer connected via auto-connect\n");

    source_burst_t burst = {.transport = remote.transport};
    ASSERT_EQ(rtc_transport_invoke(remote.transport, send_source_burst, &burst), RTC_OK);
    ASSERT_EQ(burst.rc, RTC_OK);
    uint64_t deadline = rtc_time_ms() + 2000;
    while (g_alice_state != RTC_CONNECTION_FAILED && rtc_time_ms() < deadline)
        SLEEP_MS(10);
    bool failed = g_alice_state == RTC_CONNECTION_FAILED;
    unsigned frames = atomic_load(&received);

    rtc_peer_connection_close(alice);
    rtc_peer_connection_destroy(alice);
    close_remote_env(&remote);
    ASSERT(frames > 0);
    ASSERT(failed);
}

TEST(peer_get_stats) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);
    ASSERT(sender != NULL);

    /* Snapshot before any traffic: counters all zero, ssrc populated. */
    rtc_stats_report_t report;
    int rc = rtc_peer_connection_get_stats(pc, &report);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(report.transceiver_count, 1);
    ASSERT_EQ((int)report.transceivers[0].kind, (int)RTC_KIND_AUDIO);
    ASSERT(report.transceivers[0].out_ssrc != 0);
    ASSERT_EQ((int)report.transceivers[0].out_packets_sent, 0);
    ASSERT_EQ((int)report.transceivers[0].in_packets_received, 0);

    /* NULL args */
    ASSERT_EQ(rtc_peer_connection_get_stats(NULL, &report), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_peer_connection_get_stats(pc, NULL), RTC_ERR_INVALID);

    printf("    stats: txn=%d mid=%s out_ssrc=%u\n", report.transceiver_count,
           report.transceivers[0].mid, report.transceivers[0].out_ssrc);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

TEST(peer_add_transceiver_remove_track) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;

    /* addTransceiver with sendonly */
    rtc_rtp_transceiver_init_t init = {.direction = RTC_DIR_SENDONLY};
    rtc_rtp_transceiver_t *t1 =
        rtc_peer_connection_add_transceiver(pc, RTC_KIND_AUDIO, &opus, &init);
    ASSERT(t1 != NULL);
    ASSERT_EQ((int)rtc_rtp_transceiver_direction(t1), (int)RTC_DIR_SENDONLY);

    /* addTransceiver with default (NULL init -> sendrecv from init_slot) */
    rtc_rtp_transceiver_t *t2 =
        rtc_peer_connection_add_transceiver(pc, RTC_KIND_AUDIO, &opus, NULL);
    ASSERT(t2 != NULL);
    ASSERT_EQ((int)rtc_rtp_transceiver_direction(t2), (int)RTC_DIR_SENDRECV);

    /* removeTrack: sendonly -> inactive */
    rtc_rtp_sender_t *s1 = rtc_rtp_transceiver_sender(t1);
    ASSERT_EQ(rtc_peer_connection_remove_track(pc, s1), RTC_OK);
    ASSERT_EQ((int)rtc_rtp_transceiver_direction(t1), (int)RTC_DIR_INACTIVE);

    /* removeTrack: sendrecv -> recvonly */
    rtc_rtp_sender_t *s2 = rtc_rtp_transceiver_sender(t2);
    ASSERT_EQ(rtc_peer_connection_remove_track(pc, s2), RTC_OK);
    ASSERT_EQ((int)rtc_rtp_transceiver_direction(t2), (int)RTC_DIR_RECVONLY);

    /* removeTrack of a foreign sender fails. rtc_rtp_sender_t is opaque
     * to public users, so use any non-NULL address that is not a real
     * sender; remove_track must reject without crashing. */
    int sentinel = 0;
    ASSERT_EQ(rtc_peer_connection_remove_track(pc, (rtc_rtp_sender_t *)&sentinel), RTC_ERR_INVALID);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

TEST(peer_sender_parameters) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_codec_t vp8;
    memset(&vp8, 0, sizeof(vp8));
    vp8.payload_type = 96;
    strcpy(vp8.mime_type, "video/VP8");
    vp8.clock_rate = 90000;
    rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(pc, RTC_KIND_VIDEO, &vp8);
    ASSERT(sender != NULL);

    /* Defaults: active=true, max_bitrate_bps=0 */
    rtc_rtp_send_params_t params;
    ASSERT_EQ(rtc_rtp_sender_get_parameters(sender, &params), RTC_OK);
    ASSERT_EQ(params.encoding_count, 1);
    ASSERT(params.encodings[0].active);
    ASSERT_EQ((int)params.encodings[0].max_bitrate_bps, 0);

    /* Cap at 800 kbps */
    params.encodings[0].max_bitrate_bps = 800000;
    ASSERT_EQ(rtc_rtp_sender_set_parameters(sender, &params), RTC_OK);
    /* get_target_bitrate clamps; if rate ctrl says >800 we expect 800;
     * if it returns 0 (no estimate yet) we expect the cap as a floor in
     * the no-rate-ctrl branch. Either way it must be <= 800. */
    int t = rtc_rtp_sender_get_target_bitrate(sender);
    ASSERT(t == 0 || t <= 800);

    /* Suspend: sender_send must accept but not crash; we can't easily
     * observe the wire (no SRTP yet), so just check the API contract. */
    params.encodings[0].active = false;
    ASSERT_EQ(rtc_rtp_sender_set_parameters(sender, &params), RTC_OK);
    ASSERT_EQ(rtc_rtp_sender_get_parameters(sender, &params), RTC_OK);
    ASSERT(!params.encodings[0].active);

    /* Validation: NULL / empty params rejected. */
    ASSERT_EQ(rtc_rtp_sender_get_parameters(NULL, &params), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_rtp_sender_set_parameters(sender, NULL), RTC_ERR_INVALID);
    rtc_rtp_send_params_t empty = {0};
    ASSERT_EQ(rtc_rtp_sender_set_parameters(sender, &empty), RTC_ERR_INVALID);

    printf("    send params: cap=%u target=%d\n", 800000, t);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

TEST(peer_identity_getters) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    /* Local fingerprint is always available after create (DTLS cert
     * is generated up-front). */
    const char *fp = rtc_peer_connection_local_fingerprint(pc);
    ASSERT(fp != NULL && fp[0] != '\0');
    /* Sanity: SHA-256 hex with colons is exactly 95 chars (32 bytes * 2
     * hex + 31 colons). */
    ASSERT(strlen(fp) == 95);

    /* Before set_remote_desc: remote fingerprint is "", trickle is false. */
    ASSERT(rtc_peer_connection_remote_fingerprint(pc)[0] == '\0');
    ASSERT(!rtc_peer_connection_can_trickle_ice_candidates(pc));

    /* Locally generated offer includes a=ice-options:trickle. */
    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);
    rtc_desc_t offer;
    ASSERT_EQ(rtc_peer_connection_create_offer(pc, &offer), RTC_OK);
    ASSERT(strstr(offer.sdp, "a=ice-options:trickle") != NULL);

    /* Round-trip: feeding the offer back in as remote desc must flip
     * can_trickle and populate the remote fingerprint. */
    rtc_peer_connection_t *pc2 = rtc_peer_connection_create(&config);
    ASSERT(pc2 != NULL);
    rtc_peer_connection_add_track(pc2, RTC_KIND_AUDIO, &opus);
    offer.type = RTC_SDP_OFFER;
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(pc2, &offer), RTC_OK);
    ASSERT(rtc_peer_connection_can_trickle_ice_candidates(pc2));
    ASSERT(strlen(rtc_peer_connection_remote_fingerprint(pc2)) > 0);

    printf("    local_fp=%.20s... trickle=%d\n", fp,
           (int)rtc_peer_connection_can_trickle_ice_candidates(pc2));

    rtc_peer_connection_close(pc);
    rtc_peer_connection_close(pc2);
    rtc_peer_connection_destroy(pc);
    rtc_peer_connection_destroy(pc2);
}

TEST(peer_restart_ice) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    ASSERT(pc != NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;
    rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);

    rtc_desc_t offer1;
    ASSERT_EQ(rtc_peer_connection_create_offer(pc, &offer1), RTC_OK);
    const char *u1 = strstr(offer1.sdp, "a=ice-ufrag:");
    ASSERT(u1 != NULL);
    char ufrag1[32] = {0};
    sscanf(u1, "a=ice-ufrag:%31[^\r\n]", ufrag1);

    /* Restart and re-offer; ufrag must change. */
    ASSERT_EQ(rtc_peer_connection_restart_ice(pc), RTC_OK);
    rtc_desc_t offer2;
    ASSERT_EQ(rtc_peer_connection_create_offer(pc, &offer2), RTC_OK);
    const char *u2 = strstr(offer2.sdp, "a=ice-ufrag:");
    ASSERT(u2 != NULL);
    char ufrag2[32] = {0};
    sscanf(u2, "a=ice-ufrag:%31[^\r\n]", ufrag2);

    ASSERT(strcmp(ufrag1, ufrag2) != 0);
    printf("    restart_ice: %s -> %s\n", ufrag1, ufrag2);

    /* NULL is invalid. */
    ASSERT_EQ(rtc_peer_connection_restart_ice(NULL), RTC_ERR_INVALID);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

int main(void) {
    printf("========================================\n");
    printf("  Peer Connection Tests (New API)\n");
    printf("========================================\n\n");

    rtc_client_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(peer_create_destroy);
    RUN_TEST(shared_runtime_has_room_for_eight_relay_sockets);
    RUN_TEST(peer_turn_url_budget_and_relay_family);
    RUN_TEST(peer_add_track);
    RUN_TEST(peer_rejects_duplicate_remote_sources_without_mutation);
    RUN_TEST(peer_create_offer);
    RUN_TEST(peer_signaling_states);
    RUN_TEST(peer_ice_candidates);
    RUN_TEST(peer_add_ice_candidate);
    RUN_TEST(peer_shared_runtime_listener);
    RUN_TEST(peer_extension_directions_govern_encrypted_wire_and_feedback);
    RUN_TEST(peer_connects_and_surfaces_source_retirement_exhaustion);
    RUN_TEST(peer_get_stats);
    RUN_TEST(peer_add_transceiver_remove_track);
    RUN_TEST(peer_sender_parameters);
    RUN_TEST(peer_identity_getters);
    RUN_TEST(peer_restart_ice);

    rtc_client_cleanup();
    TEST_SUMMARY();
}
