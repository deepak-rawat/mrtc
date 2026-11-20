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
#include "rtc_transport_internal.h"

#include <string.h>
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

static bool make_endpoint_ex(loop_endpoint_t *endpoint, rtc_ice_mode_t mode, bool paced) {
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
                                                                     .enable_rtp_pacing = paced,
                                                                 });
    return endpoint->transport != NULL;
}

static bool make_endpoint(loop_endpoint_t *endpoint, rtc_ice_mode_t mode) {
    return make_endpoint_ex(endpoint, mode, false);
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
    if (rtc_transport_set_remote_dtls_parameters(full->transport, &lite_dtls) != RTC_OK ||
        rtc_transport_set_remote_dtls_parameters(lite->transport, &full_dtls) != RTC_OK)
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
        rtc_transport_set_remote_ice_parameters(lite->transport, &full_ice) != RTC_OK ||
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

static bool setup_media_with_ssrc(loop_endpoint_t *endpoint, uint8_t send_pt, uint8_t recv_pt,
                                  uint32_t recv_local_ssrc, uint32_t send_ssrc, frame_rec_t *frames,
                                  feedback_rec_t *feedback) {
    memset(&endpoint->media, 0, sizeof(endpoint->media));
    if (rtc_media_session_init(&endpoint->media, endpoint->transport, endpoint->worker) != RTC_OK)
        return false;
    endpoint->media_ready = true;

    endpoint->send_stream = rtc_rtp_send_stream_create(&(rtc_rtp_send_stream_config_t){
        .payload_type = send_pt,
        .clock_rate = 90000,
        .ssrc = send_ssrc,
    });
    endpoint->recv_stream = rtc_rtp_recv_stream_create(&(rtc_rtp_recv_stream_config_t){
        .payload_type = recv_pt,
        .clock_rate = 90000,
        .local_ssrc =
            recv_local_ssrc ? recv_local_ssrc : rtc_rtp_send_stream_ssrc(endpoint->send_stream),
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

static bool setup_media(loop_endpoint_t *endpoint, uint8_t send_pt, uint8_t recv_pt,
                        uint32_t recv_local_ssrc, frame_rec_t *frames, feedback_rec_t *feedback) {
    return setup_media_with_ssrc(endpoint, send_pt, recv_pt, recv_local_ssrc, 0, frames, feedback);
}

TEST(loopback_collision_rotates_only_the_independent_local_reporter) {
    frame_rec_t full_frames, lite_frames;
    ASSERT(frame_rec_init(&full_frames));
    ASSERT(frame_rec_init(&lite_frames));
    loop_endpoint_t full, lite;
    ASSERT(make_endpoint(&full, RTC_ICE_MODE_FULL));
    ASSERT(make_endpoint(&lite, RTC_ICE_MODE_LITE));
    const uint32_t collision = 0x11223344;
    ASSERT(setup_media_with_ssrc(&full, 96, 97, 0, collision, &full_frames, NULL));
    ASSERT(setup_media(&lite, 97, 96, collision, &lite_frames, NULL));
    uint32_t independent_sender = rtc_rtp_send_stream_ssrc(lite.send_stream);
    ASSERT(connect_endpoints(&full, &lite));
    const uint8_t payload[] = {0x10, 0};
    ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload, sizeof(payload), 3000, true),
              RTC_OK);
    ASSERT(wait_for_frame(&lite_frames, 1, 1000));
    ASSERT_EQ(rtc_rtp_send_stream_ssrc(full.send_stream), collision);
    ASSERT_EQ(rtc_rtp_send_stream_generation(full.send_stream), 1);
    ASSERT_EQ(rtc_rtp_send_stream_ssrc(lite.send_stream), independent_sender);
    const rtc_rtcp_stats_t *receiver = rtc_rtp_recv_stream_stats(lite.recv_stream);
    ASSERT(receiver->ssrc && receiver->ssrc != collision);
    ASSERT(receiver->ssrc != independent_sender);
    ASSERT_EQ(receiver->remote_ssrc, collision);
    ASSERT_EQ(rtc_rtp_recv_stream_ssrc(lite.recv_stream), collision);
    ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload, sizeof(payload), 3000, true),
              RTC_OK);
    ASSERT(wait_for_frame(&lite_frames, 2, 1000));
    ASSERT_EQ(rtc_rtp_send_stream_send(lite.send_stream, payload, sizeof(payload), 3000, true),
              RTC_OK);
    ASSERT(wait_for_frame(&full_frames, 1, 1000));
    close_endpoint(&full);
    close_endpoint(&lite);
    frame_rec_destroy(&full_frames);
    frame_rec_destroy(&lite_frames);
}

TEST(loopback_collision_cancels_staged_ciphertext_and_old_repair) {
    frame_rec_t full_frames, lite_frames;
    ASSERT(frame_rec_init(&full_frames));
    ASSERT(frame_rec_init(&lite_frames));
    loop_endpoint_t full, lite;
    ASSERT(make_endpoint_ex(&full, RTC_ICE_MODE_FULL, true));
    ASSERT(make_endpoint(&lite, RTC_ICE_MODE_LITE));
    const uint32_t previous = 0x11223344;
    ASSERT(setup_media_with_ssrc(&full, 96, 97, 0, previous, &full_frames, NULL));
    ASSERT(setup_media(&lite, 97, 96, 0, &lite_frames, NULL));
    ASSERT(connect_endpoints(&full, &lite));
    uint8_t payload[1000] = {0x10};
    rtc_rtp_packet_t staged, incoming;
    ASSERT_EQ(rtc_rtp_build(&staged, 96, 123, 3000, previous, false, payload, sizeof(payload)),
              RTC_OK);
    size_t wire_len = staged.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp_media(full.transport, staged.buf, &wire_len,
                                           sizeof(staged.buf), true, 1000),
              RTC_OK);
    rtc_transport_stats_t transport;
    ASSERT_EQ(rtc_transport_get_stats(full.transport, &transport), RTC_OK);
    ASSERT(transport.rtp_packets_queued > 0);
    ASSERT_EQ(rtc_rtp_build(&incoming, 97, 456, 3000, previous, true, payload, 2), RTC_OK);
    size_t incoming_len = incoming.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(lite.transport, incoming.buf, &incoming_len,
                                     sizeof(incoming.buf)),
              RTC_OK);
    ASSERT(wait_for_frame(&full_frames, 1, 1000));
    ASSERT(rtc_rtp_send_stream_ssrc(full.send_stream) != previous);
    ASSERT_EQ(rtc_rtp_send_stream_generation(full.send_stream), 2);
    ASSERT_EQ(rtc_transport_get_stats(full.transport, &transport), RTC_OK);
    ASSERT_EQ(transport.rtp_packets_queued, 0);
    rtc_pacer_sent_t sent;
    ASSERT(!rtc_transport_rtp_sent(full.transport, previous, &sent));
    ASSERT_EQ(rtc_transport_retransmit_rtp(full.transport, staged.buf, wire_len), RTC_ERR_TIMEOUT);
    ASSERT_EQ(rtc_rtp_build(&staged, 96, 124, 3000, previous, true, payload, 2), RTC_OK);
    wire_len = staged.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(full.transport, staged.buf, &wire_len, sizeof(staged.buf)),
              RTC_ERR_INVALID);
    ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload, 2, 3000, true), RTC_OK);
    ASSERT(wait_for_frame(&lite_frames, 1, 1000));
    ASSERT_EQ(lite_frames.payload_len, 2);
    ASSERT_EQ(lite_frames.ssrc, rtc_rtp_send_stream_ssrc(full.send_stream));
    close_endpoint(&full);
    close_endpoint(&lite);
    frame_rec_destroy(&full_frames);
    frame_rec_destroy(&lite_frames);
}

typedef struct {
    _Atomic unsigned ended;
    _Atomic uint32_t previous;
} source_rec_t;

static void received_source_change(uint32_t previous, uint32_t current, void *user) {
    source_rec_t *record = user;
    if (previous && !current) {
        atomic_store(&record->previous, previous);
        atomic_fetch_add(&record->ended, 1);
    }
}

static bool wait_for_source_end(source_rec_t *record, unsigned target) {
    uint64_t deadline = rtc_time_ms() + 1000;
    while (atomic_load(&record->ended) < target && rtc_time_ms() < deadline)
        SLEEP_MS(1);
    return atomic_load(&record->ended) >= target;
}

static int frame_count(frame_rec_t *record) {
    rtc_mutex_lock(&record->mutex);
    int count = record->count;
    rtc_mutex_unlock(&record->mutex);
    return count;
}

static void expire_received_source(void *user) {
    loop_endpoint_t *endpoint = user;
    uint64_t timeout = rtc_rtcp_source_timeout_ms(rtc_rtp_recv_stream_stats(endpoint->recv_stream));
    rtc_rtp_recv_stream_tick(endpoint->recv_stream, rtc_time_ms() + timeout + 1);
}

TEST(loopback_bye_expiry_and_replay_preserve_source_isolation) {
    frame_rec_t frames, unused_frames;
    data_rec_t data;
    ASSERT(frame_rec_init(&frames));
    ASSERT(frame_rec_init(&unused_frames));
    ASSERT(data_rec_init(&data));
    loop_endpoint_t full, lite;
    ASSERT(make_endpoint(&full, RTC_ICE_MODE_FULL));
    ASSERT(make_endpoint(&lite, RTC_ICE_MODE_LITE));
    ASSERT(setup_media(&full, 96, 97, 1, &unused_frames, NULL));
    ASSERT(setup_media(&lite, 97, 96, 2, &frames, NULL));
    source_rec_t source = {0};
    rtc_rtp_recv_stream_on_source_change(lite.recv_stream, received_source_change, &source);
    rtc_transport_on_data(lite.transport, data_callback, &data);
    ASSERT(connect_endpoints(&full, &lite));
    const uint8_t payload[] = {0x10, 0};
    uint8_t short_header[8] = {0};
    size_t invalid_length = RTP_HEADER_SIZE;
    ASSERT_EQ(rtc_transport_send_rtp(full.transport, short_header, &invalid_length,
                                     sizeof(short_header)),
              RTC_ERR_NOMEM);
    ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload, sizeof(payload), 3000, true),
              RTC_OK);
    ASSERT(wait_for_frame(&frames, 1, 1000));
    uint32_t original = rtc_rtp_send_stream_ssrc(full.send_stream);
    rtc_transport_stats_t membership;
    ASSERT_EQ(rtc_transport_get_stats(lite.transport, &membership), RTC_OK);
    ASSERT(membership.rtcp_members >= 1);
    ASSERT_EQ(membership.rtcp_senders, 1);
    ASSERT_EQ(rtc_transport_send_bye(full.transport, original), RTC_OK);
    ASSERT(wait_for_source_end(&source, 1));
    ASSERT_EQ(rtc_transport_get_stats(lite.transport, &membership), RTC_OK);
    ASSERT_EQ(membership.rtcp_senders, 0);
    ASSERT_EQ(rtc_transport_get_stats(full.transport, &membership), RTC_OK);
    ASSERT_EQ(membership.rtcp_senders, 0);
    ASSERT_EQ(atomic_load(&source.previous), original);
    for (unsigned i = 0; i < 10; i++)
        ASSERT_EQ(rtc_rtp_send_stream_send(full.send_stream, payload, sizeof(payload), 3000, true),
                  RTC_OK);
    ASSERT_EQ(rtc_transport_send_data(full.transport, (const uint8_t *)"barrier", 7), RTC_OK);
    ASSERT(wait_for_data(&data, 1, 1000));
    ASSERT_EQ(frame_count(&frames), 1);

    uint32_t replacement = original == UINT32_MAX ? original - 1 : original + 1;
    rtc_rtp_packet_t packet;
    ASSERT_EQ(rtc_rtp_build(&packet, 96, 700, 12, replacement, true, payload, sizeof(payload)),
              RTC_OK);
    size_t wire_len = packet.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(full.transport, packet.buf, &wire_len, sizeof(packet.buf)),
              RTC_OK);
    ASSERT(wait_for_frame(&frames, 2, 1000));
    ASSERT_EQ(frames.ssrc, replacement);
    ASSERT_EQ(rtc_transport_invoke(lite.transport, expire_received_source, &lite), RTC_OK);
    ASSERT(wait_for_source_end(&source, 2));
    ASSERT_EQ(atomic_load(&source.previous), replacement);
    ASSERT_EQ(rtc_transport_retransmit_rtp(full.transport, packet.buf, wire_len), RTC_OK);
    ASSERT_EQ(rtc_transport_send_data(full.transport, (const uint8_t *)"barrier", 7), RTC_OK);
    ASSERT(wait_for_data(&data, 2, 1000));
    ASSERT_EQ(frame_count(&frames), 2);
    ASSERT_EQ(rtc_rtp_build(&packet, 96, 701, 3012, replacement, true, payload, sizeof(payload)),
              RTC_OK);
    wire_len = packet.buf_len;
    ASSERT_EQ(rtc_transport_send_rtp(full.transport, packet.buf, &wire_len, sizeof(packet.buf)),
              RTC_OK);
    ASSERT(wait_for_frame(&frames, 3, 1000));
    ASSERT_EQ(frame_count(&frames), 3);
    ASSERT_EQ(frames.ssrc, replacement);
    close_endpoint(&full);
    close_endpoint(&lite);
    data_rec_destroy(&data);
    frame_rec_destroy(&frames);
    frame_rec_destroy(&unused_frames);
}

typedef struct {
    loop_endpoint_t *endpoint;
    rtc_rtcp_stats_t sender, receiver;
    rtc_rtcp_packet_t packet;
    int rc;
} shared_report_t;

static void snapshot_shared_report(void *user) {
    shared_report_t *report = user;
    rtc_interceptor_chain_tick(&report->endpoint->media.chain, rtc_time_ms());
    report->sender = *rtc_rtp_send_stream_stats(report->endpoint->send_stream);
    report->receiver = *rtc_rtp_recv_stream_stats(report->endpoint->recv_stream);
    rtc_rtcp_packet_t wire;
    report->rc = rtc_rtcp_build_sr(&wire, &report->sender);
    if (report->rc == RTC_OK)
        report->rc = rtc_rtcp_parse(&report->packet, wire.buf, wire.buf_len);
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
    ASSERT(setup_media(&full, 96, 97, 0, &full_frames, &full_feedback));
    ASSERT(setup_media(&lite, 97, 96, 0, &lite_frames, NULL));
    ASSERT(connect_endpoints(&full, &lite));

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

    shared_report_t report = {.endpoint = &full};
    ASSERT_EQ(rtc_transport_invoke(full.transport, snapshot_shared_report, &report), RTC_OK);
    ASSERT_EQ(report.rc, RTC_OK);
    ASSERT_EQ(report.packet.sr.sender_pkt_count, 2);
    ASSERT_EQ(report.packet.report_count, 1);
    ASSERT_EQ(report.packet.reports[0].ssrc, rtc_rtp_send_stream_ssrc(lite.send_stream));
    ASSERT_EQ(report.sender.next_report_time, report.receiver.next_report_time);
    ASSERT_EQ(report.sender.packets_received, report.receiver.packets_received);

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

typedef struct {
    rtc_transport_t *transport;
    rtc_mutex_t mutex;
    rtc_cond_t cond;
    bool ready, go;
    _Atomic unsigned unexpected;
} paced_producer_t;

static void *paced_producer(void *user) {
    paced_producer_t *producer = user;
    rtc_mutex_lock(&producer->mutex);
    producer->ready = true;
    rtc_cond_signal(&producer->cond);
    while (!producer->go)
        rtc_cond_wait_timeout(&producer->cond, &producer->mutex, 1000);
    rtc_mutex_unlock(&producer->mutex);
    uint8_t payload[1000] = {0x10};
    for (uint16_t seq = 100; seq < 612; seq++) {
        rtc_rtp_packet_t packet;
        if (rtc_rtp_build(&packet, 117, seq, (uint32_t)(seq / 10) * 3000, 0x11223344, seq % 10 == 9,
                          payload, sizeof(payload)) != RTC_OK) {
            atomic_fetch_add(&producer->unexpected, 1);
            break;
        }
        size_t len = packet.buf_len;
        int rc = rtc_transport_send_rtp_media(producer->transport, packet.buf, &len,
                                              sizeof(packet.buf), true, 1000);
        if (rc != RTC_OK && rc != RTC_ERR_INVALID && rc != RTC_ERR_NOMEM)
            atomic_fetch_add(&producer->unexpected, 1);
    }
    rtc_transport_release(producer->transport);
    return NULL;
}

TEST(paced_producer_races_discard_and_owner_destruction) {
    loop_endpoint_t full, lite;
    ASSERT(make_endpoint_ex(&full, RTC_ICE_MODE_FULL, true));
    ASSERT(make_endpoint(&lite, RTC_ICE_MODE_LITE));
    ASSERT(connect_endpoints(&full, &lite));
    paced_producer_t producer = {.transport = full.transport};
    ASSERT_EQ(rtc_mutex_init(&producer.mutex), RTC_OK);
    ASSERT_EQ(rtc_cond_init(&producer.cond), RTC_OK);
    rtc_transport_retain(producer.transport);
    rtc_thread_t thread;
    ASSERT_EQ(rtc_thread_create(&thread, paced_producer, &producer), RTC_OK);
    rtc_mutex_lock(&producer.mutex);
    while (!producer.ready)
        rtc_cond_wait_timeout(&producer.cond, &producer.mutex, 1000);
    uint8_t payload[1000] = {0x10};
    for (uint16_t seq = 0; seq < 10; seq++) {
        rtc_rtp_packet_t packet;
        ASSERT_EQ(rtc_rtp_build(&packet, 117, seq, 3000, 0x11223344, seq == 9, payload,
                                sizeof(payload)),
                  RTC_OK);
        size_t len = packet.buf_len;
        ASSERT_EQ(rtc_transport_send_rtp_media(full.transport, packet.buf, &len, sizeof(packet.buf),
                                               true, 1000),
                  RTC_OK);
    }
    rtc_transport_stats_t stats;
    ASSERT_EQ(rtc_transport_get_stats(full.transport, &stats), RTC_OK);
    ASSERT(stats.rtp_packets_queued > 0);
    producer.go = true;
    rtc_cond_signal(&producer.cond);
    rtc_mutex_unlock(&producer.mutex);
    rtc_transport_discard_rtp(full.transport, 0x11223344);
    rtc_transport_destroy(full.transport);
    full.transport = NULL;
    rtc_thread_join(&thread);
    ASSERT_EQ(atomic_load(&producer.unexpected), 0);
    close_endpoint(&full);
    close_endpoint(&lite);
    rtc_cond_destroy(&producer.cond);
    rtc_mutex_destroy(&producer.mutex);
}

int main(void) {
    rtc_init();
    RUN_TEST(loopback_full_to_lite_media_feedback_and_data);
    RUN_TEST(loopback_collision_rotates_only_the_independent_local_reporter);
    RUN_TEST(loopback_collision_cancels_staged_ciphertext_and_old_repair);
    RUN_TEST(loopback_bye_expiry_and_replay_preserve_source_isolation);
    RUN_TEST(paced_producer_races_discard_and_owner_destruction);
    rtc_cleanup();
    TEST_SUMMARY();
}
