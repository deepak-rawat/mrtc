#include <rtc/rtc_client.h>
#include <rtc/rtc_sdp.h>
#include "test_harness.h"
#include "rtc_peer_internal.h"
#include <rtc/rtc_transport.h>
#include "rtc_transport_internal.h"
#include <rtc/rtc_rtp_ext.h>
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
    _Atomic uint16_t last_seq;
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
    (void)timestamp;
    (void)ssrc;
    (void)marker;
    endpoint_t *endpoint = user;
    atomic_store(&endpoint->last_seq, seq);
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

typedef struct {
    rtc_peer_connection_t *peer;
    _Atomic unsigned byes;
    _Atomic uint32_t bye_ssrc;
    _Atomic unsigned packets;
} collision_reports_t;

static void collision_reports(const uint8_t *data, size_t len, void *user) {
    collision_reports_t *reports = user;
    for (size_t off = 0; off + 4 <= len;) {
        size_t size = ((size_t)data[off + 2] * 256 + data[off + 3] + 1) * 4;
        if (size > len - off)
            break;
        rtc_rtcp_bye_t bye;
        if (data[off + 1] == RTCP_PT_BYE && rtc_rtcp_parse_bye(&bye, data + off, size) == RTC_OK &&
            bye.count) {
            atomic_store(&reports->bye_ssrc, bye.ssrcs[0]);
            atomic_fetch_add(&reports->byes, 1);
        }
        off += size;
    }
    rtc_media_session_handle_rtcp(&reports->peer->media_session, data, len);
    atomic_fetch_add(&reports->packets, 1);
}

TEST(peer_authenticated_collision_preserves_bundle_and_sctp) {
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_peer_connection_on_track(a.peer, received_track, &a);
    rtc_peer_connection_on_track(b.peer, received_track, &b);
    rtc_peer_connection_on_data_channel(b.peer, received_channel, &b);
    rtc_codec_t video = {.payload_type = 96, .mime_type = "video/VP8", .clock_rate = 90000};
    rtc_codec_t audio = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_rtp_sender_t *first = rtc_peer_connection_add_track(a.peer, RTC_KIND_VIDEO, &video);
    rtc_rtp_sender_t *second = rtc_peer_connection_add_track(a.peer, RTC_KIND_VIDEO, &video);
    rtc_rtp_sender_t *sound = rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &audio);
    ASSERT(rtc_peer_connection_add_track(b.peer, RTC_KIND_VIDEO, &video));
    ASSERT(rtc_peer_connection_add_track(b.peer, RTC_KIND_VIDEO, &video));
    ASSERT(rtc_peer_connection_add_track(b.peer, RTC_KIND_AUDIO, &audio));
    rtc_data_channel_t *channel =
        rtc_peer_connection_create_data_channel(a.peer, "collision", NULL);
    ASSERT(first && second && sound && channel);
    ASSERT(exchange(a.peer, b.peer));
    ASSERT(wait_connected(&a, &b, channel));
    collision_reports_t ar = {.peer = a.peer}, br = {.peer = b.peer};
    rtc_transport_on_rtcp(a.peer->runtime_transport, collision_reports, &ar);
    rtc_transport_on_rtcp(b.peer->runtime_transport, collision_reports, &br);
    ASSERT_EQ(rtc_rtp_sender_send(first, vp8_black, sizeof(vp8_black), 3000, true), RTC_OK);
    ASSERT(wait_count(&b.vp8_frames, 1, 3000));
    uint16_t old_seq = atomic_load(&b.last_seq);
    ASSERT_EQ(rtc_rtp_sender_send(second, vp8_black, sizeof(vp8_black), 3000, true), RTC_OK);
    ASSERT(wait_count(&b.vp8_frames, 2, 3000));
    rtc_stats_report_t before, after;
    ASSERT_EQ(rtc_peer_connection_get_stats(a.peer, &before), RTC_OK);
    uint32_t old = before.transceivers[0].out_ssrc;

    /* A report-block reference and a retiring RR+SDES+BYE sender are not collisions. */
    rtc_rtcp_stats_t stats;
    rtc_rtcp_packet_t rr;
    rtc_rtcp_stats_init(&stats, 0xabcdef01);
    rtc_rtcp_stats_on_rtp_recv(&stats, 1, 1, old, 90000);
    ASSERT_EQ(rtc_rtcp_build_report(&rr, &stats, false), RTC_OK);
    uint8_t report[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
    memcpy(report, rr.buf, rr.buf_len);
    size_t length = rr.buf_len;
    const uint8_t unknown[] = {
        0x80,        211, 0, 1, (uint8_t)(old >> 24), (uint8_t)(old >> 16), (uint8_t)(old >> 8),
        (uint8_t)old};
    memcpy(report + length, unknown, sizeof(unknown));
    length += sizeof(unknown);
    unsigned packets = atomic_load(&ar.packets);
    ASSERT_EQ(rtc_transport_send_rtcp(b.peer->runtime_transport, report, &length, sizeof(report)),
              RTC_OK);
    ASSERT(wait_count(&ar.packets, packets + 1, 3000));
    packets = atomic_load(&ar.packets);
    ASSERT_EQ(rtc_transport_send_bye(b.peer->runtime_transport, old), RTC_OK);
    ASSERT(wait_count(&ar.packets, packets + 1, 3000));
    ASSERT_EQ(rtc_peer_connection_get_stats(a.peer, &after), RTC_OK);
    ASSERT_EQ(after.transceivers[0].out_ssrc, old);

    rtc_rtp_ext_t mid;
    rtc_rtp_ext_make_string(&mid, 4, "0");
    rtc_rtp_packet_t collision;
    ASSERT_EQ(rtc_rtp_build_with_ext(&collision, 96, 123, 9000, old, true, &mid, 1, vp8_black,
                                     sizeof(vp8_black)),
              RTC_OK);
    length = collision.buf_len;
    /* Plain RTP fails authentication and must not rename a local source. */
    ASSERT_EQ(rtc_transport_send_raw(b.peer->runtime_transport, collision.buf, length), RTC_OK);
    SLEEP_MS(30);
    ASSERT_EQ(rtc_peer_connection_get_stats(a.peer, &after), RTC_OK);
    ASSERT_EQ(after.transceivers[0].out_ssrc, old);
    ASSERT_EQ(rtc_transport_send_rtp(b.peer->runtime_transport, collision.buf, &length,
                                     sizeof(collision.buf)),
              RTC_OK);
    ASSERT(wait_count(&br.byes, 1, 3000));
    ASSERT_EQ(atomic_load(&br.bye_ssrc), old);
    ASSERT(wait_count(&a.vp8_frames, 1, 3000));
    ASSERT_EQ(rtc_peer_connection_get_stats(a.peer, &after), RTC_OK);
    uint32_t current = after.transceivers[0].out_ssrc;
    ASSERT(current && current != old);
    ASSERT_EQ(after.transceivers[0].out_source_generation, 2);
    ASSERT_EQ(after.transceivers[0].out_packets_sent, 0);
    ASSERT_EQ(after.transceivers[1].out_ssrc, before.transceivers[1].out_ssrc);
    ASSERT_EQ(after.transceivers[2].out_ssrc, before.transceivers[2].out_ssrc);
    ASSERT_EQ(rtc_transport_send_raw(b.peer->runtime_transport, collision.buf, length), RTC_OK);
    SLEEP_MS(30);
    ASSERT_EQ(atomic_load(&a.vp8_frames), 1); /* old receive replay context survived */

    /* The new SSRC is unsignalled and two BUNDLE sections share PT=96: MID is essential. */
    ASSERT_EQ(rtc_rtp_sender_send(first, vp8_black, sizeof(vp8_black), 3000, true), RTC_OK);
    ASSERT(wait_count(&b.vp8_frames, 3, 3000));
    uint16_t new_seq = atomic_load(&b.last_seq);
    ASSERT_EQ(rtc_rtp_sender_send(second, vp8_black, sizeof(vp8_black), 3000, true), RTC_OK);
    ASSERT(wait_count(&b.vp8_frames, 4, 3000));
    ASSERT_EQ(rtc_rtp_sender_send(sound, opus_silence, sizeof(opus_silence), 960, true), RTC_OK);
    ASSERT(wait_count(&b.opus_frames, 1, 3000));
    ASSERT_EQ(rtc_data_channel_send_text(channel, "same SCTP association"), RTC_OK);
    ASSERT(wait_count(&b.messages, 1, 3000));
    ASSERT_EQ(atomic_load(&b.channels), 1);
    ASSERT_EQ(rtc_data_channel_state(channel), RTC_DC_OPEN);

    rtc_transport_stats_t tx_before, tx_after;
    ASSERT_EQ(rtc_transport_get_stats(a.peer->runtime_transport, &tx_before), RTC_OK);
    length = 0;
    ASSERT_EQ(rtc_rtcp_build_nack(report, sizeof(report), &length, 0xabcdef01, old, &old_seq, 1),
              RTC_OK);
    packets = atomic_load(&ar.packets);
    ASSERT_EQ(rtc_transport_send_rtcp(b.peer->runtime_transport, report, &length, sizeof(report)),
              RTC_OK);
    ASSERT(wait_count(&ar.packets, packets + 1, 3000));
    ASSERT_EQ(rtc_transport_get_stats(a.peer->runtime_transport, &tx_after), RTC_OK);
    ASSERT_EQ(tx_after.rtp_retransmissions_sent, tx_before.rtp_retransmissions_sent);
    length = 0;
    ASSERT_EQ(rtc_rtcp_build_nack(report, sizeof(report), &length, 0xabcdef01, current, &new_seq,
                                  1),
              RTC_OK);
    packets = atomic_load(&ar.packets);
    ASSERT_EQ(rtc_transport_send_rtcp(b.peer->runtime_transport, report, &length, sizeof(report)),
              RTC_OK);
    ASSERT(wait_count(&ar.packets, packets + 1, 3000));
    ASSERT_EQ(rtc_transport_get_stats(a.peer->runtime_transport, &tx_after), RTC_OK);
    ASSERT_EQ(tx_after.rtp_retransmissions_sent, tx_before.rtp_retransmissions_sent + 1);
    rtc_desc_t offer;
    ASSERT_EQ(rtc_peer_connection_create_offer(a.peer, &offer), RTC_OK);
    rtc_sdp_t parsed = {0};
    ASSERT_EQ(rtc_sdp_parse(&parsed, offer.sdp, offer.sdp_len), RTC_OK);
    ASSERT_EQ(parsed.media[0].ssrc, current);
    rtc_sdp_close(&parsed);
    ASSERT_EQ(rtc_peer_connection_connection_state(a.peer), RTC_CONNECTION_CONNECTED);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

TEST(peer_authenticated_rtcp_collisions_exhaust_without_reusing_ids) {
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_codec_t audio = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    ASSERT(rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &audio));
    ASSERT(rtc_peer_connection_add_track(b.peer, RTC_KIND_AUDIO, &audio));
    ASSERT(exchange(a.peer, b.peer));
    ASSERT(wait_connected(&a, &b, NULL));
    rtc_stats_report_t stats, remote;
    ASSERT_EQ(rtc_peer_connection_get_stats(a.peer, &stats), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_get_stats(b.peer, &remote), RTC_OK);
    uint32_t used[65] = {stats.transceivers[0].out_ssrc};
    unsigned rotations = 0;
    while (rtc_peer_connection_connection_state(a.peer) == RTC_CONNECTION_CONNECTED &&
           rotations < 64) {
        rtc_rtcp_stats_t sender;
        rtc_rtcp_packet_t report;
        rtc_rtcp_stats_init(&sender, remote.transceivers[0].out_ssrc);
        ASSERT_EQ(rtc_rtcp_build_report(&report, &sender, false), RTC_OK);
        uint32_t current = used[rotations];
        /* A stable authenticated reporter describes a colliding SDES source.
         * Its crypto namespace stays constant while the local victim rotates. */
        uint8_t chunk[] = {0x81,
                           RTCP_PT_SDES,
                           0,
                           2,
                           (uint8_t)(current >> 24),
                           (uint8_t)(current >> 16),
                           (uint8_t)(current >> 8),
                           (uint8_t)current,
                           1,
                           1,
                           'c',
                           0};
        uint8_t wire[RTCP_MAX_PACKET + RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD];
        memcpy(wire, report.buf, report.buf_len);
        memcpy(wire + report.buf_len, chunk, sizeof(chunk));
        size_t length = report.buf_len + sizeof(chunk);
        ASSERT_EQ(rtc_transport_send_rtcp(b.peer->runtime_transport, wire, &length, sizeof(wire)),
                  RTC_OK);
        uint64_t deadline = rtc_time_ms() + 3000;
        do {
            int rc = rtc_peer_connection_get_stats(a.peer, &stats);
            if (rc != RTC_OK) {
                ASSERT_EQ(rc, RTC_ERR_INVALID);
                ASSERT_EQ(rtc_peer_connection_connection_state(a.peer), RTC_CONNECTION_FAILED);
                break;
            }
            if (stats.transceivers[0].out_ssrc != current ||
                rtc_peer_connection_connection_state(a.peer) == RTC_CONNECTION_FAILED)
                break;
            SLEEP_MS(1);
        } while (rtc_time_ms() < deadline);
        if (rtc_peer_connection_connection_state(a.peer) == RTC_CONNECTION_FAILED)
            break;
        ASSERT(stats.transceivers[0].out_ssrc != current);
        uint32_t replacement = stats.transceivers[0].out_ssrc;
        ASSERT(replacement != remote.transceivers[0].out_ssrc);
        for (unsigned i = 0; i <= rotations; i++)
            ASSERT(replacement != used[i]);
        used[++rotations] = replacement;
    }
    ASSERT(rotations > 1 && rotations < 64);
    ASSERT_EQ(rtc_peer_connection_connection_state(a.peer), RTC_CONNECTION_FAILED);
    ASSERT_EQ(rtc_peer_connection_connection_state(b.peer), RTC_CONNECTION_CONNECTED);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

static bool description_extension_direction(rtc_desc_t *description,
                                            rtc_sdp_ext_direction_t direction) {
    rtc_sdp_t sdp = {0};
    int rc = rtc_sdp_parse(&sdp, description->sdp, description->sdp_len);
    if (rc == RTC_OK) {
        for (int i = 0; i < sdp.media_count; i++)
            for (int j = 0; j < sdp.media[i].extmap_count; j++)
                sdp.media[i].extmaps[j].direction = direction;
        rc = rtc_sdp_generate(&sdp);
    }
    if (rc == RTC_OK) {
        memcpy(description->sdp, sdp.raw, sdp.raw_len + 1);
        description->sdp_len = sdp.raw_len;
    }
    rtc_sdp_close(&sdp);
    return rc == RTC_OK;
}

TEST(peer_directional_answers_and_invalid_negotiation_preserve_live_media) {
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_rtp_sender_t *as = rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &opus);
    rtc_rtp_sender_t *bs = rtc_peer_connection_add_track(b.peer, RTC_KIND_AUDIO, &opus);
    ASSERT(as && bs);
    rtc_peer_connection_on_track(a.peer, received_track, &a);
    rtc_peer_connection_on_track(b.peer, received_track, &b);
    rtc_peer_connection_on_data_channel(b.peer, received_channel, &b);
    rtc_data_channel_t *channel =
        rtc_peer_connection_create_data_channel(a.peer, "directions", NULL);
    ASSERT(channel && exchange(a.peer, b.peer));
    ASSERT(wait_connected(&a, &b, channel));
    const rtc_sdp_ext_direction_t inverse[] = {RTC_SDP_EXT_SENDRECV, RTC_SDP_EXT_RECVONLY,
                                               RTC_SDP_EXT_SENDONLY, RTC_SDP_EXT_INACTIVE};
    for (int direction = RTC_SDP_EXT_SENDONLY; direction <= RTC_SDP_EXT_INACTIVE; direction++) {
        rtc_desc_t previous_a = *rtc_peer_connection_remote_desc(a.peer);
        rtc_desc_t previous_b = *rtc_peer_connection_local_desc(b.peer);
        rtc_desc_t offer, answer, invalid;
        ASSERT_EQ(rtc_peer_connection_create_offer(a.peer, &offer), RTC_OK);
        ASSERT(description_extension_direction(&offer, (rtc_sdp_ext_direction_t)direction));
        ASSERT_EQ(rtc_peer_connection_set_local_desc(a.peer, &offer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_set_remote_desc(b.peer, &offer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_create_answer(b.peer, &answer), RTC_OK);
        rtc_sdp_t parsed = {0};
        ASSERT_EQ(rtc_sdp_parse(&parsed, answer.sdp, answer.sdp_len), RTC_OK);
        ASSERT_EQ(parsed.media[0].extmaps[0].direction, inverse[direction]);
        rtc_sdp_close(&parsed);
        invalid = answer;
        ASSERT(description_extension_direction(&invalid, RTC_SDP_EXT_SENDRECV));
        ASSERT_EQ(rtc_peer_connection_set_local_desc(b.peer, &invalid), RTC_ERR_SDP);
        ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &invalid), RTC_ERR_SDP);
        ASSERT_STR_EQ(rtc_peer_connection_remote_desc(a.peer)->sdp, previous_a.sdp);
        ASSERT_STR_EQ(rtc_peer_connection_local_desc(b.peer)->sdp, previous_b.sdp);
        ASSERT_EQ(rtc_peer_connection_connection_state(a.peer), RTC_CONNECTION_CONNECTED);
        ASSERT_EQ(rtc_data_channel_state(channel), RTC_DC_OPEN);
        ASSERT_EQ(rtc_peer_connection_set_local_desc(b.peer, &answer), RTC_OK);
        ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &answer), RTC_OK);
        unsigned af = atomic_load(&a.frames), bf = atomic_load(&b.frames);
        ASSERT_EQ(rtc_rtp_sender_send(as, opus_silence, sizeof(opus_silence), 960, true), RTC_OK);
        ASSERT_EQ(rtc_rtp_sender_send(bs, opus_silence, sizeof(opus_silence), 960, true), RTC_OK);
        ASSERT(wait_count(&a.frames, af + 1, 1000));
        ASSERT(wait_count(&b.frames, bf + 1, 1000));
    }
    rtc_desc_t stable = *rtc_peer_connection_remote_desc(a.peer);
    const char *invalid_lines[] = {"a=ssrc:4294967297 cname:test\r\n", "a=sctp-port:4294972296\r\n",
                                   "a=max-message-size:4294967296\r\n",
                                   "a=extmap:4/unsupported "
                                   "urn:ietf:params:rtp-hdrext:sdes:mid\r\n"};
    for (unsigned i = 0; i < sizeof(invalid_lines) / sizeof(*invalid_lines); i++) {
        rtc_desc_t invalid = stable;
        invalid.type = RTC_SDP_OFFER;
        size_t extra = strlen(invalid_lines[i]);
        ASSERT(invalid.sdp_len + extra < sizeof(invalid.sdp));
        memcpy(invalid.sdp + invalid.sdp_len, invalid_lines[i], extra + 1);
        invalid.sdp_len += extra;
        ASSERT(rtc_peer_connection_set_remote_desc(a.peer, &invalid) != RTC_OK);
        ASSERT_STR_EQ(rtc_peer_connection_remote_desc(a.peer)->sdp, stable.sdp);
        ASSERT_EQ(rtc_peer_connection_signaling_state(a.peer), RTC_SIGNALING_STABLE);
    }
    rtc_desc_t unsupported = stable;
    unsupported.type = RTC_SDP_OFFER;
    char *profile = strstr(unsupported.sdp, "UDP/TLS/RTP/SAVPF");
    ASSERT(profile);
    memcpy(profile, "TCP", 3);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &unsupported), RTC_ERR_SDP);
    rtc_sdp_t rejected = {0};
    ASSERT_EQ(rtc_sdp_parse(&rejected, stable.sdp, stable.sdp_len), RTC_OK);
    for (int i = 0; i < rejected.media_count; i++)
        if (rejected.media[i].media_type == RTC_MEDIA_APPLICATION)
            rejected.media[i].rejected = true;
    ASSERT_EQ(rtc_sdp_generate(&rejected), RTC_OK);
    unsupported = (rtc_desc_t){.type = RTC_SDP_OFFER};
    memcpy(unsupported.sdp, rejected.raw, rejected.raw_len + 1);
    unsupported.sdp_len = rejected.raw_len;
    rtc_sdp_close(&rejected);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &unsupported), RTC_ERR_SDP);
    ASSERT_STR_EQ(rtc_peer_connection_remote_desc(a.peer)->sdp, stable.sdp);
    ASSERT_EQ(rtc_peer_connection_signaling_state(a.peer), RTC_SIGNALING_STABLE);
    ASSERT_EQ(rtc_data_channel_send_text(channel, "unchanged association"), RTC_OK);
    ASSERT(wait_count(&b.messages, 1, 1000));
    ASSERT_EQ(atomic_load(&b.channels), 1);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
}

TEST(peer_initial_rejected_application_keeps_rtp_without_sctp) {
    rtc_config_t ac = {.stun_server = "127.0.0.1", .stun_port = 9};
    rtc_config_t bc = {.stun_server = "127.0.0.1", .stun_port = 10};
    endpoint_t a = {.peer = rtc_peer_connection_create(&ac)};
    endpoint_t b = {.peer = rtc_peer_connection_create(&bc)};
    ASSERT(a.peer && b.peer);
    rtc_peer_connection_on_track(b.peer, received_track, &b);
    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(a.peer, RTC_KIND_AUDIO, &opus);
    ASSERT(sender && rtc_peer_connection_create_data_channel(a.peer, "rejected", NULL));
    rtc_desc_t offer, answer;
    ASSERT_EQ(rtc_peer_connection_create_offer(a.peer, &offer), RTC_OK);
    rtc_sdp_t parsed = {0};
    ASSERT_EQ(rtc_sdp_parse(&parsed, offer.sdp, offer.sdp_len), RTC_OK);
    for (int i = 0; i < parsed.media_count; i++)
        if (parsed.media[i].media_type == RTC_MEDIA_APPLICATION)
            parsed.media[i].rejected = true;
    ASSERT_EQ(rtc_sdp_generate(&parsed), RTC_OK);
    memcpy(offer.sdp, parsed.raw, parsed.raw_len + 1);
    offer.sdp_len = parsed.raw_len;
    rtc_sdp_close(&parsed);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(a.peer, &offer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(b.peer, &offer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_create_answer(b.peer, &answer), RTC_OK);
    ASSERT(strstr(answer.sdp, "m=application 0 UDP/DTLS/SCTP") != NULL);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(b.peer, &answer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(a.peer, &answer), RTC_OK);
    ASSERT(wait_connected(&a, &b, NULL));
    ASSERT(a.peer->dc_manager.socket == NULL && b.peer->dc_manager.socket == NULL);
    ASSERT_EQ(rtc_rtp_sender_send(sender, opus_silence, sizeof(opus_silence), 960, true), RTC_OK);
    ASSERT(wait_count(&b.frames, 1, 1000));
    ASSERT(a.peer->dc_manager.socket == NULL && b.peer->dc_manager.socket == NULL);
    rtc_peer_connection_close(a.peer);
    rtc_peer_connection_close(b.peer);
    rtc_peer_connection_destroy(a.peer);
    rtc_peer_connection_destroy(b.peer);
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
        RUN_TEST(peer_directional_answers_and_invalid_negotiation_preserve_live_media);
        RUN_TEST(peer_initial_rejected_application_keeps_rtp_without_sctp);
        RUN_TEST(peer_authenticated_collision_preserves_bundle_and_sctp);
        RUN_TEST(peer_authenticated_rtcp_collisions_exhaust_without_reusing_ids);
        RUN_TEST(peer_sctp_live_renegotiation_glare_and_ice_restart);
        RUN_TEST(peer_adds_sctp_after_media_connection);
    }
    rtc_client_cleanup();
    TEST_SUMMARY();
}
