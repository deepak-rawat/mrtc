#include "rtc_rtp_pacer.h"
#include "test_harness.h"

typedef struct {
    unsigned packets;
    size_t bytes;
    int fail_after;
    uint8_t last[1200];
} writer_t;

static int write_packet(const uint8_t *wire, size_t len, void *user) {
    writer_t *w = user;
    if (w->fail_after >= 0 && w->packets >= (unsigned)w->fail_after)
        return RTC_ERR_GENERIC;
    w->packets++;
    w->bytes += len;
    memcpy(w->last, wire, len);
    return RTC_OK;
}

static void packet(uint8_t *wire, uint32_t ssrc, uint32_t timestamp, uint16_t seq, bool marker) {
    memset(wire, 0x5a, 1200);
    wire[0] = 0x80;
    wire[1] = 117 | (marker ? 0x80 : 0);
    wire[2] = (uint8_t)(seq >> 8);
    wire[3] = (uint8_t)seq;
    for (unsigned i = 0; i < 4; i++) {
        wire[4 + i] = (uint8_t)(timestamp >> (24 - 8 * i));
        wire[8 + i] = (uint8_t)(ssrc >> (24 - 8 * i));
    }
}

TEST(frame_staging_two_mtu_burst_and_deterministic_spacing) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 384000};
    writer_t w = {.fail_after = -1};
    uint8_t wire[1200];
    for (uint16_t seq = 0; seq < 5; seq++) {
        packet(wire, 42, 9000, seq, seq == 4);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
        if (seq < 4) {
            rtc_rtp_pacer_run(&p, 100, write_packet, &w);
            ASSERT_EQ(w.packets, 0);
        }
    }
    rtc_rtp_pacer_run(&p, 100, write_packet, &w);
    ASSERT_EQ(w.packets, 2);
    ASSERT_EQ(p.packets, 3);
    rtc_rtp_pacer_run(&p, 109, write_packet, &w);
    ASSERT_EQ(w.packets, 2);
    rtc_rtp_pacer_run(&p, 110, write_packet, &w);
    ASSERT_EQ(w.packets, 3);
    rtc_rtp_pacer_run(&p, 120, write_packet, &w);
    rtc_rtp_pacer_run(&p, 130, write_packet, &w);
    ASSERT_EQ(w.packets, 5);
    ASSERT_EQ(p.bytes, 0);
    rtc_pacer_sent_t sent;
    ASSERT(rtc_rtp_pacer_sent(&p, 42, &sent));
    ASSERT_EQ(sent.packets_sent, 5);
    ASSERT_EQ(sent.octets_sent, 5890);
    ASSERT_EQ(sent.send_time_ms, 100);
    ASSERT_MEM_EQ(w.last, wire, sizeof(wire));
    rtc_rtp_pacer_close(&p);
}

TEST(orphan_and_failed_frames_expire_without_resurrection) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 384000};
    writer_t w = {.fail_after = -1};
    uint8_t wire[1200];
    packet(wire, 42, 9000, 0, false);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    rtc_rtp_pacer_run(&p, 300, write_packet, &w);
    ASSERT_EQ(w.packets, 0);
    ASSERT_EQ(p.frames_dropped, 1);
    ASSERT_EQ(p.packets_dropped, 1);
    wire[1] |= 0x80;
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 301), RTC_ERR_NOMEM);
    ASSERT(!rtc_rtp_pacer_repair(&p, wire, sizeof(wire), 301));
    packet(wire, 42, 18000, 1, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 301), RTC_OK);
    rtc_rtp_pacer_run(&p, 301, write_packet, &w);
    ASSERT_EQ(w.packets, 1);
    ASSERT(rtc_rtp_pacer_repair(&p, wire, sizeof(wire), 311));
    rtc_rtp_pacer_discard(&p, 42);
    ASSERT(!rtc_rtp_pacer_repair(&p, wire, sizeof(wire), 321));
    ASSERT_EQ(p.packets, 0);
    rtc_rtp_pacer_close(&p);
}

TEST(bounded_frame_memory_failure_and_recovery) {
    rtc_rtp_pacer_t p = {0};
    writer_t w = {.fail_after = -1};
    uint8_t wire[1200];
    unsigned accepted = 0;
    for (uint16_t i = 0; i < 200; i++) {
        packet(wire, 42, 9000, i, false);
        if (rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100) != RTC_OK)
            break;
        accepted++;
        ASSERT(p.bytes <= RTC_PACER_MAX_FRAME_BYTES);
    }
    ASSERT(accepted > 0 && accepted < 200);
    ASSERT_EQ(p.packets, 0);
    ASSERT_EQ(p.packets_dropped, accepted + 1);
    packet(wire, 42, 18000, 201, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 101), RTC_OK);
    rtc_rtp_pacer_run(&p, 101, write_packet, &w);
    ASSERT_EQ(w.packets, 1);
    rtc_rtp_pacer_close(&p);
}

TEST(socket_failure_cancels_unsent_frame_and_invalidates_repairs) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 384000};
    writer_t w = {.fail_after = 1};
    uint8_t wire[1200], first[1200];
    for (uint16_t seq = 0; seq < 5; seq++) {
        packet(wire, 42, 9000, seq, seq == 4);
        if (!seq)
            memcpy(first, wire, sizeof(first));
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    }
    rtc_rtp_pacer_run(&p, 100, write_packet, &w);
    ASSERT_EQ(w.packets, 1);
    ASSERT_EQ(p.packets_dropped, 4);
    ASSERT_EQ(p.frames_dropped, 1);
    ASSERT_EQ(p.packets, 0);
    ASSERT(!rtc_rtp_pacer_repair(&p, first, sizeof(first), 110));
    rtc_pacer_sent_t sent;
    ASSERT(rtc_rtp_pacer_sent(&p, 42, &sent));
    ASSERT_EQ(sent.packets_sent, 1);
    ASSERT_EQ(sent.octets_sent, 1178);
    rtc_rtp_pacer_close(&p);
}

TEST(stale_queue_deadline_drops_remainder_and_recovers) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 100000};
    writer_t w = {.fail_after = -1};
    uint8_t wire[1200];
    for (uint16_t seq = 0; seq < 10; seq++) {
        packet(wire, 42, 9000, seq, seq == 9);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    }
    rtc_rtp_pacer_run(&p, 100, write_packet, &w);
    ASSERT_EQ(w.packets, 2);
    rtc_rtp_pacer_run(&p, 300, write_packet, &w);
    ASSERT_EQ(w.packets, 2);
    ASSERT_EQ(p.packets_dropped, 8);
    packet(wire, 42, 18000, 10, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 301), RTC_OK);
    rtc_rtp_pacer_run(&p, 301, write_packet, &w);
    ASSERT_EQ(w.packets, 3);
    rtc_rtp_pacer_close(&p);
}

TEST(transport_and_source_bounds_and_selective_cancellation) {
    rtc_rtp_pacer_t p = {0};
    writer_t w = {.fail_after = -1};
    uint8_t wire[1200];
    for (uint32_t ssrc = 1; ssrc <= RTC_PACER_MAX_SOURCES; ssrc++) {
        packet(wire, ssrc, 9000, 0, true);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    }
    packet(wire, 1000, 9000, 0, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_ERR_NOMEM);
    rtc_rtp_pacer_forget(&p, 1);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    rtc_rtp_pacer_clear(&p);
    ASSERT_EQ(p.bytes, 0);
    ASSERT_EQ(p.packets, 0);
    rtc_rtp_pacer_run(&p, 101, write_packet, &w);
    ASSERT_EQ(w.packets, 0);
    rtc_rtp_pacer_close(&p);
    ASSERT_EQ(p.source_count, 0);
}

int main(void) {
    RUN_TEST(frame_staging_two_mtu_burst_and_deterministic_spacing);
    RUN_TEST(orphan_and_failed_frames_expire_without_resurrection);
    RUN_TEST(bounded_frame_memory_failure_and_recovery);
    RUN_TEST(socket_failure_cancels_unsent_frame_and_invalidates_repairs);
    RUN_TEST(stale_queue_deadline_drops_remainder_and_recovers);
    RUN_TEST(transport_and_source_bounds_and_selective_cancellation);
    TEST_SUMMARY();
}
