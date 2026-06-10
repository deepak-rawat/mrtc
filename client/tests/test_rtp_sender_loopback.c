/*
 * Client RTP sender over a public runtime transport.
 */
#include <rtc/rtc_client.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_sdp.h>
#include <rtc/rtc_transport.h>
#include <rtc/rtc_worker.h>

#include "test_harness.h"

#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((unsigned)((ms) * 1000))
#endif

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_transport_t *transport;
} remote_env_t;

static _Atomic rtc_connection_state_t g_peer_state;
static rtc_mutex_t g_rtp_lock;
static rtc_cond_t g_rtp_cond;
static int g_rtp_count;
static uint8_t g_rtp_payload[128];
static size_t g_rtp_payload_len;
static uint8_t g_rtp_payload_type;

static void on_peer_state(rtc_connection_state_t state, void *user) {
    (void)user;
    g_peer_state = state;
}

static void on_plain_rtp(const rtc_rtp_packet_t *pkt, void *user) {
    (void)user;
    rtc_mutex_lock(&g_rtp_lock);
    g_rtp_count++;
    g_rtp_payload_type = pkt->header.payload_type;
    g_rtp_payload_len = pkt->payload_len;
    if (g_rtp_payload_len > sizeof(g_rtp_payload))
        g_rtp_payload_len = sizeof(g_rtp_payload);
    memcpy(g_rtp_payload, pkt->payload, g_rtp_payload_len);
    rtc_cond_signal(&g_rtp_cond);
    rtc_mutex_unlock(&g_rtp_lock);
}

static bool make_remote_env(remote_env_t *env) {
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

static void close_remote_env(remote_env_t *env) {
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

static bool build_runtime_answer(remote_env_t *remote, const rtc_desc_t *offer,
                                 rtc_desc_t *answer) {
    rtc_sdp_t parsed_offer;
    memset(&parsed_offer, 0, sizeof(parsed_offer));
    if (rtc_sdp_parse(&parsed_offer, offer->sdp, offer->sdp_len) != RTC_OK)
        return false;

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

static bool wait_for_peer_connected(remote_env_t *remote, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (g_peer_state == RTC_CONNECTION_CONNECTED &&
            rtc_transport_get_stats(remote->transport, &stats) == RTC_OK &&
            stats.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED && stats.srtp_ready)
            return true;
        if (g_peer_state == RTC_CONNECTION_FAILED)
            return false;
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_rtp(int target, int timeout_ms) {
    rtc_mutex_lock(&g_rtp_lock);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (g_rtp_count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&g_rtp_lock);
            return false;
        }
        rtc_cond_wait_timeout(&g_rtp_cond, &g_rtp_lock, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&g_rtp_lock);
    return true;
}

TEST(sender_send_over_logical_transport) {
    ASSERT_EQ(rtc_mutex_init(&g_rtp_lock), RTC_OK);
    ASSERT_EQ(rtc_cond_init(&g_rtp_cond), RTC_OK);
    g_peer_state = RTC_CONNECTION_NEW;
    g_rtp_count = 0;
    g_rtp_payload_len = 0;
    g_rtp_payload_type = 0;
    memset(g_rtp_payload, 0, sizeof(g_rtp_payload));

    rtc_config_t config;
    memset(&config, 0, sizeof(config));
    rtc_peer_connection_t *peer = rtc_peer_connection_create(&config);
    ASSERT(peer != NULL);

    remote_env_t remote;
    ASSERT(make_remote_env(&remote));
    rtc_transport_on_rtp(remote.transport, on_plain_rtp, NULL);
    rtc_peer_connection_on_connection_state(peer, on_peer_state, NULL);

    rtc_codec_t vp8;
    memset(&vp8, 0, sizeof(vp8));
    vp8.payload_type = 96;
    memcpy(vp8.mime_type, "video/VP8", sizeof("video/VP8"));
    vp8.clock_rate = 90000;
    rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(peer, RTC_KIND_VIDEO, &vp8);
    ASSERT(sender != NULL);

    rtc_desc_t offer;
    ASSERT_EQ(rtc_peer_connection_create_offer(peer, &offer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(peer, &offer), RTC_OK);

    rtc_desc_t answer;
    ASSERT(build_runtime_answer(&remote, &offer, &answer));
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(peer, &answer), RTC_OK);
    ASSERT(wait_for_peer_connected(&remote, 10000));

    const uint8_t payload[] = {0xFE, 0xED, 0xFA, 0xCE};
    ASSERT_EQ(rtc_rtp_sender_send(sender, payload, sizeof(payload), 3000, true), RTC_OK);
    ASSERT(wait_for_rtp(1, 1000));

    ASSERT_EQ(g_rtp_payload_type, 96);
    ASSERT_EQ(g_rtp_payload_len, sizeof(payload));
    ASSERT_MEM_EQ(g_rtp_payload, payload, sizeof(payload));

    rtc_peer_connection_close(peer);
    rtc_peer_connection_destroy(peer);
    close_remote_env(&remote);
    rtc_cond_destroy(&g_rtp_cond);
    rtc_mutex_destroy(&g_rtp_lock);
}

int main(void) {
    rtc_client_init();
    RUN_TEST(sender_send_over_logical_transport);
    rtc_client_cleanup();
    TEST_SUMMARY();
}
