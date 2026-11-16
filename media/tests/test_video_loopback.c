#include "test_harness.h"
#include <media/media_pipeline.h>
#include <rtc/rtc_client.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_sdp.h>
#include <rtc/rtc_transport.h>
#include <rtc/rtc_worker.h>
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
    _Atomic rtc_connection_state_t state;
} loopback_t;

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

static void on_rtp(const rtc_rtp_packet_t *pkt, void *user) {
    loopback_t *ctx = user;
    rtc_mutex_lock(&ctx->mutex);
    if (pkt->payload[0] & 0x10)
        ctx->timestamp = pkt->header.timestamp;
    else if (pkt->header.timestamp != ctx->timestamp)
        ctx->timestamp_mismatch = true;
    ctx->packets++;
    media_pipeline_recv_rtp(ctx->pipeline, ctx->stream, pkt->payload, (int)pkt->payload_len,
                            pkt->header.seq, pkt->header.timestamp, pkt->header.marker);
    rtc_cond_signal(&ctx->cond);
    rtc_mutex_unlock(&ctx->mutex);
}

static bool make_answer(rtc_transport_t *transport, rtc_listener_t *listener,
                        const rtc_desc_t *offer, rtc_desc_t *answer) {
    rtc_sdp_t sdp = {0};
    if (rtc_sdp_parse(&sdp, offer->sdp, offer->sdp_len) != RTC_OK)
        return false;
    rtc_ice_parameters_t ice;
    rtc_dtls_parameters_t dtls;
    rtc_addr_t addr;
    bool ok = rtc_transport_get_ice_parameters(transport, &ice) == RTC_OK &&
              rtc_transport_get_dtls_parameters(transport, &dtls) == RTC_OK &&
              rtc_listener_get_local_addr(listener, &addr) == RTC_OK;
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
        uint16_t port = ntohs(((struct sockaddr_in *)&addr.addr)->sin_port);
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
    rtc_transport_t *transport = rtc_transport_create(
        worker, &(rtc_transport_config_t){.listener = listener, .ice_mode = RTC_ICE_MODE_LITE});
    ASSERT(transport != NULL);
    rtc_transport_on_rtp(transport, on_rtp, &ctx);

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
    ASSERT(make_answer(transport, listener, &offer, &answer));
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

    media_pipeline_t *send = media_pipeline_create(&(media_pipeline_config_t){0});
    ASSERT(send != NULL);
    media_send_stream_t *camera =
        media_pipeline_add_send_stream(send, "camera", true, "VP8", 640, 480, 30, 500);
    ASSERT(camera != NULL);
    ASSERT_EQ(media_pipeline_add_send_peer(send, "receiver", sender, NULL), RTC_OK);
    video_frame_t input;
    ASSERT_EQ(video_frame_alloc(&input, 640, 480), RTC_OK);
    memset(input.planes[1], 128, 320 * 240);
    memset(input.planes[2], 128, 320 * 240);
    for (int frame = 0; frame < 12; frame++) {
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
    ASSERT(ctx.packets > ctx.frames);
    ASSERT_EQ(media_pipeline_get_recv_stats(ctx.pipeline, 0)->frames_decode_failed, 0);
    ASSERT(media_pipeline_get_send_stats(send, 0)->keyframes_encoded >= 2);

    rtc_peer_connection_close(pc);
    rtc_peer_connection_destroy(pc);
    rtc_transport_destroy(transport);
    rtc_listener_destroy(listener);
    rtc_worker_destroy(worker);
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
