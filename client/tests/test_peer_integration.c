#include <rtc/rtc_client.h>
#include <rtc/rtc_sdp.h>
#include "test_harness.h"
#include "rtc_peer_internal.h"
#include <rtc/rtc_transport.h>
#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

static const uint8_t opus_silence[] = {0xf8, 0xff, 0xfe};
/* A 16x16 black keyframe, encoded and decode-checked with libvpx. */
static const uint8_t vp8_black[] = {0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x10, 0x00,
                                    0x10, 0x00, 0x00, 0x47, 0x08, 0x85, 0x85, 0x88,
                                    0x99, 0x84, 0x88, 0x02, 0x02, 0x00, 0x0c, 0x0d,
                                    0x60, 0x00, 0xfe, 0xff, 0xab, 0x50, 0x80};

typedef struct {
    rtc_peer_connection_t *peer;
    rtc_data_channel_t *remote_channel;
    _Atomic unsigned messages;
    _Atomic unsigned frames;
    _Atomic unsigned opus_frames, vp8_frames;
    _Atomic unsigned channels;
    uint8_t message[4096];
    size_t message_len;
    rtc_ice_candidate_desc_t candidates[16];
    _Atomic unsigned candidate_count;
} endpoint_t;

static void received_message(const uint8_t *data, size_t len, void *user) {
    endpoint_t *endpoint = user;
    if (len <= sizeof(endpoint->message)) {
        memcpy(endpoint->message, data, len);
        endpoint->message_len = len;
        atomic_fetch_add(&endpoint->messages, 1);
    }
}

static void received_channel(rtc_data_channel_t *channel, void *user) {
    endpoint_t *endpoint = user;
    endpoint->remote_channel = channel;
    rtc_data_channel_on_message(channel, received_message, endpoint);
    atomic_fetch_add(&endpoint->channels, 1);
}

static void received_frame(const uint8_t *data, size_t len, uint16_t seq, uint32_t timestamp,
                           uint32_t ssrc, bool marker, void *user) {
    (void)seq;
    (void)timestamp;
    (void)ssrc;
    (void)marker;
    endpoint_t *endpoint = user;
    if (len == sizeof(opus_silence) && !memcmp(data, opus_silence, len))
        atomic_fetch_add(&endpoint->opus_frames, 1);
    if (len == sizeof(vp8_black) && !memcmp(data, vp8_black, len))
        atomic_fetch_add(&endpoint->vp8_frames, 1);
    atomic_fetch_add(&endpoint->frames, 1);
}

static void received_track(rtc_rtp_receiver_t *receiver, void *user) {
    rtc_rtp_receiver_on_frame(receiver, received_frame, user);
}

static void gathered_candidate(const rtc_ice_candidate_desc_t *candidate, void *user) {
    endpoint_t *endpoint = user;
    unsigned count = atomic_load(&endpoint->candidate_count);
    if (candidate && count < 16) {
        endpoint->candidates[count] = *candidate;
        atomic_store(&endpoint->candidate_count, count + 1);
    }
}

static bool exchange(rtc_peer_connection_t *offerer, rtc_peer_connection_t *answerer) {
    rtc_desc_t offer, answer;
    return rtc_peer_connection_create_offer(offerer, &offer) == RTC_OK &&
           rtc_peer_connection_set_local_desc(offerer, &offer) == RTC_OK &&
           rtc_peer_connection_set_remote_desc(answerer, &offer) == RTC_OK &&
           rtc_peer_connection_create_answer(answerer, &answer) == RTC_OK &&
           rtc_peer_connection_set_local_desc(answerer, &answer) == RTC_OK &&
           rtc_peer_connection_set_remote_desc(offerer, &answer) == RTC_OK;
}

static bool loopback_description(rtc_desc_t *description) {
    rtc_sdp_t loopback = {0};
    bool ok = rtc_sdp_parse(&loopback, description->sdp, description->sdp_len) == RTC_OK;
    loopback.type = description->type;
    for (size_t i = 0; ok && i < rtc_sdp_candidate_count(&loopback); i++) {
        rtc_ice_candidate_t *candidate = rtc_vec_at(&loopback.candidates, i);
        char ip[64];
        uint16_t port;
        ok = rtc_addr_to_string(&candidate->addr, ip, sizeof(ip), &port) == RTC_OK &&
             rtc_addr_from_string(&candidate->addr,
                                  getenv("MRTC_TEST_TURN_RELAY") ? getenv("MRTC_TEST_TURN_RELAY")
                                                                 : "127.0.0.1",
                                  port) == RTC_OK;
    }
    if (ok)
        ok = rtc_sdp_generate(&loopback) == RTC_OK;
    if (ok) {
        memcpy(description->sdp, loopback.raw, loopback.raw_len + 1);
        description->sdp_len = loopback.raw_len;
    }
    rtc_sdp_close(&loopback);
    return ok;
}

static bool exchange_relay(rtc_peer_connection_t *offerer, rtc_peer_connection_t *answerer) {
    rtc_desc_t offer, answer;
    if (rtc_peer_connection_create_offer(offerer, &offer) != RTC_OK ||
        (!offerer->relay_only && !loopback_description(&offer)) ||
        rtc_peer_connection_set_local_desc(offerer, &offer) != RTC_OK ||
        rtc_peer_connection_set_remote_desc(answerer, &offer) != RTC_OK ||
        rtc_peer_connection_create_answer(answerer, &answer) != RTC_OK ||
        (!answerer->relay_only && !loopback_description(&answer)))
        return false;
    return rtc_peer_connection_set_local_desc(answerer, &answer) == RTC_OK &&
           rtc_peer_connection_set_remote_desc(offerer, &answer) == RTC_OK;
}

static bool wait_connected(endpoint_t *a, endpoint_t *b, rtc_data_channel_t *channel) {
    uint64_t deadline = rtc_time_ms() + 10000;
    while (rtc_time_ms() < deadline) {
        if (rtc_peer_connection_connection_state(a->peer) == RTC_CONNECTION_CONNECTED &&
            rtc_peer_connection_connection_state(b->peer) == RTC_CONNECTION_CONNECTED &&
            (!channel || rtc_data_channel_state(channel) == RTC_DC_OPEN))
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_count(_Atomic unsigned *count, unsigned target, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        if (atomic_load(count) >= target)
            return true;
        SLEEP_MS(5);
    }
    return false;
}

TEST(peer_answer_adopts_offered_payload_type_before_media) {
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_peer_connection_on_track(a.peer, received_track, &a);
    rtc_peer_connection_on_track(b.peer, received_track, &b);
    rtc_codec_t offered = {
        .payload_type = 109, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_codec_t preferred = offered;
    preferred.payload_type = 111;
    rtc_rtp_sender_t *as = rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &offered);
    rtc_rtp_sender_t *bs = rtc_peer_connection_add_track(b.peer, RTC_KIND_AUDIO, &preferred);
    ASSERT(as && bs);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT(strstr(rtc_peer_connection_local_desc(b.peer)->sdp, "a=rtpmap:109 opus/48000/2") !=
           NULL);
    ASSERT(wait_connected(&a, &b, NULL));
    const uint8_t audio[] = {0xf8, 0xff, 0xfe};
    ASSERT_EQ(rtc_rtp_sender_send(as, audio, sizeof(audio), 960, true), RTC_OK);
    ASSERT_EQ(rtc_rtp_sender_send(bs, audio, sizeof(audio), 960, true), RTC_OK);
    ASSERT(wait_count(&a.frames, 1, 3000));
    ASSERT(wait_count(&b.frames, 1, 3000));
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

TEST(peer_sctp_live_renegotiation_glare_and_ice_restart) {
    /* Different STUN contexts allocate independent shared UDP listeners.
     * No STUN server is required: host candidates carry this local exchange. */
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_peer_connection_on_data_channel(b.peer, received_channel, &b);
    rtc_peer_connection_on_track(b.peer, received_track, &b);
    rtc_data_channel_t *channel =
        rtc_peer_connection_create_data_channel(a.peer, "browser-wire", NULL);
    ASSERT(channel != NULL);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT(strstr(rtc_peer_connection_local_desc(a.peer)->sdp, "UDP/DTLS/SCTP "
                                                               "webrtc-datachannel") != NULL);
    ASSERT(wait_connected(&a, &b, channel));
    const uint8_t binary[] = {0, 1, 0xff, 0, 0x7f};
    ASSERT_EQ(rtc_data_channel_send(channel, binary, sizeof(binary)), RTC_OK);
    ASSERT(wait_count(&b.messages, 1, 3000));
    ASSERT_EQ(b.message_len, sizeof(binary));
    ASSERT_MEM_EQ(b.message, binary, sizeof(binary));

    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_rtp_transceiver_init_t init = {.direction = RTC_DIR_SENDONLY};
    rtc_rtp_transceiver_t *transceiver =
        rtc_peer_connection_add_transceiver(a.peer, RTC_KIND_AUDIO, &opus, &init);
    ASSERT(transceiver != NULL);
    ASSERT(exchange(a.peer, b.peer));
    rtc_rtp_sender_t *sender = rtc_rtp_transceiver_sender(transceiver);
    const uint8_t audio[] = {0xf8, 0xff, 0xfe};
    ASSERT_EQ(rtc_rtp_sender_send(sender, audio, sizeof(audio), 960, true), RTC_OK);
    ASSERT(wait_count(&b.frames, 1, 3000));

    ASSERT_EQ(rtc_peer_connection_remove_track(a.peer, sender), RTC_OK);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT_EQ(rtc_rtp_transceiver_direction(transceiver), RTC_DIR_INACTIVE);
    unsigned before = atomic_load(&b.frames);
    (void)rtc_rtp_sender_send(sender, audio, sizeof(audio), 960, true);
    SLEEP_MS(100);
    ASSERT_EQ(atomic_load(&b.frames), before);
    rtc_rtp_transceiver_set_direction(transceiver, RTC_DIR_SENDONLY);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT_EQ(rtc_rtp_sender_send(sender, audio, sizeof(audio), 960, true), RTC_OK);
    ASSERT(wait_count(&b.frames, before + 1, 3000));

    rtc_desc_t ao, bo, answer;
    ASSERT_EQ(rtc_peer_connection_create_offer(a.peer, &ao), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(a.peer, &ao), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_create_offer(b.peer, &bo), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(b.peer, &bo), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &bo), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(b.peer, &ao), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_peer_connection_rollback(b.peer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(b.peer, &ao), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_create_answer(b.peer, &answer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(b.peer, &answer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &answer), RTC_OK);

    char fingerprint[96];
    snprintf(fingerprint, sizeof(fingerprint), "%s", rtc_peer_connection_local_fingerprint(a.peer));
    rtc_desc_t previous = *rtc_peer_connection_local_desc(a.peer);
    ASSERT_EQ(rtc_peer_connection_restart_ice(a.peer), RTC_OK);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT(strcmp(previous.sdp, rtc_peer_connection_local_desc(a.peer)->sdp) != 0);
    ASSERT(wait_connected(&a, &b, channel));
    ASSERT_STR_EQ(fingerprint, rtc_peer_connection_local_fingerprint(a.peer));
    ASSERT_EQ(rtc_data_channel_send_text(channel, "same association after restart"), RTC_OK);
    ASSERT(wait_count(&b.messages, 2, 3000));
    ASSERT_EQ(rtc_rtp_sender_send(sender, audio, sizeof(audio), 960, true), RTC_OK);
    ASSERT(wait_count(&b.frames, before + 2, 3000));

    rtc_desc_t invalid = {.type = RTC_SDP_OFFER, .sdp = "not SDP", .sdp_len = 7};
    rtc_desc_t stable = *rtc_peer_connection_remote_desc(a.peer);
    ASSERT(rtc_peer_connection_set_remote_desc(a.peer, &invalid) != RTC_OK);
    ASSERT_STR_EQ(stable.sdp, rtc_peer_connection_remote_desc(a.peer)->sdp);
    ASSERT_EQ(rtc_peer_connection_connection_state(a.peer), RTC_CONNECTION_CONNECTED);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

TEST(peer_adds_sctp_after_media_connection) {
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_peer_connection_on_data_channel(a.peer, received_channel, &a);
    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    ASSERT(rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &opus) != NULL);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT(wait_connected(&a, &b, NULL));
    rtc_data_channel_t *channel = rtc_peer_connection_create_data_channel(b.peer, "late", NULL);
    ASSERT(channel != NULL);
    ASSERT(exchange(b.peer, a.peer));
    ASSERT(wait_connected(&a, &b, channel));
    ASSERT_EQ(rtc_data_channel_send_text(channel, "late DCEP"), RTC_OK);
    ASSERT(wait_count(&a.messages, 1, 3000));
    ASSERT_MEM_EQ(a.message, "late DCEP", 9);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

TEST(peer_turn_relay_sctp_refresh_and_restart) {
    const char *port = getenv("MRTC_TEST_TURN_PORT");
    ASSERT(port && port[0]);
    char url[128];
    const char *transport = getenv("MRTC_TEST_TURN_TRANSPORT");
    bool tls = transport && !strcmp(transport, "tls");
    const char *host = getenv("MRTC_TEST_TURN_HOST");
    const char *relay_host = getenv("MRTC_TEST_TURN_RELAY");
    bool ipv6 = relay_host && !strcmp(relay_host, "::1");
    snprintf(url, sizeof(url), "%s:%s:%s?transport=%s", tls ? "turns" : "turn",
             host && !strcmp(host, "::1") ? "[::1]" : "localhost", port,
             tls || (transport && !strcmp(transport, "tcp")) ? "tcp" : "udp");
    rtc_config_t ac = {.ice_server_count = 1};
    ac.turn_tls_ca_file = getenv("MRTC_TEST_TURN_CA");
    ac.ice_servers[0] = (rtc_ice_server_t){.urls = {url},
                                           .url_count = 1,
                                           .username = "mrtc",
                                           .credential = "runtime",
                                           .relay_family = ipv6 ? AF_INET6 : AF_INET};
    char wrong_name_url[128];
    if (getenv("MRTC_TEST_TURN_IDENTITIES")) {
        snprintf(wrong_name_url, sizeof(wrong_name_url), "%s:127.0.0.1:%s?transport=tcp",
                 tls ? "turns" : "turn", port);
        ac.ice_server_count = 4;
        ac.turn_tls_ca_file = NULL;
        for (int i = 0; i < 4; i++) {
            ac.ice_servers[i] = ac.ice_servers[0];
            ac.ice_servers[i].tls_ca_file = getenv("MRTC_TEST_TURN_CA");
        }
        ac.ice_servers[0].urls[0] = wrong_name_url;
        ac.ice_servers[0].username = "wrong-user";
        ac.ice_servers[0].credential = "wrong-password";
        ac.ice_servers[1].credential = "wrong-password";
        ac.ice_servers[2].tls_ca_file = NULL;
        if (!tls)
            ac.ice_servers[2].username = "wrong-user";
    }
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 9};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_peer_connection_on_track(b.peer, received_track, &b);
    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_codec_t vp8 = {.payload_type = 96, .mime_type = "video/VP8", .clock_rate = 90000};
    rtc_rtp_sender_t *audio_sender = rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &opus);
    rtc_rtp_sender_t *video_sender = rtc_peer_connection_add_track(a.peer, RTC_KIND_VIDEO, &vp8);
    ASSERT(audio_sender && video_sender);
    rtc_peer_connection_on_data_channel(b.peer, received_channel, &b);
    rtc_peer_connection_on_ice_candidate(a.peer, gathered_candidate, &a);
    rtc_data_channel_t *channel = rtc_peer_connection_create_data_channel(a.peer, "relay", NULL);
    ASSERT(channel != NULL);
    ASSERT(exchange_relay(a.peer, b.peer));
    uint64_t deadline = rtc_time_ms() + 10000;
    while (rtc_time_ms() < deadline &&
           rtc_peer_connection_ice_gathering_state(a.peer) != RTC_ICE_GATHERING_COMPLETE)
        SLEEP_MS(10);
    ASSERT_EQ(rtc_peer_connection_ice_gathering_state(a.peer), RTC_ICE_GATHERING_COMPLETE);
    ASSERT(atomic_load(&a.candidate_count) > 0);
    for (unsigned i = 0; i < atomic_load(&a.candidate_count); i++) {
        ASSERT(strstr(a.candidates[i].candidate, " typ relay") != NULL);
        ASSERT_EQ(rtc_peer_connection_add_ice_candidate(b.peer, &a.candidates[i]), RTC_OK);
    }
    ASSERT(wait_connected(&a, &b, channel));
    ASSERT_EQ(rtc_rtp_sender_send(audio_sender, opus_silence, sizeof(opus_silence), 960, true),
              RTC_OK);
    ASSERT_EQ(rtc_rtp_sender_send(video_sender, vp8_black, sizeof(vp8_black), 3000, true), RTC_OK);
    ASSERT(wait_count(&b.frames, 2, 3000));
    ASSERT_EQ(atomic_load(&b.opus_frames), 1);
    ASSERT_EQ(atomic_load(&b.vp8_frames), 1);
    ASSERT_EQ(rtc_data_channel_send_text(channel, "authenticated relay"), RTC_OK);
    ASSERT(wait_count(&b.messages, 1, 3000));
    SLEEP_MS(3300);
    ASSERT_EQ(rtc_data_channel_send_text(channel, "after allocation refresh"), RTC_OK);
    ASSERT(wait_count(&b.messages, 2, 3000));
    rtc_transport_candidate_t published, recovered;
    ASSERT_EQ(rtc_transport_get_relay_candidate(a.peer->runtime_transport, &published), RTC_OK);
    const char *restart = getenv("MRTC_TEST_TURN_RESTART");
    if (restart) {
        FILE *request = fopen(restart, "w");
        ASSERT(request != NULL);
        ASSERT_EQ(fclose(request), 0);
        uint64_t end = rtc_time_ms() + 10000;
        bool failed = false, ready = false;
        do {
            FILE *ack = fopen(getenv("MRTC_TEST_TURN_READY"), "r");
            if (ack) {
                fclose(ack);
                ready = true;
            }
            rtc_transport_stats_t stats;
            ASSERT_EQ(rtc_transport_get_stats(a.peer->runtime_transport, &stats), RTC_OK);
            failed = stats.turn_failed;
            if (failed && ready)
                break;
            SLEEP_MS(10);
        } while (rtc_time_ms() < end);
        ASSERT(failed && ready);
    }
    rtc_ice_parameters_t before, after;
    ASSERT_EQ(rtc_transport_get_ice_parameters(a.peer->runtime_transport, &before), RTC_OK);
    atomic_store(&a.candidate_count, 0);
    bool remote_restart = getenv("MRTC_TEST_TURN_REMOTE_RESTART") != NULL;
    ASSERT_EQ(rtc_peer_connection_restart_ice(remote_restart ? b.peer : a.peer), RTC_OK);
    ASSERT(remote_restart ? exchange_relay(b.peer, a.peer) : exchange_relay(a.peer, b.peer));
    ASSERT_EQ(rtc_transport_get_ice_parameters(a.peer->runtime_transport, &after), RTC_OK);
    ASSERT(strcmp(before.username_fragment, after.username_fragment) != 0);
    deadline = rtc_time_ms() + 10000;
    while (rtc_time_ms() < deadline &&
           rtc_peer_connection_ice_gathering_state(a.peer) != RTC_ICE_GATHERING_COMPLETE)
        SLEEP_MS(10);
    ASSERT_EQ(rtc_peer_connection_ice_gathering_state(a.peer), RTC_ICE_GATHERING_COMPLETE);
    ASSERT(atomic_load(&a.candidate_count) > 0);
    for (unsigned i = 0; i < atomic_load(&a.candidate_count); i++) {
        ASSERT(strstr(a.candidates[i].candidate, after.username_fragment) != NULL);
        ASSERT_EQ(rtc_peer_connection_add_ice_candidate(b.peer, &a.candidates[i]), RTC_OK);
    }
    ASSERT(wait_connected(&a, &b, channel));
    ASSERT_EQ(rtc_transport_get_relay_candidate(a.peer->runtime_transport, &recovered), RTC_OK);
    if (!restart) {
        ASSERT_EQ(strcmp(published.address, recovered.address), 0);
        ASSERT_EQ(published.port, recovered.port);
    }
    ASSERT_EQ(rtc_data_channel_send_text(channel, "same SCTP association"), RTC_OK);
    ASSERT(wait_count(&b.messages, 3, 3000));
    ASSERT_EQ(rtc_rtp_sender_send(audio_sender, opus_silence, sizeof(opus_silence), 1920, true),
              RTC_OK);
    ASSERT_EQ(rtc_rtp_sender_send(video_sender, vp8_black, sizeof(vp8_black), 6000, true), RTC_OK);
    ASSERT(wait_count(&b.frames, 4, 3000));
    ASSERT_EQ(atomic_load(&b.opus_frames), 2);
    ASSERT_EQ(atomic_load(&b.vp8_frames), 2);
    ASSERT_EQ(atomic_load(&b.channels), 1);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

int main(int argc, char **argv) {
    rtc_set_log_level(RTC_LOG_DEBUG);
    if (rtc_client_init() != RTC_OK)
        return 1;
    if (argc > 1 && strcmp(argv[1], "--turn") == 0) {
        RUN_TEST(peer_turn_relay_sctp_refresh_and_restart);
    } else {
        RUN_TEST(peer_answer_adopts_offered_payload_type_before_media);
        RUN_TEST(peer_sctp_live_renegotiation_glare_and_ice_restart);
        RUN_TEST(peer_adds_sctp_after_media_connection);
    }
    rtc_client_cleanup();
    TEST_SUMMARY();
}
