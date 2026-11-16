#include "test_harness.h"
#include <media/media_pipeline.h>
#include <rtc/rtc_client.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_sdp.h>
#include <rtc/rtc_transport.h>
#include <rtc/rtc_worker.h>
#include <rtc/rtc_media_session.h>
#include <stdatomic.h>

typedef struct {
    media_pipeline_t *pipeline;
    media_recv_stream_t *stream;
    rtc_mutex_t mutex;
    rtc_cond_t cond;
    int packets;
    int frames;
    uint32_t timestamp;
    bool timestamp_mismatch;
    uint32_t timestamp_step;
    rtc_rtp_recv_stream_t *receiver;
    _Atomic rtc_connection_state_t state;
} loopback_t;

typedef struct {
    rtc_socket_t socket;
    rtc_addr_t server, local;
    _Atomic bool running, drop_next, dropped;
    _Atomic uint16_t dropped_seq;
    _Atomic int retransmitted, errors;
} loss_proxy_t;

static void pause_ms(int ms);

static void *proxy_loop(void *user) {
    loss_proxy_t *proxy = user;
    struct sockaddr_storage client = {0};
    socklen_t client_len = 0;
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
        bool from_server = ((struct sockaddr_in *)&from)->sin_port ==
                           ((struct sockaddr_in *)&proxy->server.addr)->sin_port;
        if (!from_server) {
            client = from;
            client_len = from_len;
            if (len >= 12 && (data[0] & 0xc0) == 0x80 && !(data[1] >= 192 && data[1] <= 223)) {
                uint16_t seq = (uint16_t)((uint16_t)data[2] << 8 | data[3]);
                if (atomic_exchange(&proxy->drop_next, false)) {
                    atomic_store(&proxy->dropped_seq, seq);
                    atomic_store(&proxy->dropped, true);
                    continue;
                }
                if (atomic_load(&proxy->dropped) && atomic_load(&proxy->dropped_seq) == seq)
                    atomic_fetch_add(&proxy->retransmitted, 1);
            }
        }
        if (from_server && !client_len)
            continue;
        const struct sockaddr *target =
            from_server ? (struct sockaddr *)&client : (struct sockaddr *)&proxy->server.addr;
        socklen_t target_len = from_server ? client_len : proxy->server.len;
        if (sendto(proxy->socket, (const char *)data, (size_t)len, 0, target, target_len) != len)
            atomic_fetch_add(&proxy->errors, 1);
    }
    return NULL;
}

static void on_state(rtc_connection_state_t state, void *user) {
    loopback_t *ctx = user;
    ctx->state = state;
}

static void on_video(const char *peer_id, const char *label, const video_frame_t *frame,
                     void *user) {
    (void)peer_id;
    (void)label;
    (void)frame;
    loopback_t *ctx = user;
    ctx->frames++;
}

static void on_payload(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                       uint32_t ssrc, bool marker, void *user) {
    (void)ssrc;
    loopback_t *ctx = user;
    rtc_mutex_lock(&ctx->mutex);
    if (!ctx->packets) {
        ctx->timestamp = timestamp;
    } else if ((int32_t)(timestamp - ctx->timestamp) > 0) {
        if (timestamp - ctx->timestamp != ctx->timestamp_step)
            ctx->timestamp_mismatch = true;
        ctx->timestamp = timestamp;
    }
    ctx->packets++;
    media_pipeline_recv_rtp(ctx->pipeline, ctx->stream, payload, (int)len, seq, timestamp, marker);
    rtc_cond_signal(&ctx->cond);
    rtc_mutex_unlock(&ctx->mutex);
}

static void request_keyframe(void *user) {
    loopback_t *ctx = user;
    (void)rtc_rtp_recv_stream_request_keyframe(ctx->receiver);
}

static bool make_answer(rtc_transport_t *transport, const rtc_addr_t *candidate_addr,
                        const rtc_desc_t *offer, rtc_desc_t *answer) {
    rtc_sdp_t sdp = {0};
    if (rtc_sdp_parse(&sdp, offer->sdp, offer->sdp_len) != RTC_OK)
        return false;
    rtc_ice_parameters_t ice;
    rtc_dtls_parameters_t dtls;
    bool ok = rtc_transport_get_ice_parameters(transport, &ice) == RTC_OK &&
              rtc_transport_get_dtls_parameters(transport, &dtls) == RTC_OK;
    if (ok) {
        rtc_dtls_parameters_t remote = {.role = RTC_TRANSPORT_DTLS_ROLE_CLIENT};
        snprintf(remote.fingerprint, sizeof(remote.fingerprint), "%s", sdp.fingerprint);
        ok = rtc_transport_set_remote_dtls_parameters(transport, &remote) == RTC_OK;
        rtc_ice_parameters_t remote_ice = {.mode = RTC_ICE_MODE_FULL};
        snprintf(remote_ice.username_fragment, sizeof(remote_ice.username_fragment), "%s",
                 sdp.ice_ufrag);
        snprintf(remote_ice.password, sizeof(remote_ice.password), "%s", sdp.ice_pwd);
        ok = ok && rtc_transport_set_remote_ice_parameters(transport, &remote_ice) == RTC_OK;
    }
    if (ok) {
        sdp.type = RTC_SDP_ANSWER;
        sdp.setup = RTC_SETUP_PASSIVE;
        memcpy(sdp.ice_ufrag, ice.username_fragment, sizeof(sdp.ice_ufrag));
        memcpy(sdp.ice_pwd, ice.password, sizeof(sdp.ice_pwd));
        memcpy(sdp.fingerprint, dtls.fingerprint, sizeof(sdp.fingerprint));
        rtc_vec_clear(&sdp.candidates);
        rtc_ice_candidate_t candidate = {0};
        candidate.type = ICE_CANDIDATE_HOST;
        candidate.component = 1;
        candidate.priority = 2130706431u;
        strcpy(candidate.foundation, "H0");
        uint16_t port = ntohs(((const struct sockaddr_in *)&candidate_addr->addr)->sin_port);
        ok = rtc_addr_from_string(&candidate.addr, "127.0.0.1", port) == RTC_OK &&
             rtc_sdp_add_candidate(&sdp, &candidate) == RTC_OK && rtc_sdp_generate(&sdp) == RTC_OK;
    }
    if (ok) {
        memset(answer, 0, sizeof(*answer));
        answer->type = RTC_SDP_ANSWER;
        memcpy(answer->sdp, sdp.raw, sdp.raw_len);
        answer->sdp_len = sdp.raw_len;
    }
    rtc_sdp_close(&sdp);
    return ok;
}

static void pause_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((unsigned int)ms * 1000);
#endif
}

TEST(fragmented_video_loopback) {
    loopback_t ctx = {0};
    ctx.timestamp_step = 90000 / 15;
    ASSERT_EQ(rtc_mutex_init(&ctx.mutex), RTC_OK);
    ASSERT_EQ(rtc_cond_init(&ctx.cond), RTC_OK);
    media_renderer_t renderer = {.on_video_frame = on_video, .user = &ctx};
    media_pipeline_config_t cfg = {.renderer = &renderer};
    ctx.pipeline = media_pipeline_create(&cfg);
    ASSERT(ctx.pipeline != NULL);
    ctx.stream = media_pipeline_add_recv_stream(ctx.pipeline, "sender", "camera", 1, true, "VP8");
    ASSERT(ctx.stream != NULL);

    rtc_worker_t *worker = rtc_worker_create(NULL);
    ASSERT(worker != NULL);
    rtc_listener_t *listener = rtc_listener_create(worker, NULL);
    ASSERT(listener != NULL);
    loss_proxy_t proxy = {0};
    ASSERT_EQ(rtc_listener_get_local_addr(listener, &proxy.server), RTC_OK);
    uint16_t server_port = ntohs(((struct sockaddr_in *)&proxy.server.addr)->sin_port);
    ASSERT_EQ(rtc_addr_from_string(&proxy.server, "127.0.0.1", server_port), RTC_OK);
    ASSERT_EQ(rtc_addr_from_string(&proxy.local, "127.0.0.1", 0), RTC_OK);
    proxy.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT(proxy.socket != RTC_INVALID_SOCKET);
    ASSERT_EQ(bind(proxy.socket, (struct sockaddr *)&proxy.local.addr, proxy.local.len), 0);
    ASSERT_EQ(getsockname(proxy.socket, (struct sockaddr *)&proxy.local.addr, &proxy.local.len), 0);
    ASSERT_EQ(rtc_set_nonblocking(proxy.socket), RTC_OK);
    atomic_store(&proxy.running, true);
    rtc_thread_t proxy_thread;
    ASSERT_EQ(rtc_thread_create(&proxy_thread, proxy_loop, &proxy), RTC_OK);
    rtc_transport_t *transport = rtc_transport_create(
        worker, &(rtc_transport_config_t){.listener = listener, .ice_mode = RTC_ICE_MODE_LITE});
    ASSERT(transport != NULL);
    rtc_media_session_t session = {0};
    ASSERT_EQ(rtc_media_session_init(&session, transport, worker), RTC_OK);
    ctx.receiver = rtc_rtp_recv_stream_create(&(
        rtc_rtp_recv_stream_config_t){.payload_type = 96, .clock_rate = 90000, .local_ssrc = 100});
    ASSERT(ctx.receiver != NULL);
    rtc_rtp_recv_stream_set_active(ctx.receiver, true);
    rtc_rtp_recv_stream_on_frame(ctx.receiver, on_payload, &ctx);
    ASSERT_EQ(rtc_media_session_add_receiver(&session, ctx.receiver), RTC_OK);
    ASSERT_EQ(media_pipeline_set_recv_feedback(ctx.pipeline, ctx.stream, request_keyframe, &ctx),
              RTC_OK);

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&(rtc_config_t){0});
    ASSERT(pc != NULL);
    rtc_peer_connection_on_connection_state(pc, on_state, &ctx);
    rtc_codec_t codec = {.payload_type = 96, .clock_rate = 90000};
    strcpy(codec.mime_type, "video/VP8");
    rtc_rtp_sender_t *sender = rtc_peer_connection_add_track(pc, RTC_KIND_VIDEO, &codec);
    ASSERT(sender != NULL);
    rtc_desc_t offer, answer;
    ASSERT_EQ(rtc_peer_connection_create_offer(pc, &offer), RTC_OK);
    ASSERT_EQ(rtc_peer_connection_set_local_desc(pc, &offer), RTC_OK);
    ASSERT(make_answer(transport, &proxy.local, &offer, &answer));
    ASSERT_EQ(rtc_peer_connection_set_remote_desc(pc, &answer), RTC_OK);
    bool connected = false;
    for (int i = 0; i < 1000; i++) {
        rtc_transport_stats_t stats;
        if (ctx.state == RTC_CONNECTION_CONNECTED &&
            rtc_transport_get_stats(transport, &stats) == RTC_OK && stats.srtp_ready) {
            connected = true;
            break;
        }
        pause_ms(10);
    }
    ASSERT(connected);
    rtc_media_session_start(&session);

    media_pipeline_t *send = media_pipeline_create(&(media_pipeline_config_t){0});
    ASSERT(send != NULL);
    media_send_stream_t *camera =
        media_pipeline_add_send_stream(send, "camera", true, "VP8", 640, 480, 15, 500);
    ASSERT(camera != NULL);
    ASSERT_EQ(media_pipeline_add_send_peer(send, "receiver", sender, NULL), RTC_OK);
    ASSERT_EQ(media_pipeline_set_peer_bitrate(send, "receiver", 500000), RTC_OK);
    video_frame_t input;
    ASSERT_EQ(video_frame_alloc(&input, 640, 480), RTC_OK);
    memset(input.planes[1], 128, 320 * 240);
    memset(input.planes[2], 128, 320 * 240);
    for (int frame = 0; frame < 12; frame++) {
        if (frame == 2) {
            media_pipeline_remove_send_peer(send, "receiver");
            ASSERT_EQ(media_pipeline_add_send_peer(send, "receiver", sender, NULL), RTC_OK);
            atomic_store(&proxy.drop_next, true);
        }
        for (int y = 0; y < input.height; y++)
            for (int x = 0; x < input.width; x++)
                input.planes[0][y * input.stride[0] + x] = (uint8_t)(x + y + frame);
        ASSERT_EQ(media_pipeline_push_video(send, camera, &input), RTC_OK);
        rtc_mutex_lock(&ctx.mutex);
        uint64_t deadline = rtc_time_ms() + 2000;
        while (ctx.frames <= frame && rtc_time_ms() < deadline)
            rtc_cond_wait_timeout(&ctx.cond, &ctx.mutex, 20);
        int frames = ctx.frames;
        rtc_mutex_unlock(&ctx.mutex);
        ASSERT_EQ(frames, frame + 1);
        if (frame == 5) {
            media_pipeline_remove_send_peer(send, "receiver");
            ASSERT_EQ(media_pipeline_add_send_peer(send, "new-receiver", sender, NULL), RTC_OK);
        }
    }
    ASSERT(!ctx.timestamp_mismatch);
    printf("    received %d packets / %d frames, retransmitted=%d\n", ctx.packets, ctx.frames,
           atomic_load(&proxy.retransmitted));
    ASSERT(ctx.packets > ctx.frames);
    ASSERT(atomic_load(&proxy.retransmitted) >= 1);
    ASSERT_EQ(atomic_load(&proxy.errors), 0);
    ASSERT_EQ(media_pipeline_get_recv_stats(ctx.pipeline, 0)->frames_decode_failed, 0);
    ASSERT(media_pipeline_get_send_stats(send, 0)->keyframes_encoded >= 2);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
    rtc_media_session_stop(&session);
    rtc_transport_destroy(transport);
    rtc_media_session_close(&session);
    rtc_rtp_recv_stream_destroy(ctx.receiver);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
    atomic_store(&proxy.running, false);
    ASSERT_EQ(rtc_thread_join(&proxy_thread), RTC_OK);
    rtc_close_socket(proxy.socket);
    video_frame_free(&input);
    media_pipeline_destroy(send);
    media_pipeline_destroy(ctx.pipeline);
    rtc_cond_destroy(&ctx.cond);
    rtc_mutex_destroy(&ctx.mutex);
}

int main(void) {
    rtc_client_init();
    RUN_TEST(fragmented_video_loopback);
    rtc_client_cleanup();
    TEST_SUMMARY();
}
