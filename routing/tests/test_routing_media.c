/*
 * SFU producer/consumer skeleton tests.
 */
#include <rtc/rtc.h>
#include <rtc/rtc_worker.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_router.h>
#include <rtc/rtc_transport.h>
#include <rtc/rtc_producer.h>
#include <rtc/rtc_consumer.h>
#include <rtc/rtc_rtcp.h>
#include "test_harness.h"

#include <stdatomic.h>

#ifdef MRTC_ROUTING_MEDIA_CODEC_TEST
#  include <media/media_pipeline.h>
#  include "vp8_packetizer.h"

static void pause_ms(int ms) {
#  ifdef _WIN32
    Sleep((DWORD)ms);
#  else
    usleep((unsigned)ms * 1000);
#  endif
}
#endif

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_router_t *router;
    rtc_transport_t *transport;
} test_env_t;

static bool make_env(test_env_t *env) {
    memset(env, 0, sizeof(*env));
    env->worker = rtc_worker_create(NULL);
    if (!env->worker)
        return false;
    env->listener = rtc_listener_create(env->worker, NULL);
    if (!env->listener)
        return false;
    env->router = rtc_router_create(env->worker, NULL);
    if (!env->router)
        return false;
    env->transport = rtc_router_create_transport(env->router, &(rtc_transport_config_t){
                                                                  .listener = env->listener,
                                                                  .ice_mode = RTC_ICE_MODE_LITE,
                                                              });
    return env->transport != NULL;
}

static void close_env(test_env_t *env) {
    rtc_transport_destroy(env->transport);
    rtc_router_destroy(env->router);
    rtc_listener_destroy(env->listener);
    rtc_worker_destroy(env->worker);
}

static rtc_producer_options_t video_producer_options(void) {
    rtc_producer_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.kind = RTC_MEDIA_KIND_VIDEO;
    opts.label = "camera";
    opts.rtp.ssrc = 0x12345678;
    opts.rtp.codec_count = 1;
    opts.rtp.codecs[0].kind = RTC_MEDIA_KIND_VIDEO;
    opts.rtp.codecs[0].payload_type = 96;
    memcpy(opts.rtp.codecs[0].mime_type, "video/VP8", sizeof("video/VP8"));
    opts.rtp.codecs[0].clock_rate = 90000;
    return opts;
}

TEST(producer_create_stats) {
    test_env_t env;
    ASSERT(make_env(&env));

    rtc_producer_options_t opts = video_producer_options();
    rtc_producer_t *producer = rtc_transport_produce(env.transport, &opts);
    ASSERT(producer != NULL);
    ASSERT(rtc_producer_id(producer)[0] != '\0');

    rtc_producer_stats_t stats;
    ASSERT_EQ(rtc_producer_get_stats(producer, &stats), RTC_OK);
    ASSERT(!stats.closed);
    ASSERT_EQ(stats.kind, RTC_MEDIA_KIND_VIDEO);

    rtc_producer_close(producer);
    ASSERT_EQ(rtc_producer_get_stats(producer, &stats), RTC_OK);
    ASSERT(stats.closed);

    rtc_producer_destroy(producer);
    close_env(&env);
}

TEST(consumer_create_pause_resume) {
    test_env_t env;
    ASSERT(make_env(&env));

    rtc_producer_options_t opts = video_producer_options();
    rtc_producer_t *producer = rtc_transport_produce(env.transport, &opts);
    ASSERT(producer != NULL);

    rtc_consumer_t *consumer = rtc_transport_consume(env.transport, &(rtc_consumer_options_t){
                                                                        .producer = producer,
                                                                        .paused = true,
                                                                    });
    ASSERT(consumer != NULL);
    ASSERT(rtc_consumer_id(consumer)[0] != '\0');

    rtc_consumer_stats_t stats;
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(!stats.closed);
    ASSERT(stats.paused);
    ASSERT_EQ(stats.kind, RTC_MEDIA_KIND_VIDEO);

    rtc_rtp_parameters_t rtp;
    ASSERT_EQ(rtc_consumer_get_rtp_parameters(consumer, &rtp), RTC_OK);
    ASSERT_EQ(rtp.codec_count, 1);
    ASSERT_EQ(rtp.codecs[0].payload_type, 96);
    ASSERT_STR_EQ(rtp.codecs[0].mime_type, "video/VP8");
    ASSERT(rtp.ssrc != 0);
    ASSERT(rtp.ssrc != opts.rtp.ssrc);
    ASSERT(rtp.mid[0] != '\0');

    rtc_consumer_resume(consumer);
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(!stats.paused);
    rtc_consumer_pause(consumer);
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &stats), RTC_OK);
    ASSERT(stats.paused);

    rtc_consumer_destroy(consumer);
    rtc_producer_destroy(producer);
    close_env(&env);
}

TEST(media_rejects_closed_inputs) {
    test_env_t env;
    ASSERT(make_env(&env));

    rtc_producer_options_t opts = video_producer_options();
    rtc_transport_close(env.transport);
    ASSERT(rtc_transport_produce(env.transport, &opts) == NULL);

    close_env(&env);
}

TEST(consumer_rejects_malformed_feedback) {
    test_env_t env;
    ASSERT(make_env(&env));
    rtc_producer_options_t options = video_producer_options();
    rtc_producer_t *producer = rtc_transport_produce(env.transport, &options);
    ASSERT(producer != NULL);
    rtc_consumer_t *consumer =
        rtc_transport_consume(env.transport, &(rtc_consumer_options_t){.producer = producer});
    ASSERT(consumer != NULL);
    uint8_t malformed[] = {0x81, 206, 0xff, 0xff};
    ASSERT_EQ(rtc_consumer_handle_rtcp(consumer, malformed, sizeof(malformed)), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_consumer_handle_rtcp(consumer, malformed, 3), RTC_ERR_INVALID);
    rtc_consumer_close(consumer);
    ASSERT_EQ(rtc_consumer_handle_rtcp(consumer, malformed, sizeof(malformed)), RTC_ERR_INVALID);
    rtc_consumer_destroy(consumer);
    rtc_producer_destroy(producer);
    close_env(&env);
}

#ifdef MRTC_ROUTING_MEDIA_CODEC_TEST
typedef struct {
    rtc_socket_t socket;
    rtc_addr_t server, local;
    bool drop_server_packets;
    _Atomic bool running;
    _Atomic int drop_seq, capture_seq;
    _Atomic int drops_remaining, dropped, captured;
    _Atomic bool wire_mismatch;
    _Atomic int errors;
} routed_proxy_t;

static void *routed_proxy_loop(void *user) {
    routed_proxy_t *proxy = user;
    struct sockaddr_storage client = {0};
    socklen_t client_len = 0;
    uint8_t original[2048];
    size_t original_len = 0;
    while (atomic_load(&proxy->running)) {
        uint8_t data[2048];
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int len = (int)recvfrom(proxy->socket, (char *)data, sizeof(data), 0,
                                (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            pause_ms(1);
            continue;
        }
        bool server = ((struct sockaddr_in *)&from)->sin_port ==
                      ((struct sockaddr_in *)&proxy->server.addr)->sin_port;
        if (!server) {
            client = from;
            client_len = from_len;
        }
        if (server == proxy->drop_server_packets && len >= 12 && (data[0] & 0xc0) == 0x80 &&
            !(data[1] >= 192 && data[1] <= 223)) {
            int seq = (int)((uint16_t)data[2] << 8 | data[3]);
            if (seq == atomic_load(&proxy->capture_seq)) {
                if (!original_len) {
                    memcpy(original, data, (size_t)len);
                    original_len = (size_t)len;
                } else if (original_len != (size_t)len ||
                           memcmp(original, data, original_len) != 0) {
                    atomic_store(&proxy->wire_mismatch, true);
                }
                atomic_fetch_add(&proxy->captured, 1);
            }
            if (seq == atomic_load(&proxy->drop_seq) && atomic_load(&proxy->drops_remaining) > 0) {
                atomic_fetch_sub(&proxy->drops_remaining, 1);
                atomic_fetch_add(&proxy->dropped, 1);
                continue;
            }
        }
        if (server && !client_len)
            continue;
        const struct sockaddr *target =
            server ? (struct sockaddr *)&client : (struct sockaddr *)&proxy->server.addr;
        socklen_t target_len = server ? client_len : proxy->server.len;
        if (sendto(proxy->socket, (const char *)data, (size_t)len, 0, target, target_len) != len)
            atomic_fetch_add(&proxy->errors, 1);
    }
    return NULL;
}

static bool routed_proxy_create(routed_proxy_t *proxy, rtc_listener_t *server, bool downstream,
                                rtc_thread_t *thread) {
    memset(proxy, 0, sizeof(*proxy));
    proxy->drop_server_packets = downstream;
    proxy->drop_seq = proxy->capture_seq = -1;
    if (rtc_listener_get_local_addr(server, &proxy->server) != RTC_OK)
        return false;
    uint16_t port = ntohs(((struct sockaddr_in *)&proxy->server.addr)->sin_port);
    if (rtc_addr_from_string(&proxy->server, "127.0.0.1", port) != RTC_OK ||
        rtc_addr_from_string(&proxy->local, "127.0.0.1", 0) != RTC_OK)
        return false;
    proxy->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy->socket == RTC_INVALID_SOCKET ||
        bind(proxy->socket, (struct sockaddr *)&proxy->local.addr, proxy->local.len) != 0 ||
        getsockname(proxy->socket, (struct sockaddr *)&proxy->local.addr, &proxy->local.len) != 0 ||
        rtc_set_nonblocking(proxy->socket) != RTC_OK)
        return false;
    atomic_store(&proxy->running, true);
    return rtc_thread_create(thread, routed_proxy_loop, proxy) == RTC_OK;
}

typedef struct {
    media_pipeline_t *pipeline;
    media_recv_stream_t *stream;
    _Atomic int packets, frames, plis, feedback, feedback_errors;
    _Atomic uint16_t last_seq;
    rtc_consumer_t *consumer;
} routed_receiver_t;

static void routed_video(const char *peer, const char *label, const video_frame_t *frame,
                         void *user) {
    (void)peer;
    (void)label;
    (void)frame;
    routed_receiver_t *receiver = user;
    atomic_fetch_add(&receiver->frames, 1);
}

static void routed_rtp(const rtc_rtp_packet_t *packet, void *user) {
    routed_receiver_t *receiver = user;
    media_pipeline_recv_rtp(receiver->pipeline, receiver->stream, packet->payload,
                            (int)packet->payload_len, packet->header.seq, packet->header.timestamp,
                            packet->header.marker);
    atomic_store(&receiver->last_seq, packet->header.seq);
    atomic_fetch_add(&receiver->packets, 1);
}

static void routed_feedback(const uint8_t *data, size_t len, void *user) {
    routed_receiver_t *receiver = user;
    if (rtc_consumer_handle_rtcp(receiver->consumer, data, len) != RTC_OK)
        atomic_fetch_add(&receiver->feedback_errors, 1);
    atomic_fetch_add(&receiver->feedback, 1);
}

static void routed_upstream_feedback(const uint8_t *data, size_t len, void *user) {
    routed_receiver_t *receiver = user;
    rtc_rtcp_pli_t pli;
    if (rtc_rtcp_parse_pli(&pli, data, len) == RTC_OK && data[1] == RTCP_PT_PSFB)
        atomic_fetch_add(&receiver->plis, 1);
}

static bool wait_count(_Atomic int *value, int expected) {
    uint64_t deadline = rtc_time_ms() + 3000;
    while (atomic_load(value) < expected && rtc_time_ms() < deadline)
        pause_ms(1);
    return atomic_load(value) >= expected;
}

static bool routed_connect(rtc_transport_t *client, rtc_transport_t *server,
                           routed_proxy_t *proxy) {
    rtc_ice_parameters_t ci, si;
    rtc_dtls_parameters_t cd, sd;
    rtc_transport_candidate_t candidate = {0};
    strcpy(candidate.address, "127.0.0.1");
    strcpy(candidate.protocol, "udp");
    candidate.port = ntohs(((struct sockaddr_in *)&proxy->local.addr)->sin_port);
    if (rtc_transport_get_ice_parameters(client, &ci) != RTC_OK ||
        rtc_transport_get_ice_parameters(server, &si) != RTC_OK ||
        rtc_transport_set_remote_ice_parameters(client, &si) != RTC_OK ||
        rtc_transport_set_remote_ice_parameters(server, &ci) != RTC_OK ||
        rtc_transport_add_remote_candidate(client, &candidate) != RTC_OK ||
        rtc_transport_add_remote_candidate(server, &candidate) != RTC_OK ||
        rtc_transport_set_dtls_role(server, RTC_TRANSPORT_DTLS_ROLE_SERVER) != RTC_OK ||
        rtc_transport_get_dtls_parameters(client, &cd) != RTC_OK ||
        rtc_transport_get_dtls_parameters(server, &sd) != RTC_OK ||
        rtc_transport_set_remote_dtls_parameters(client, &sd) != RTC_OK ||
        rtc_transport_set_remote_dtls_parameters(server, &cd) != RTC_OK ||
        rtc_transport_start_ice(client) != RTC_OK)
        return false;
    uint64_t deadline = rtc_time_ms() + 5000;
    rtc_transport_stats_t cs, ss;
    do {
        rtc_transport_get_stats(client, &cs);
        rtc_transport_get_stats(server, &ss);
        if (cs.selected_tuple_valid && ss.selected_tuple_valid)
            break;
        pause_ms(1);
    } while (rtc_time_ms() < deadline);
    if (!cs.selected_tuple_valid || !ss.selected_tuple_valid ||
        rtc_transport_start_dtls(client) != RTC_OK)
        return false;
    do {
        rtc_transport_get_stats(client, &cs);
        rtc_transport_get_stats(server, &ss);
        if (cs.srtp_ready && ss.srtp_ready)
            return true;
        pause_ms(1);
    } while (rtc_time_ms() < deadline);
    return false;
}

static int routed_send(rtc_transport_t *publisher, uint16_t seq, uint32_t timestamp,
                       const rtc_vp8_payload_t *payload) {
    rtc_rtp_packet_t packet;
    int rc = rtc_rtp_build(&packet, 96, seq, timestamp, 0x12345678, payload->marker, payload->data,
                           payload->len);
    size_t len = packet.buf_len;
    return rc == RTC_OK ? rtc_transport_send_rtp(publisher, packet.buf, &len, sizeof(packet.buf))
                        : rc;
}

static int routed_nack(rtc_transport_t *subscriber, uint32_t ssrc, uint16_t seq) {
    uint8_t wire[64];
    size_t len;
    int rc = rtc_rtcp_build_nack(wire, sizeof(wire), &len, 1, ssrc, &seq, 1);
    return rc == RTC_OK ? rtc_transport_send_rtcp(subscriber, wire, &len, sizeof(wire)) : rc;
}

typedef struct {
    routed_receiver_t *receiver;
    uint8_t data[64];
    size_t len;
} routed_feedback_args_t;

static void routed_inject_feedback(void *user) {
    routed_feedback_args_t *args = user;
    routed_feedback(args->data, args->len, args->receiver);
}

static void routed_barrier(void *user) {
    (void)user;
}

TEST(routed_loss_wrap_and_bounded_retransmission) {
    test_env_t sfu;
    ASSERT(make_env(&sfu));
    rtc_transport_t *outbound = rtc_router_create_transport(
        sfu.router,
        &(rtc_transport_config_t){.listener = sfu.listener, .ice_mode = RTC_ICE_MODE_LITE});
    ASSERT(outbound != NULL);
    rtc_worker_t *client_worker = rtc_worker_create(NULL);
    ASSERT(client_worker != NULL);
    rtc_listener_t *publisher_listener = rtc_listener_create(client_worker, NULL);
    rtc_listener_t *subscriber_listener = rtc_listener_create(client_worker, NULL);
    ASSERT(publisher_listener != NULL && subscriber_listener != NULL);
    rtc_transport_t *publisher = rtc_transport_create(
        client_worker,
        &(rtc_transport_config_t){.listener = publisher_listener, .ice_mode = RTC_ICE_MODE_FULL});
    rtc_transport_t *subscriber = rtc_transport_create(
        client_worker,
        &(rtc_transport_config_t){.listener = subscriber_listener, .ice_mode = RTC_ICE_MODE_FULL});
    ASSERT(publisher != NULL && subscriber != NULL);
    routed_proxy_t upstream, downstream;
    rtc_thread_t upstream_thread, downstream_thread;
    ASSERT(routed_proxy_create(&upstream, sfu.listener, false, &upstream_thread));
    ASSERT(routed_proxy_create(&downstream, sfu.listener, true, &downstream_thread));
    ASSERT(routed_connect(publisher, sfu.transport, &upstream));
    ASSERT(routed_connect(subscriber, outbound, &downstream));
    rtc_producer_options_t options = video_producer_options();
    rtc_producer_t *producer = rtc_transport_produce(sfu.transport, &options);
    ASSERT(producer != NULL);
    rtc_consumer_t *consumer =
        rtc_transport_consume(outbound, &(rtc_consumer_options_t){.producer = producer});
    ASSERT(consumer != NULL);
    rtc_rtp_parameters_t params;
    ASSERT_EQ(rtc_consumer_get_rtp_parameters(consumer, &params), RTC_OK);
    routed_receiver_t receiver = {.consumer = consumer};
    media_renderer_t renderer = {.on_video_frame = routed_video, .user = &receiver};
    receiver.pipeline = media_pipeline_create(&(media_pipeline_config_t){.renderer = &renderer});
    ASSERT(receiver.pipeline != NULL);
    receiver.stream =
        media_pipeline_add_recv_stream(receiver.pipeline, "sfu", "video", params.ssrc, true, "VP8");
    ASSERT(receiver.stream != NULL);
    rtc_transport_on_rtp(subscriber, routed_rtp, &receiver);
    rtc_transport_on_rtcp(outbound, routed_feedback, &receiver);
    rtc_transport_on_rtcp(publisher, routed_upstream_feedback, &receiver);

    rtc_vp8_payload_t orphan = {.data = {0, 1}, .len = 2, .marker = false};
    uint16_t source_seq = 1000;
    int received = 1;
    ASSERT_EQ(routed_send(publisher, source_seq, 1, &orphan), RTC_OK);
    ASSERT(wait_count(&receiver.packets, received));
    uint16_t offset = (uint16_t)(atomic_load(&receiver.last_seq) - source_seq);
    while ((uint16_t)(65533u - (uint16_t)(source_seq + offset)) != 0) {
        uint16_t distance = (uint16_t)(65533u - (uint16_t)(source_seq + offset));
        source_seq = (uint16_t)(source_seq + (distance > 16000 ? 16000 : distance));
        ASSERT_EQ(routed_send(publisher, source_seq, 1, &orphan), RTC_OK);
        ASSERT(wait_count(&receiver.packets, ++received));
        ASSERT_EQ(atomic_load(&receiver.last_seq), (uint16_t)(source_seq + offset));
    }
    source_seq++;
    video_encoder_t encoder;
    ASSERT_EQ(video_encoder_create(&encoder, "VP8", 160, 120, 1000, 30), RTC_OK);
    video_frame_t frame;
    ASSERT_EQ(video_frame_alloc(&frame, 160, 120), RTC_OK);
    uint32_t random = 1;
    for (int y = 0; y < frame.height; y++)
        for (int x = 0; x < frame.width; x++) {
            random = random * 1664525u + 1013904223u;
            frame.planes[0][y * frame.stride[0] + x] = (uint8_t)(random >> 24);
        }
    memset(frame.planes[1], 128, 80 * 60);
    memset(frame.planes[2], 128, 80 * 60);
    rtc_vp8_payload_t payloads[64];
    for (int f = 0; f < 3; f++) {
        video_packet_t encoded;
        ASSERT_EQ(video_encoder_request_keyframe(&encoder), RTC_OK);
        ASSERT_EQ(video_encode(&encoder, &frame, &encoded), RTC_OK);
        int count;
        ASSERT_EQ(rtc_vp8_packetize(encoded.data, (size_t)encoded.len, true, payloads, 64, &count),
                  RTC_OK);
        ASSERT(count >= 3);
        uint16_t missing = (uint16_t)(source_seq + offset + 1);
        if (f == 0) {
            ASSERT_EQ(missing, 65535);
            atomic_store(&downstream.capture_seq, missing);
            atomic_store(&downstream.drop_seq, missing);
            atomic_store(&downstream.drops_remaining, 1);
        } else if (f == 1) {
            atomic_store(&upstream.drop_seq, (uint16_t)(source_seq + 1));
            atomic_store(&upstream.drops_remaining, 1);
        }
        for (int i = 0; i < count; i++) {
            int index = f == 2 && i == 1 ? 2 : f == 2 && i == 2 ? 1 : i;
            ASSERT_EQ(routed_send(publisher, (uint16_t)(source_seq + index),
                                  (uint32_t)(f + 1) * 3000, &payloads[index]),
                      RTC_OK);
        }
        received += count - (f < 2 ? 1 : 0);
        ASSERT(wait_count(&receiver.packets, received));
        ASSERT_EQ(atomic_load(&receiver.frames), f == 2 ? 2 : f);
        if (f < 2) {
            int feedback = atomic_load(&receiver.feedback);
            ASSERT_EQ(routed_nack(subscriber, params.ssrc, missing), RTC_OK);
            ASSERT(wait_count(&receiver.feedback, feedback + 1));
            if (f == 0) {
                ASSERT(wait_count(&receiver.frames, 1));
                ASSERT(wait_count(&receiver.packets, ++received));
                ASSERT_EQ(atomic_load(&downstream.captured), 2);
                ASSERT(!atomic_load(&downstream.wire_mismatch));
            } else {
                ASSERT(wait_count(&receiver.plis, 1));
                ASSERT_EQ(atomic_load(&receiver.frames), 1);
            }
        }
        source_seq = (uint16_t)(source_seq + count);
    }
    ASSERT_EQ(atomic_load(&upstream.dropped), 1);
    ASSERT_EQ(atomic_load(&downstream.dropped), 1);

    uint16_t evicted = (uint16_t)(source_seq + offset);
    for (int i = 0; i <= RTC_CONSUMER_HISTORY_SIZE; i++) {
        ASSERT_EQ(routed_send(publisher, source_seq++, 10000u + (uint32_t)i, &orphan), RTC_OK);
        ASSERT(wait_count(&receiver.packets, ++received));
    }
    uint16_t latest = (uint16_t)(source_seq + offset - 1);
    rtc_consumer_stats_t before, after;
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &before), RTC_OK);
    int feedback = atomic_load(&receiver.feedback);
    ASSERT_EQ(routed_nack(subscriber, params.ssrc, evicted), RTC_OK);
    ASSERT(wait_count(&receiver.feedback, ++feedback));
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &after), RTC_OK);
    ASSERT_EQ(after.packets_retransmitted, before.packets_retransmitted);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(routed_nack(subscriber, params.ssrc, latest), RTC_OK);
    ASSERT(wait_count(&receiver.feedback, feedback + 10));
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &after), RTC_OK);
    ASSERT_EQ(after.packets_retransmitted - before.packets_retransmitted,
              RTC_CONSUMER_MAX_RETRANSMITS);
    ASSERT_EQ(atomic_load(&receiver.feedback_errors), 0);

    rtc_consumer_pause(consumer);
    rtc_producer_stats_t producer_before, producer_after;
    ASSERT_EQ(rtc_producer_get_stats(producer, &producer_before), RTC_OK);
    ASSERT_EQ(routed_send(publisher, source_seq++, 19000, &orphan), RTC_OK);
    uint64_t deadline = rtc_time_ms() + 3000;
    do {
        ASSERT_EQ(rtc_producer_get_stats(producer, &producer_after), RTC_OK);
        if (producer_after.packets_received > producer_before.packets_received)
            break;
        pause_ms(1);
    } while (rtc_time_ms() < deadline);
    ASSERT(producer_after.packets_received > producer_before.packets_received);
    ASSERT_EQ(rtc_transport_invoke(sfu.transport, routed_barrier, NULL), RTC_OK);
    rtc_consumer_resume(consumer);
    ASSERT_EQ(routed_send(publisher, source_seq++, 19001, &orphan), RTC_OK);
    ASSERT(wait_count(&receiver.packets, ++received));
    ASSERT_EQ(atomic_load(&receiver.last_seq), (uint16_t)(latest + 2));
    ASSERT_EQ(rtc_consumer_get_stats(consumer, &before), RTC_OK);

    rtc_transport_close(outbound);
    uint16_t failed_seq = (uint16_t)(source_seq + offset);
    ASSERT_EQ(routed_send(publisher, source_seq++, 20000, &orphan), RTC_OK);
    deadline = rtc_time_ms() + 3000;
    do {
        ASSERT_EQ(rtc_consumer_get_stats(consumer, &after), RTC_OK);
        if (after.packets_dropped > before.packets_dropped)
            break;
        pause_ms(1);
    } while (rtc_time_ms() < deadline);
    ASSERT(after.packets_dropped > before.packets_dropped);
    ASSERT_EQ(after.packets_sent, before.packets_sent);
    routed_feedback_args_t failed_nack = {.receiver = &receiver};
    ASSERT_EQ(rtc_rtcp_build_nack(failed_nack.data, sizeof(failed_nack.data), &failed_nack.len, 1,
                                  params.ssrc, &failed_seq, 1),
              RTC_OK);
    /* A failed original send must be a cache miss, not a plaintext entry
     * that would attempt to protect/send again on the closed transport. */
    ASSERT_EQ(rtc_transport_invoke(sfu.transport, routed_inject_feedback, &failed_nack), RTC_OK);
    ASSERT_EQ(atomic_load(&receiver.feedback_errors), 0);

    rtc_transport_on_rtcp(outbound, NULL, NULL);
    rtc_consumer_destroy(consumer);
    rtc_producer_destroy(producer);
    atomic_store(&upstream.running, false);
    atomic_store(&downstream.running, false);
    ASSERT_EQ(rtc_thread_join(&upstream_thread), RTC_OK);
    ASSERT_EQ(rtc_thread_join(&downstream_thread), RTC_OK);
    ASSERT_EQ(atomic_load(&upstream.errors), 0);
    ASSERT_EQ(atomic_load(&downstream.errors), 0);
    rtc_close_socket(upstream.socket);
    rtc_close_socket(downstream.socket);
    rtc_transport_destroy(publisher);
    rtc_transport_destroy(subscriber);
    rtc_transport_destroy(outbound);
    close_env(&sfu);
    rtc_listener_destroy(publisher_listener);
    rtc_listener_destroy(subscriber_listener);
    rtc_worker_destroy(client_worker);
    const video_recv_stats_t *stats = media_pipeline_get_recv_stats(receiver.pipeline, 0);
    ASSERT_EQ(stats->frames_decode_failed, 0);
    ASSERT_EQ(stats->frames_decoded, 2);
    video_frame_free(&frame);
    video_encoder_destroy(&encoder);
    media_pipeline_destroy(receiver.pipeline);
}
#endif

int main(void) {
    rtc_init();
    RUN_TEST(producer_create_stats);
    RUN_TEST(consumer_create_pause_resume);
    RUN_TEST(media_rejects_closed_inputs);
    RUN_TEST(consumer_rejects_malformed_feedback);
#ifdef MRTC_ROUTING_MEDIA_CODEC_TEST
    RUN_TEST(routed_loss_wrap_and_bounded_retransmission);
#endif
    rtc_cleanup();
    TEST_SUMMARY();
}