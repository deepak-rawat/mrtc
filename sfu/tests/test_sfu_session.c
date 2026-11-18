#include "../src/rtc_sfu.c"
#include "../../client/src/rtc_peer_internal.h"
#include <rtc/rtc_client.h>
#include <signaling/signaling_msg.h>
#include <rtc/rtc_rtcp.h>
#include "test_harness.h"
#include <stdatomic.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

typedef struct endpoint {
    rtc_peer_connection_t *pc;
    char id[RTC_SFU_ID_SIZE];
    uint32_t epoch;
    _Atomic unsigned packets;
    _Atomic unsigned audio_packets;
    _Atomic unsigned messages;
    _Atomic unsigned estimates;
    _Atomic unsigned feedback;
    _Atomic unsigned acknowledged;
} endpoint_t;

typedef struct {
    rtc_sfu_t *sfu;
    endpoint_t *endpoints[2];
    rtc_mutex_t mutex;
    struct {
        endpoint_t *endpoint;
        char *json;
    } queue[128];
    unsigned head;
    unsigned count;
    bool failed;
} fixture_t;

static int subscription_id(char out[RTC_SFU_SESSION_SIZE], uint32_t epoch, const char *subscriber,
                           const char *publisher) {
    return sig_session_id(out, SIG_SESSION_SUBSCRIBE, epoch, subscriber, publisher);
}

static void send_message(const rtc_sfu_event_t *event, void *user) {
    fixture_t *fixture = user;
    if (event->type == RTC_SFU_DATA) {
        rtc_sfu_send_data(fixture->sfu, event->room, event->owner, event->epoch, event->data,
                          event->data_len);
        return;
    }
    endpoint_t *endpoint = NULL;
    for (unsigned i = 0; i < 2; i++)
        if (fixture->endpoints[i] && !strcmp(fixture->endpoints[i]->id, event->owner) &&
            fixture->endpoints[i]->epoch == event->epoch)
            endpoint = fixture->endpoints[i];
    if (!endpoint)
        return;
    sig_msg_t message = {.version = 2, .epoch = event->epoch};
    message.type = event->type == RTC_SFU_OFFER       ? SIG_MSG_OFFER
                   : event->type == RTC_SFU_ANSWER    ? SIG_MSG_ANSWER
                   : event->type == RTC_SFU_CANDIDATE ? SIG_MSG_CANDIDATE
                                                      : SIG_MSG_ERROR;
    snprintf(message.from, sizeof(message.from), "%s", event->remote);
    snprintf(message.session_id, sizeof(message.session_id), "%s", event->session_id);
    if (message.type == SIG_MSG_CANDIDATE)
        snprintf(message.candidate, sizeof(message.candidate), "%.*s",
                 (int)sizeof(message.candidate) - 1, event->text);
    else if (message.type == SIG_MSG_ERROR)
        snprintf(message.error_msg, sizeof(message.error_msg), "%.*s",
                 (int)sizeof(message.error_msg) - 1, event->text);
    else
        snprintf(message.sdp, sizeof(message.sdp), "%s", event->text);
    char *json = sig_msg_build(&message);
    rtc_mutex_lock(&fixture->mutex);
    if (fixture->count == 128) {
        fixture->failed = true;
    } else {
        unsigned slot = (fixture->head + fixture->count++) % 128;
        fixture->queue[slot].endpoint = endpoint;
        fixture->queue[slot].json = json;
        json = NULL;
    }
    rtc_mutex_unlock(&fixture->mutex);
    free(json);
}

static int handle(fixture_t *fixture, endpoint_t *endpoint, const sig_msg_t *message) {
    rtc_sfu_command_t command = {
        .version = 2, .epoch = endpoint->epoch, .room = "live", .peer_count = 2};
    command.type = message->type == SIG_MSG_OFFER    ? RTC_SFU_OFFER
                   : message->type == SIG_MSG_ANSWER ? RTC_SFU_ANSWER
                                                     : RTC_SFU_CANDIDATE;
    snprintf(command.owner, sizeof(command.owner), "%s", endpoint->id);
    snprintf(command.remote, sizeof(command.remote), "%s", message->to);
    sig_session_id(command.session_id,
                   !strcmp(message->to, "sfu") ? SIG_SESSION_PUBLISH : SIG_SESSION_SUBSCRIBE,
                   endpoint->epoch, endpoint->id, message->to);
    snprintf(command.text, sizeof(command.text), "%s",
             message->type == SIG_MSG_CANDIDATE ? message->candidate : message->sdp);
    for (unsigned i = 0; i < 2; i++)
        snprintf(command.peers[i], sizeof(command.peers[i]), "%s", fixture->endpoints[i]->id);
    return rtc_sfu_handle(fixture->sfu, &command);
}

static void received_packet(const uint8_t *data, size_t len, uint16_t seq, uint32_t timestamp,
                            uint32_t ssrc, bool marker, void *user) {
    (void)data;
    (void)len;
    (void)seq;
    (void)timestamp;
    (void)ssrc;
    (void)marker;
    endpoint_t *endpoint = user;
    atomic_fetch_add(&endpoint->packets, 1);
}

static void received_audio(const uint8_t *data, size_t len, uint16_t seq, uint32_t timestamp,
                           uint32_t ssrc, bool marker, void *user) {
    received_packet(data, len, seq, timestamp, ssrc, marker, user);
    endpoint_t *endpoint = user;
    atomic_fetch_add(&endpoint->audio_packets, 1);
}

static void received_track(rtc_rtp_receiver_t *receiver, void *user) {
    rtc_rtp_receiver_on_frame(
        receiver,
        rtc_rtp_receiver_kind(receiver) == RTC_KIND_AUDIO ? received_audio : received_packet, user);
}

static void received_message(const uint8_t *data, size_t len, void *user) {
    (void)data;
    (void)len;
    endpoint_t *endpoint = user;
    atomic_fetch_add(&endpoint->messages, 1);
}

static void bitrate_estimate(uint32_t bitrate, void *user) {
    endpoint_t *endpoint = user;
    if (bitrate)
        atomic_fetch_add(&endpoint->estimates, 1);
}

static void received_feedback(const uint8_t *data, size_t len, void *user) {
    endpoint_t *endpoint = user;
    size_t offset = 0;
    while (len - offset >= 4) {
        size_t size = (((size_t)data[offset + 2] << 8) | data[offset + 3]) * 4 + 4;
        if (size > len - offset)
            return;
        uint8_t pt, fmt;
        if (rtc_rtcp_get_pt_fmt(data + offset, size, &pt, &fmt) && pt == RTCP_PT_RTPFB &&
            fmt == 15) {
            rtc_rtcp_twcc_t feedback;
            if (rtc_rtcp_parse_twcc(&feedback, data + offset, size) != RTC_OK)
                return;
            atomic_fetch_add(&endpoint->feedback, 1);
            for (int i = 0; i < feedback.item_count; i++)
                if (feedback.items[i].received)
                    atomic_fetch_add(&endpoint->acknowledged, 1);
        }
        offset += size;
    }
}

static bool pump(fixture_t *fixture) {
    for (;;) {
        rtc_mutex_lock(&fixture->mutex);
        if (!fixture->count) {
            bool failed = fixture->failed;
            rtc_mutex_unlock(&fixture->mutex);
            return !failed;
        }
        endpoint_t *endpoint = fixture->queue[fixture->head].endpoint;
        char *json = fixture->queue[fixture->head].json;
        fixture->head = (fixture->head + 1) % 128;
        fixture->count--;
        rtc_mutex_unlock(&fixture->mutex);
        sig_msg_t message;
        int rc = sig_msg_parse(&message, json, strlen(json));
        free(json);
        if (rc != RTC_OK)
            return false;
        if (message.type == SIG_MSG_OFFER || message.type == SIG_MSG_ANSWER) {
            rtc_desc_t description = {.type = message.type == SIG_MSG_OFFER ? RTC_SDP_OFFER
                                                                            : RTC_SDP_ANSWER};
            snprintf(description.sdp, sizeof(description.sdp), "%s", message.sdp);
            description.sdp_len = strlen(description.sdp);
            if (rtc_peer_connection_set_remote_desc(endpoint->pc, &description) != RTC_OK)
                return false;
            if (message.type == SIG_MSG_OFFER) {
                rtc_desc_t answer;
                if (rtc_peer_connection_create_answer(endpoint->pc, &answer) != RTC_OK ||
                    rtc_peer_connection_set_local_desc(endpoint->pc, &answer) != RTC_OK)
                    return false;
                sig_msg_t response = {.type = SIG_MSG_ANSWER};
                snprintf(response.to, sizeof(response.to), "%s", message.from);
                snprintf(response.sdp, sizeof(response.sdp), "%s", answer.sdp);
                if (handle(fixture, endpoint, &response) != RTC_OK)
                    return false;
            }
        } else if (message.type == SIG_MSG_CANDIDATE) {
            rtc_ice_candidate_desc_t candidate = {0};
            snprintf(candidate.candidate, sizeof(candidate.candidate), "%s", message.candidate);
            if (rtc_peer_connection_add_ice_candidate(
                    endpoint->pc, message.candidate[0] ? &candidate : NULL) != RTC_OK)
                return false;
        } else if (message.type == SIG_MSG_ERROR) {
            return false;
        }
    }
}

static bool offer(fixture_t *fixture, endpoint_t *publisher) {
    rtc_desc_t description;
    if (rtc_peer_connection_create_offer(publisher->pc, &description) != RTC_OK)
        return false;
    rtc_sdp_t sdp = {0};
    int rc = rtc_sdp_parse(&sdp, description.sdp, description.sdp_len);
    for (int i = 0; rc == RTC_OK && i < sdp.media_count; i++) {
        if (sdp.media[i].media_type == RTC_MEDIA_AUDIO) {
            sdp.media[i].extmap_count = 0;
            rc = rtc_sdp_media_add_extmap(&sdp.media[i], 9, RTC_EXT_URI_TRANSPORT_CC);
        }
    }
    if (rc == RTC_OK)
        rc = rtc_sdp_generate(&sdp);
    if (rc == RTC_OK) {
        snprintf(description.sdp, sizeof(description.sdp), "%s", sdp.raw);
        description.sdp_len = sdp.raw_len;
    }
    rtc_sdp_close(&sdp);
    if (rc != RTC_OK || rtc_peer_connection_set_local_desc(publisher->pc, &description) != RTC_OK)
        return false;
    sig_msg_t message = {.type = SIG_MSG_OFFER, .to = "sfu"};
    snprintf(message.sdp, sizeof(message.sdp), "%s", description.sdp);
    return handle(fixture, publisher, &message) == RTC_OK && pump(fixture);
}

static bool wait_count(fixture_t *fixture, _Atomic unsigned *count, unsigned target) {
    uint64_t deadline = rtc_time_ms() + 3000;
    while (rtc_time_ms() < deadline) {
        if (!pump(fixture))
            return false;
        if (atomic_load(count) >= target)
            return true;
        SLEEP_MS(5);
    }
    return false;
}

TEST(sfu_live_tracks_restart_sctp_and_publisher_feedback) {
    fixture_t fixture = {0};
    ASSERT_EQ(rtc_mutex_init(&fixture.mutex), RTC_OK);
    fixture.sfu = rtc_sfu_create(&(rtc_sfu_config_t){.listen_ip = "127.0.0.1",
                                                     .subscription_id = subscription_id,
                                                     .on_event = send_message,
                                                     .user = &fixture});
    ASSERT(fixture.sfu != NULL);
    endpoint_t publisher = {.pc = rtc_peer_connection_create(NULL), .id = "publisher", .epoch = 1};
    endpoint_t subscriber = {
        .pc = rtc_peer_connection_create(NULL), .id = "subscriber", .epoch = 1};
    fixture.endpoints[0] = &publisher;
    fixture.endpoints[1] = &subscriber;
    ASSERT(publisher.pc && subscriber.pc);
    rtc_peer_connection_on_track(subscriber.pc, received_track, &subscriber);
    rtc_peer_connection_on_bitrate_estimate(publisher.pc, bitrate_estimate, &publisher);
    rtc_codec_t vp8 = {.payload_type = 96, .mime_type = "video/VP8", .clock_rate = 90000};
    rtc_rtp_transceiver_init_t init = {.direction = RTC_DIR_SENDONLY};
    rtc_rtp_transceiver_t *video =
        rtc_peer_connection_add_transceiver(publisher.pc, RTC_KIND_VIDEO, &vp8, &init);
    ASSERT(video != NULL);
    rtc_rtp_sender_t *sender = rtc_rtp_transceiver_sender(video);
    rtc_data_channel_t *channel =
        rtc_peer_connection_create_data_channel(publisher.pc, "live", NULL);
    ASSERT(channel != NULL);
    rtc_data_channel_on_message(channel, received_message, &publisher);
    ASSERT(offer(&fixture, &publisher));
    uint64_t deadline = rtc_time_ms() + 5000;
    bool observing = false;
    const uint8_t payload[] = {0x10, 0, 0, 0, 0};
    while (rtc_time_ms() < deadline) {
        ASSERT(pump(&fixture));
        if (rtc_peer_connection_connection_state(publisher.pc) == RTC_CONNECTION_CONNECTED) {
            if (!observing) {
                rtc_transport_on_rtcp(publisher.pc->runtime_transport, received_feedback,
                                      &publisher);
                observing = true;
            }
            ASSERT_EQ(rtc_rtp_sender_send(sender, payload, sizeof(payload), 3000, true), RTC_OK);
        }
        if (atomic_load(&subscriber.packets) > 5 && rtc_data_channel_state(channel) == RTC_DC_OPEN
#ifdef MRTC_ENABLE_TWCC
            && atomic_load(&publisher.estimates) > 0
#endif
        )
            break;
        SLEEP_MS(20);
    }
    ASSERT(atomic_load(&subscriber.packets) > 5);
#ifdef MRTC_ENABLE_TWCC
    printf("    TWCC feedback=%u acknowledged=%u estimates=%u\n", atomic_load(&publisher.feedback),
           atomic_load(&publisher.acknowledged), atomic_load(&publisher.estimates));
    ASSERT(atomic_load(&publisher.feedback) > 0);
    ASSERT(atomic_load(&publisher.acknowledged) > 0);
    ASSERT(atomic_load(&publisher.estimates) > 0);
#endif
    ASSERT_EQ(rtc_data_channel_send_text(channel, "before"), RTC_OK);
    ASSERT(wait_count(&fixture, &publisher.messages, 1));
    sfu_session_t *session = find_session(fixture.sfu, "live", publisher.id, "sfu", 1);
    sfu_session_t *subscription = find_session(fixture.sfu, "live", subscriber.id, publisher.id, 1);
    ASSERT(session && subscription);
    rtc_transport_t *transport = session->transport;
    rtc_transport_t *downstream = subscription->transport;
    rtc_sctp_t *association = session->sctp;
    rtc_consumer_t *consumer = subscription->consumers[0];
    rtc_rtp_parameters_t original;
    ASSERT_EQ(rtc_consumer_get_rtp_parameters(consumer, &original), RTC_OK);

    rtc_rtp_transceiver_set_direction(video, RTC_DIR_INACTIVE);
    ASSERT(offer(&fixture, &publisher));
    ASSERT_EQ(subscription->description.media[0].direction, 3);
    rtc_consumer_stats_t stats;
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(stats.paused);
    rtc_rtp_transceiver_set_direction(video, RTC_DIR_SENDONLY);
    rtc_codec_t opus = {
        .payload_type = 111, .mime_type = "audio/opus", .clock_rate = 48000, .channels = 2};
    rtc_rtp_sender_t *audio = rtc_peer_connection_add_track(publisher.pc, RTC_KIND_AUDIO, &opus);
    ASSERT(audio != NULL);
    ASSERT(offer(&fixture, &publisher));
    ASSERT_EQ(subscription->description.media_count, 2);
    ASSERT(subscription->transport == downstream);
    ASSERT(subscription->consumers[0] == consumer);
    const uint8_t silence[] = {0xf8, 0xff, 0xfe};
    ASSERT_EQ(rtc_rtp_sender_send(audio, silence, sizeof(silence), 960, false), RTC_OK);
    ASSERT(wait_count(&fixture, &subscriber.audio_packets, 1));
#ifdef MRTC_ENABLE_TWCC
    ASSERT_EQ(
        rtc_sdp_media_find_extmap_id(&session->description.media[0], RTC_EXT_URI_TRANSPORT_CC), 5);
    ASSERT_EQ(session->description.media[2].extmap_count, 0);
    ASSERT(strstr(session->description.raw, "a=rtcp-fb:96 transport-cc") != NULL);
#endif
    rtc_ice_parameters_t previous;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &previous), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_restart_ice(publisher.pc), RTC_OK);
    ASSERT(offer(&fixture, &publisher));
    rtc_ice_parameters_t current;
    ASSERT_EQ(rtc_transport_get_ice_parameters(transport, &current), RTC_OK);
    ASSERT(strcmp(previous.username_fragment, current.username_fragment) != 0);
    ASSERT(session->transport == transport && session->sctp == association);
    unsigned packets = atomic_load(&subscriber.packets);
    deadline = rtc_time_ms() + 3000;
    while (rtc_time_ms() < deadline && atomic_load(&subscriber.packets) <= packets) {
        ASSERT(pump(&fixture));
        ASSERT_EQ(rtc_rtp_sender_send(sender, payload, sizeof(payload), 3000, true), RTC_OK);
        SLEEP_MS(10);
    }
    ASSERT(atomic_load(&subscriber.packets) > packets);
    ASSERT_EQ(rtc_data_channel_state(channel), RTC_DC_OPEN);
    ASSERT_EQ(rtc_data_channel_send_text(channel, "after"), RTC_OK);
    ASSERT(wait_count(&fixture, &publisher.messages, 2));
    rtc_rtp_parameters_t retained;
    ASSERT_EQ(rtc_consumer_get_rtp_parameters(consumer, &retained), RTC_OK);
    ASSERT_EQ(retained.ssrc, original.ssrc);

    rtc_desc_t invalid;
    ASSERT_EQ(rtc_peer_connection_create_offer(publisher.pc, &invalid), RTC_OK);
    rtc_sdp_t invalid_sdp = {0};
    ASSERT_EQ(rtc_sdp_parse(&invalid_sdp, invalid.sdp, invalid.sdp_len), RTC_OK);
    invalid_sdp.media[0].payload_type = 97;
    ASSERT_EQ(rtc_sdp_generate(&invalid_sdp), RTC_OK);
    sig_msg_t message = {.type = SIG_MSG_OFFER, .to = "sfu"};
    snprintf(message.sdp, sizeof(message.sdp), "%s", invalid_sdp.raw);
    ASSERT_EQ(handle(&fixture, &publisher, &message), RTC_ERR_SDP);
    ASSERT_EQ(session->remote.media[0].payload_type, 96);
    invalid_sdp.media[0].payload_type = 96;
    snprintf(invalid_sdp.media[0].codec_name, sizeof(invalid_sdp.media[0].codec_name), "H264");
    ASSERT_EQ(rtc_sdp_generate(&invalid_sdp), RTC_OK);
    snprintf(message.sdp, sizeof(message.sdp), "%s", invalid_sdp.raw);
    ASSERT_EQ(handle(&fixture, &publisher, &message), RTC_ERR_SDP);
    ASSERT_STR_EQ(session->remote.media[0].codec_name, "VP8");
    ASSERT(session->transport == transport && session->sctp == association);
    ASSERT_EQ(rtc_data_channel_send_text(channel, "after rejection"), RTC_OK);
    ASSERT(wait_count(&fixture, &publisher.messages, 3));
    rtc_sdp_close(&invalid_sdp);

    sig_msg_t candidate = {.type = SIG_MSG_CANDIDATE, .to = "sfu"};
    const char *tcp_candidates[] = {"candidate:2707821723 1 tcp 1518214911 127.0.0.1 9 typ host "
                                    "tcptype active generation 1",
                                    "candidate:3572778064 1 tcp 1518283007 ::1 9 typ host tcptype "
                                    "active generation 1"};
    for (size_t i = 0; i < sizeof(tcp_candidates) / sizeof(tcp_candidates[0]); i++) {
        snprintf(candidate.candidate, sizeof(candidate.candidate), "%s", tcp_candidates[i]);
        ASSERT_EQ(handle(&fixture, &publisher, &candidate), RTC_OK);
    }
    snprintf(candidate.candidate, sizeof(candidate.candidate),
             "candidate:1 1 tcp 1518214911 127.0.0.1 9 typ host");
    ASSERT_EQ(handle(&fixture, &publisher, &candidate), RTC_ERR_SDP);
    snprintf(candidate.candidate, sizeof(candidate.candidate),
             "candidate:1 1 quic 1518214911 127.0.0.1 9 typ host");
    ASSERT_EQ(handle(&fixture, &publisher, &candidate), RTC_ERR_SDP);
    ASSERT_EQ(rtc_data_channel_send_text(channel, "after candidate filtering"), RTC_OK);
    ASSERT(wait_count(&fixture, &publisher.messages, 4));

    rtc_sfu_leave(fixture.sfu, "live", subscriber.id, 0);
    rtc_sfu_leave(fixture.sfu, "live", publisher.id, 0);
    rtc_peer_connection_close(publisher.pc);
    rtc_peer_connection_close(subscriber.pc);
    rtc_peer_connection_destroy(publisher.pc);
    rtc_peer_connection_destroy(subscriber.pc);
    rtc_sfu_destroy(fixture.sfu);
    ASSERT(pump(&fixture));
    rtc_mutex_destroy(&fixture.mutex);
}

TEST(overlapping_epochs_keep_distinct_sctp_associations) {
    fixture_t fixture = {0};
    ASSERT_EQ(rtc_mutex_init(&fixture.mutex), RTC_OK);
    fixture.sfu = rtc_sfu_create(&(rtc_sfu_config_t){.listen_ip = "127.0.0.1",
                                                     .subscription_id = subscription_id,
                                                     .on_event = send_message,
                                                     .user = &fixture});
    ASSERT(fixture.sfu != NULL);
    endpoint_t active = {.pc = rtc_peer_connection_create(NULL), .id = "alice", .epoch = 1};
    endpoint_t pending = {.pc = rtc_peer_connection_create(NULL), .id = "alice", .epoch = 2};
    fixture.endpoints[0] = &active;
    fixture.endpoints[1] = &pending;
    ASSERT(active.pc && pending.pc);
    rtc_data_channel_t *old_channel =
        rtc_peer_connection_create_data_channel(active.pc, "chat", NULL);
    rtc_data_channel_t *new_channel =
        rtc_peer_connection_create_data_channel(pending.pc, "chat", NULL);
    ASSERT(old_channel && new_channel);
    rtc_data_channel_on_message(old_channel, received_message, &active);
    rtc_data_channel_on_message(new_channel, received_message, &pending);
    ASSERT(offer(&fixture, &active));
    ASSERT(offer(&fixture, &pending));
    uint64_t deadline = rtc_time_ms() + 3000;
    while (rtc_time_ms() < deadline && (rtc_data_channel_state(old_channel) != RTC_DC_OPEN ||
                                        rtc_data_channel_state(new_channel) != RTC_DC_OPEN)) {
        ASSERT(pump(&fixture));
        SLEEP_MS(5);
    }
    ASSERT_EQ(rtc_data_channel_state(old_channel), RTC_DC_OPEN);
    ASSERT_EQ(rtc_data_channel_state(new_channel), RTC_DC_OPEN);
    sfu_session_t *old_session = find_session(fixture.sfu, "live", "alice", "sfu", 1);
    sfu_session_t *new_session_ptr = find_session(fixture.sfu, "live", "alice", "sfu", 2);
    ASSERT(old_session && new_session_ptr);
    ASSERT(old_session->transport != new_session_ptr->transport);
    ASSERT(old_session->sctp != new_session_ptr->sctp);
    ASSERT_STR_EQ(old_session->session_id, "p:1:alice");
    ASSERT_STR_EQ(new_session_ptr->session_id, "p:2:alice");
    ASSERT_EQ(rtc_data_channel_send_text(old_channel, "old"), RTC_OK);
    ASSERT_EQ(rtc_data_channel_send_text(new_channel, "new"), RTC_OK);
    ASSERT(wait_count(&fixture, &active.messages, 1));
    ASSERT(wait_count(&fixture, &pending.messages, 1));
    rtc_sfu_leave(fixture.sfu, "live", "alice", 1);
    ASSERT(find_session(fixture.sfu, "live", "alice", "sfu", 1) == NULL);
    ASSERT(find_session(fixture.sfu, "live", "alice", "sfu", 2) == new_session_ptr);
    ASSERT_EQ(rtc_data_channel_send_text(new_channel, "after retirement"), RTC_OK);
    ASSERT(wait_count(&fixture, &pending.messages, 2));
    rtc_sfu_close(fixture.sfu);
    rtc_peer_connection_close(active.pc);
    rtc_peer_connection_close(pending.pc);
    rtc_peer_connection_destroy(active.pc);
    rtc_peer_connection_destroy(pending.pc);
    rtc_sfu_destroy(fixture.sfu);
    ASSERT(pump(&fixture));
    rtc_mutex_destroy(&fixture.mutex);
}

int main(void) {
    rtc_client_init();
    RUN_TEST(sfu_live_tracks_restart_sctp_and_publisher_feedback);
    RUN_TEST(overlapping_epochs_keep_distinct_sctp_associations);
    rtc_client_cleanup();
    TEST_SUMMARY();
    return _test_fail_count ? 1 : 0;
}
