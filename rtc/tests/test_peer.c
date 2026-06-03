/*
 * test_peer.c - Peer connection lifecycle tests (new WebRTC-style API).
 *
 * Tests:
 *   1. Create / close / destroy lifecycle
 *   2. Add track creates sender with correct codec
 *   3. Signaling state transitions on set_local_desc
 *   4. Create offer generates valid SDP
 *   5. ICE candidates fired via on_ice_candidate callback
 *   6. Full offer/answer exchange between two peers
 */
#include <rtc/rtc.h>
#include "test_harness.h"
#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ------------------------------------------------------------------ */
/*  Test: create / close / destroy                                     */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Test: add_track creates sender with correct codec                  */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Test: create_offer generates SDP                                   */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Test: set_local_desc transitions signaling state                   */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Test: on_ice_candidate fires for gathered candidates               */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Test: full offer/answer between two peers, auto-connect            */
/* ------------------------------------------------------------------ */
static _Atomic rtc_connection_state_t g_alice_state;
static _Atomic rtc_connection_state_t g_bob_state;

static void alice_on_conn(rtc_connection_state_t state, void *user) {
    (void)user;
    g_alice_state = state;
}

static void bob_on_conn(rtc_connection_state_t state, void *user) {
    (void)user;
    g_bob_state = state;
}

TEST(peer_two_connect) {
    g_alice_state = RTC_CONNECTION_NEW;
    g_bob_state = RTC_CONNECTION_NEW;

    rtc_config_t config;
    memset(&config, 0, sizeof(config));

    rtc_peer_connection_t *alice = rtc_peer_connection_create(&config);
    rtc_peer_connection_t *bob = rtc_peer_connection_create(&config);
    ASSERT(alice != NULL);
    ASSERT(bob != NULL);

    rtc_peer_connection_on_connection_state(alice, alice_on_conn, NULL);
    rtc_peer_connection_on_connection_state(bob, bob_on_conn, NULL);

    rtc_codec_t opus;
    memset(&opus, 0, sizeof(opus));
    opus.payload_type = 111;
    strcpy(opus.mime_type, "audio/opus");
    opus.clock_rate = 48000;
    opus.channels = 2;

    rtc_peer_connection_add_track(alice, RTC_KIND_AUDIO, &opus);
    rtc_peer_connection_add_track(bob, RTC_KIND_AUDIO, &opus);

    /* Alice creates offer */
    rtc_desc_t offer;
    int rc = rtc_peer_connection_create_offer(alice, &offer);
    ASSERT_EQ(rc, RTC_OK);

    rtc_peer_connection_set_local_desc(alice, &offer);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(alice), (int)RTC_SIGNALING_HAVE_LOCAL_OFFER);

    /* Bob receives offer, creates answer */
    rtc_peer_connection_set_remote_desc(bob, &offer);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(bob), (int)RTC_SIGNALING_HAVE_REMOTE_OFFER);

    rtc_desc_t answer;
    rc = rtc_peer_connection_create_answer(bob, &answer);
    ASSERT_EQ(rc, RTC_OK);

    rtc_peer_connection_set_local_desc(bob, &answer);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(bob), (int)RTC_SIGNALING_STABLE);

    /* Alice receives answer → connection starts automatically */
    rtc_peer_connection_set_remote_desc(alice, &answer);
    ASSERT_EQ((int)rtc_peer_connection_signaling_state(alice), (int)RTC_SIGNALING_STABLE);

    /* Wait for both to connect (up to 10 seconds) */
    for (int i = 0; i < 200; i++) {
        if (g_alice_state == RTC_CONNECTION_CONNECTED && g_bob_state == RTC_CONNECTION_CONNECTED)
            break;
        if (g_alice_state == RTC_CONNECTION_FAILED || g_bob_state == RTC_CONNECTION_FAILED)
            break;
        SLEEP_MS(50);
    }

    printf("    alice: %d  bob: %d\n", (int)g_alice_state, (int)g_bob_state);
    ASSERT_EQ((int)g_alice_state, (int)RTC_CONNECTION_CONNECTED);
    ASSERT_EQ((int)g_bob_state, (int)RTC_CONNECTION_CONNECTED);

    printf("    two peers connected via auto-connect\n");

    rtc_peer_connection_close(alice);
    rtc_peer_connection_close(bob);
    rtc_peer_connection_destroy(alice);
    rtc_peer_connection_destroy(bob);
}

/* ------------------------------------------------------------------ */
/*  Test: get_stats produces a valid snapshot                          */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Test: add_transceiver honors direction; remove_track flips it      */
/* ------------------------------------------------------------------ */
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
    ASSERT_EQ(rtc_peer_connection_remove_track(pc, (rtc_rtp_sender_t *)&sentinel),
              RTC_ERR_INVALID);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
}

/* ------------------------------------------------------------------ */
/*  Test: sender get/set parameters                                    */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  Peer Connection Tests (New API)\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(peer_create_destroy);
    RUN_TEST(peer_add_track);
    RUN_TEST(peer_create_offer);
    RUN_TEST(peer_signaling_states);
    RUN_TEST(peer_ice_candidates);
    RUN_TEST(peer_two_connect);
    RUN_TEST(peer_get_stats);
    RUN_TEST(peer_add_transceiver_remove_track);
    RUN_TEST(peer_sender_parameters);

    rtc_cleanup();
    TEST_SUMMARY();
}
