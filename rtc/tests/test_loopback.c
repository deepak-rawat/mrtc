/*
 * End-to-end RTC loopback test over real UDP transports.
 */
#include <rtc/rtc.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_media_session.h>
#include <rtc/rtc_rtcp.h>
#include <rtc/rtc_rtp_stream.h>
#include <rtc/rtc_transport.h>
#include <rtc/rtc_worker.h>

#include "test_harness.h"

#include <string.h>

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
    rtc_media_session_t media;
    bool media_ready;
    rtc_rtp_send_stream_t *send_stream;
    rtc_rtp_recv_stream_t *recv_stream;
} loop_endpoint_t;

typedef struct {
    rtc_mutex_t mutex;
    rtc_cond_t cond;
    int count;
    uint8_t payload[256];
    size_t payload_len;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
    bool marker;
} frame_rec_t;

typedef struct {
    rtc_mutex_t mutex;
    rtc_cond_t cond;
    int nack_calls;
    int last_nack_count;
    uint16_t last_nack_seq;
    int pli_calls;
} feedback_rec_t;

typedef struct {
    rtc_mutex_t mutex;
    rtc_cond_t cond;
    int count;
    uint8_t data[128];
    size_t len;
} data_rec_t;

static bool frame_rec_init(frame_rec_t *rec) {
    memset(rec, 0, sizeof(*rec));
    return rtc_mutex_init(&rec->mutex) == RTC_OK && rtc_cond_init(&rec->cond) == RTC_OK;
}

static void frame_rec_destroy(frame_rec_t *rec) {
    rtc_cond_destroy(&rec->cond);
    rtc_mutex_destroy(&rec->mutex);
}

static void frame_callback(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp,
                           uint32_t ssrc, bool marker, void *user) {
    frame_rec_t *rec = (frame_rec_t *)user;
    rtc_mutex_lock(&rec->mutex);
    rec->count++;
    rec->payload_len = len;
    if (rec->payload_len > sizeof(rec->payload))
        rec->payload_len = sizeof(rec->payload);
    memcpy(rec->payload, payload, rec->payload_len);
    rec->seq = seq;
    rec->timestamp = timestamp;
    rec->ssrc = ssrc;
    rec->marker = marker;
    rtc_cond_signal(&rec->cond);
    rtc_mutex_unlock(&rec->mutex);
}

static bool wait_for_frame(frame_rec_t *rec, int target, int timeout_ms) {
    rtc_mutex_lock(&rec->mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rec->count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&rec->mutex);
            return false;
        }
        rtc_cond_wait_timeout(&rec->cond, &rec->mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&rec->mutex);
    return true;
}

static bool feedback_rec_init(feedback_rec_t *rec) {
    memset(rec, 0, sizeof(*rec));
    return rtc_mutex_init(&rec->mutex) == RTC_OK && rtc_cond_init(&rec->cond) == RTC_OK;
}

static void feedback_rec_destroy(feedback_rec_t *rec) {
    rtc_cond_destroy(&rec->cond);
    rtc_mutex_destroy(&rec->mutex);
}

static void nack_callback(const uint16_t *lost_seqs, int count, void *user) {
    feedback_rec_t *rec = (feedback_rec_t *)user;
    rtc_mutex_lock(&rec->mutex);
    rec->nack_calls++;
    rec->last_nack_count = count;
    rec->last_nack_seq = count > 0 ? lost_seqs[0] : 0;
    rtc_cond_signal(&rec->cond);
    rtc_mutex_unlock(&rec->mutex);
}

static void pli_callback(void *user) {
    feedback_rec_t *rec = (feedback_rec_t *)user;
    rtc_mutex_lock(&rec->mutex);
    rec->pli_calls++;
    rtc_cond_signal(&rec->cond);
    rtc_mutex_unlock(&rec->mutex);
}

static bool wait_for_feedback(feedback_rec_t *rec, int nack_target, int pli_target,
                              int timeout_ms) {
    rtc_mutex_lock(&rec->mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rec->nack_calls < nack_target || rec->pli_calls < pli_target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&rec->mutex);
            return false;
        }
        rtc_cond_wait_timeout(&rec->cond, &rec->mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&rec->mutex);
    return true;
}

static bool data_rec_init(data_rec_t *rec) {
    memset(rec, 0, sizeof(*rec));
    return rtc_mutex_init(&rec->mutex) == RTC_OK && rtc_cond_init(&rec->cond) == RTC_OK;
}

static void data_rec_destroy(data_rec_t *rec) {
    rtc_cond_destroy(&rec->cond);
    rtc_mutex_destroy(&rec->mutex);
}

static void data_callback(const uint8_t *data, size_t len, void *user) {
    data_rec_t *rec = (data_rec_t *)user;
    rtc_mutex_lock(&rec->mutex);
    rec->count++;
    rec->len = len;
    if (rec->len > sizeof(rec->data))
        rec->len = sizeof(rec->data);
    memcpy(rec->data, data, rec->len);
    rtc_cond_signal(&rec->cond);
    rtc_mutex_unlock(&rec->mutex);
}

static bool wait_for_data(data_rec_t *rec, int target, int timeout_ms) {
    rtc_mutex_lock(&rec->mutex);
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rec->count < target) {
        uint64_t now = rtc_time_ms();
        if (now >= deadline) {
            rtc_mutex_unlock(&rec->mutex);
            return false;
        }
        rtc_cond_wait_timeout(&rec->cond, &rec->mutex, (uint32_t)(deadline - now));
    }
    rtc_mutex_unlock(&rec->mutex);
    return true;
}

static bool make_endpoint(loop_endpoint_t *endpoint, rtc_ice_mode_t mode) {
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->worker = rtc_worker_create(NULL);
    if (!endpoint->worker)
        return false;
    endpoint->listener = rtc_listener_create(endpoint->worker, NULL);
    if (!endpoint->listener)
        return false;
    endpoint->transport = rtc_transport_create(endpoint->worker, &(rtc_transport_config_t){
                                                                     .listener = endpoint->listener,
                                                                     .ice_mode = mode,
                                                                 });
    return endpoint->transport != NULL;
}

static void close_endpoint(loop_endpoint_t *endpoint) {
    if (endpoint->media_ready)
        rtc_media_session_stop(&endpoint->media);
    if (endpoint->transport)
        rtc_transport_destroy(endpoint->transport);
    if (endpoint->media_ready)
        rtc_media_session_close(&endpoint->media);
    if (endpoint->send_stream)
        rtc_rtp_send_stream_destroy(endpoint->send_stream);
    if (endpoint->recv_stream)
        rtc_rtp_recv_stream_destroy(endpoint->recv_stream);
    if (endpoint->listener)
        rtc_listener_destroy(endpoint->listener);
    if (endpoint->worker)
        rtc_worker_destroy(endpoint->worker);
}

static bool listener_loopback_candidate(rtc_listener_t *listener, rtc_transport_candidate_t *out) {
    rtc_addr_t local;
    if (rtc_listener_get_local_addr(listener, &local) != RTC_OK)
        return false;
    uint16_t port = ntohs(((struct sockaddr_in *)&local.addr)->sin_port);
    memset(out, 0, sizeof(*out));
    memcpy(out->foundation, "H0", sizeof("H0"));
    memcpy(out->address, "127.0.0.1", sizeof("127.0.0.1"));
    memcpy(out->protocol, "udp", sizeof("udp"));
    out->port = port;
    out->type = RTC_TRANSPORT_CANDIDATE_HOST;
    return true;
}

static bool wait_for_selected(rtc_transport_t *transport, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_transport_stats_t stats;
        if (rtc_transport_get_stats(transport, &stats) == RTC_OK && stats.selected_tuple_valid)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool wait_for_dtls_connected(rtc_transport_t *transport, int timeout_ms) {
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

static bool wait_for_listener_sent(rtc_listener_t *listener, uint64_t target, int timeout_ms) {
    uint64_t deadline = rtc_time_ms() + (uint64_t)timeout_ms;
    while (rtc_time_ms() < deadline) {
        rtc_listener_stats_t stats;
        if (rtc_listener_get_stats(listener, &stats) == RTC_OK && stats.packets_sent >= target)
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static bool connect_endpoints(loop_endpoint_t *full, loop_endpoint_t *lite) {
    rtc_dtls_parameters_t full_dtls;
    rtc_dtls_parameters_t lite_dtls;
    if (rtc_transport_get_dtls_parameters(full->transport, &full_dtls) != RTC_OK ||
        rtc_transport_get_dtls_parameters(lite->transport, &lite_dtls) != RTC_OK)
        return false;
    if (full_dtls.role != RTC_TRANSPORT_DTLS_ROLE_CLIENT ||
        lite_dtls.role != RTC_TRANSPORT_DTLS_ROLE_SERVER)
        return false;

    rtc_ice_parameters_t full_ice;
    rtc_ice_parameters_t lite_ice;
    if (rtc_transport_get_ice_parameters(full->transport, &full_ice) != RTC_OK ||
        rtc_transport_get_ice_parameters(lite->transport, &lite_ice) != RTC_OK)
        return false;
    if (full_ice.username_fragment[0] == '\0' || full_ice.password[0] == '\0' ||
        lite_ice.username_fragment[0] == '\0' || lite_ice.password[0] == '\0')
        return false;

    rtc_transport_candidate_t lite_candidate;
    if (!listener_loopback_candidate(lite->listener, &lite_candidate))
        return false;
    if (rtc_transport_set_remote_ice_parameters(full->transport, &lite_ice) != RTC_OK ||
        rtc_transport_add_remote_candidate(full->transport, &lite_candidate) != RTC_OK ||
        rtc_transport_start_ice(full->transport) != RTC_OK)
        return false;

    if (!wait_for_selected(full->transport, 3000) || !wait_for_selected(lite->transport, 3000))
        return false;
    if (rtc_transport_start_dtls(full->transport) != RTC_OK)
        return false;
    return wait_for_dtls_connected(full->transport, 5000) &&
           wait_for_dtls_connected(lite->transport, 5000);
}

static bool setup_media(loop_endpoint_t *endpoint, uint8_t send_pt, uint8_t recv_pt,
                        uint32_t recv_local_ssrc, frame_rec_t *frames, feedback_rec_t *feedback) {
    memset(&endpoint->media, 0, sizeof(endpoint->media));
    if (rtc_media_session_init(&endpoint->media, endpoint->transport, endpoint->worker) != RTC_OK)
        return false;
    endpoint->media_ready = true;

    endpoint->send_stream = rtc_rtp_send_stream_create(&(rtc_rtp_send_stream_config_t){
        .payload_type = send_pt,
        .clock_rate = 90000,
    });
    endpoint->recv_stream = rtc_rtp_recv_stream_create(&(rtc_rtp_recv_stream_config_t){
        .payload_type = recv_pt,
        .clock_rate = 90000,
        .local_ssrc = recv_local_ssrc,
    });
    if (!endpoint->send_stream || !endpoint->recv_stream)
        return false;

    rtc_rtp_send_stream_attach_transport(endpoint->send_stream, endpoint->transport);
    rtc_rtp_send_stream_arm_video(endpoint->send_stream);
    if (feedback) {
        rtc_rtp_send_stream_on_nack(endpoint->send_stream, nack_callback, feedback);
        rtc_rtp_send_stream_on_pli(endpoint->send_stream, pli_callback, feedback);
    }
    rtc_rtp_recv_stream_set_active(endpoint->recv_stream, true);
    rtc_rtp_recv_stream_on_frame(endpoint->recv_stream, frame_callback, frames);

    return rtc_media_session_add_sender(&endpoint->media, endpoint->send_stream) == RTC_OK &&
           rtc_media_session_add_receiver(&endpoint->media, endpoint->recv_stream) == RTC_OK;
}

TEST(loopback_full_to_lite_media_feedback_and_data) {
    frame_rec_t full_frames;
    frame_rec_t lite_frames;
    feedback_rec_t full_feedback;
    data_rec_t full_data;
    data_rec_t lite_data;
    ASSERT(frame_rec_init(&full_frames));
    ASSERT(frame_rec_init(&lite_frames));
    ASSERT(feedback_rec_init(&full_feedback));
    ASSERT(data_rec_init(&full_data));
    ASSERT(data_rec_init(&lite_data));

    loop_endpoint_t full;
    loop_endpoint_t lite;
    ASSERT(make_endpoint(&full, RTC_ICE_MODE_FULL));
    ASSERT(make_endpoint(&lite, RTC_ICE_MODE_LITE));
    ASSERT(connect_endpoints(&full, &lite));

    ASSERT(setup_media(&full, 96, 97, 0xF0F0F0F0u, &full_frames, &full_feedback));
    ASSERT(setup_media(&lite, 97, 96, 0xB0B0B0B0u, &lite_frames, NULL));

    const uint8_t payload_full_to_lite[] = {0xFE, 0xED, 0xFA, 0xCE};
    ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload_full_to_lite,
                                       sizeof(payload_full_to_lite), 3000, true),
              RTC_OK);
    ASSERT(wait_for_frame(&lite_frames, 1, 1000));
    uint16_t first_full_seq = lite_frames.seq;
    ASSERT_EQ(lite_frames.payload_len, sizeof(payload_full_to_lite));
    ASSERT_MEM_EQ(lite_frames.payload, payload_full_to_lite, sizeof(payload_full_to_lite));
    ASSERT(lite_frames.marker);
    ASSERT_EQ(lite_frames.ssrc, rtc_rtp_send_stream_ssrc(full.send_stream));
    ASSERT_EQ(rtc_rtp_recv_stream_ssrc(lite.recv_stream),
              rtc_rtp_send_stream_ssrc(full.send_stream));

    const uint8_t payload_second[] = {0x10, 0x20, 0x30};
    ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload_second, sizeof(payload_second),
                                       3000, false),
              RTC_OK);
    ASSERT(wait_for_frame(&lite_frames, 2, 1000));
    ASSERT_EQ(lite_frames.payload_len, sizeof(payload_second));
    ASSERT_MEM_EQ(lite_frames.payload, payload_second, sizeof(payload_second));
    ASSERT(!lite_frames.marker);

    const uint8_t payload_lite_to_full[] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    ASSERT_EQ(rtc_rtp_send_stream_send(lite.send_stream, payload_lite_to_full,
                                       sizeof(payload_lite_to_full), 3000, true),
              RTC_OK);
    ASSERT(wait_for_frame(&full_frames, 1, 1000));
    ASSERT_EQ(full_frames.payload_len, sizeof(payload_lite_to_full));
    ASSERT_MEM_EQ(full_frames.payload, payload_lite_to_full, sizeof(payload_lite_to_full));
    ASSERT_EQ(full_frames.ssrc, rtc_rtp_send_stream_ssrc(lite.send_stream));

    const rtc_rtcp_stats_t *full_send_stats = rtc_rtp_send_stream_stats(full.send_stream);
    const rtc_rtcp_stats_t *lite_recv_stats = rtc_rtp_recv_stream_stats(lite.recv_stream);
    ASSERT(full_send_stats != NULL);
    ASSERT(lite_recv_stats != NULL);
    ASSERT_EQ(full_send_stats->packets_sent, 2);
    ASSERT_EQ(lite_recv_stats->packets_received, 2);

    rtc_listener_stats_t full_listener_stats;
    ASSERT_EQ(rtc_listener_get_stats(full.listener, &full_listener_stats), RTC_OK);
    uint64_t full_packets_sent_before_feedback = full_listener_stats.packets_sent;

    uint8_t feedback_buf[256];
    size_t feedback_len = 0;
    size_t sub_len = 0;
    uint16_t lost_seqs[] = {first_full_seq};
    ASSERT_EQ(rtc_rtcp_build_nack(feedback_buf, sizeof(feedback_buf), &sub_len, 0xB0B0B0B0u,
                                  rtc_rtp_send_stream_ssrc(full.send_stream), lost_seqs, 1),
              RTC_OK);
    feedback_len += sub_len;
    ASSERT_EQ(rtc_rtcp_build_pli(feedback_buf + feedback_len, sizeof(feedback_buf) - feedback_len,
                                 &sub_len, 0xB0B0B0B0u, rtc_rtp_send_stream_ssrc(full.send_stream)),
              RTC_OK);
    feedback_len += sub_len;
    ASSERT_EQ(rtc_transport_send_rtcp(lite.transport, feedback_buf, &feedback_len,
                                      sizeof(feedback_buf)),
              RTC_OK);
    ASSERT(wait_for_feedback(&full_feedback, 1, 1, 1000));
    ASSERT_EQ(full_feedback.last_nack_count, 1);
    ASSERT_EQ(full_feedback.last_nack_seq, first_full_seq);
    ASSERT(wait_for_listener_sent(full.listener, full_packets_sent_before_feedback + 1, 1000));

    rtc_transport_on_data(full.transport, data_callback, &full_data);
    rtc_transport_on_data(lite.transport, data_callback, &lite_data);

    const uint8_t app_full_to_lite[] = {0x01, 0x02, 0x03};
    ASSERT_EQ(rtc_transport_send_data(full.transport, app_full_to_lite, sizeof(app_full_to_lite)),
              RTC_OK);
    ASSERT(wait_for_data(&lite_data, 1, 1000));
    ASSERT_EQ(lite_data.len, sizeof(app_full_to_lite));
    ASSERT_MEM_EQ(lite_data.data, app_full_to_lite, sizeof(app_full_to_lite));

    const uint8_t app_lite_to_full[] = {0x11, 0x12, 0x13, 0x14};
    ASSERT_EQ(rtc_transport_send_data(lite.transport, app_lite_to_full, sizeof(app_lite_to_full)),
              RTC_OK);
    ASSERT(wait_for_data(&full_data, 1, 1000));
    ASSERT_EQ(full_data.len, sizeof(app_lite_to_full));
    ASSERT_MEM_EQ(full_data.data, app_lite_to_full, sizeof(app_lite_to_full));

    rtc_transport_stats_t full_transport_stats;
    rtc_transport_stats_t lite_transport_stats;
    ASSERT_EQ(rtc_transport_get_stats(full.transport, &full_transport_stats), RTC_OK);
    ASSERT_EQ(rtc_transport_get_stats(lite.transport, &lite_transport_stats), RTC_OK);
    ASSERT(full_transport_stats.packets_received > 0);
    ASSERT(lite_transport_stats.packets_received > 0);
    ASSERT(full_transport_stats.dtls_packets_received > 0);
    ASSERT(lite_transport_stats.dtls_packets_received > 0);

    close_endpoint(&full);
    close_endpoint(&lite);
    data_rec_destroy(&lite_data);
    data_rec_destroy(&full_data);
    feedback_rec_destroy(&full_feedback);
    frame_rec_destroy(&lite_frames);
    frame_rec_destroy(&full_frames);
}

int main(void) {
    rtc_init();
    RUN_TEST(loopback_full_to_lite_media_feedback_and_data);
    rtc_cleanup();
    TEST_SUMMARY();
}
