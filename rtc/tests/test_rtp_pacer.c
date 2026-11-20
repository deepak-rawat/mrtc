#include "rtc_rtp_pacer.h"
#include "test_harness.h"
#include <assert.h>
#include <stdlib.h>

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
    ASSERT_EQ(rtc_rtp_pacer_delay_ms(&p, 109), 1);
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

TEST(shared_wire_and_packet_limits_cancel_the_whole_failed_frame) {
    rtc_rtp_pacer_t p = {0};
    uint8_t wire[1200];
    bool rejected = false;
    for (uint32_t ssrc = 1; ssrc <= 3; ssrc++) {
        for (uint16_t seq = 0; seq < 100; seq++) {
            packet(wire, ssrc, 9000, seq, false);
            int rc = rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100);
            ASSERT(p.bytes <= RTC_PACER_MAX_BYTES);
            if (rc != RTC_OK) {
                rejected = true;
                break;
            }
        }
    }
    ASSERT(rejected);
    ASSERT_EQ(p.bytes, 240000);
    rtc_rtp_pacer_close(&p);
    for (uint16_t seq = 0; seq < 1024; seq++) {
        packet(wire, 42, seq, seq, true);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, 12, 0, 100), RTC_OK);
    }
    packet(wire, 42, 1024, 1024, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, 12, 0, 100), RTC_ERR_NOMEM);
    ASSERT_EQ(p.packets, 1024);
    rtc_rtp_pacer_clear(&p);
    ASSERT_EQ(p.packets, 0);
    rtc_rtp_pacer_close(&p);
}

TEST(protection_failure_aborts_staging_without_double_counting_the_rejection) {
    rtc_rtp_pacer_t p = {0};
    uint8_t wire[1200];
    packet(wire, 42, 9000, 0, false);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    rtc_rtp_pacer_abort(&p, 42, false);
    rtc_pacer_sent_t sent;
    ASSERT(rtc_rtp_pacer_sent(&p, 42, &sent));
    ASSERT_EQ(sent.packets_sent, 0);
    ASSERT_EQ(sent.packets_queued, 0);
    ASSERT_EQ(sent.packets_dropped, 2);
    ASSERT_EQ(sent.packets_rejected, 1);
    wire[1] |= 0x80;
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 101), RTC_ERR_NOMEM);
    packet(wire, 42, 18000, 2, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 102), RTC_OK);
    rtc_rtp_pacer_abort(&p, 42, true);
    ASSERT(rtc_rtp_pacer_sent(&p, 42, &sent));
    ASSERT_EQ(sent.packets_queued, 0);
    ASSERT_EQ(sent.packets_dropped - sent.packets_rejected, 1);
    rtc_rtp_pacer_close(&p);
}

TEST(large_atomic_frame_has_bounded_serialization_without_a_large_burst) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 500000};
    writer_t writer = {.fail_after = -1};
    uint8_t wire[1200];
    for (uint16_t seq = 0; seq < 110; seq++) {
        size_t len = seq == 109 ? RTC_PACER_MAX_FRAME_BYTES - 109 * sizeof(wire) : sizeof(wire);
        packet(wire, 42, 9000, seq, seq == 109);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, len, len - 22, 100), RTC_OK);
    }
    uint64_t completed = 0;
    for (uint64_t now = 100; now < 300; now++) {
        size_t before = writer.bytes;
        rtc_rtp_pacer_run(&p, now, write_packet, &writer);
        ASSERT(writer.bytes - before <= RTC_PACER_BURST_BYTES);
        if (writer.packets == 110) {
            completed = now;
            break;
        }
    }
    ASSERT(completed > 100 && completed <= 200);
    ASSERT_EQ(writer.bytes, RTC_PACER_MAX_FRAME_BYTES);
    ASSERT_EQ(p.frames_dropped, 0);
    ASSERT_EQ(p.bytes, 0);
    printf("    %zu-byte atomic frame: %llu ms, burst <= %u bytes\n", writer.bytes,
           (unsigned long long)(completed - 100), RTC_PACER_BURST_BYTES);
    rtc_rtp_pacer_close(&p);
}

TEST(streamed_frame_assembly_is_not_packet_queue_residence) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 500000, .streaming = true};
    writer_t writer = {.fail_after = -1};
    uint8_t wire[1200];
    for (uint16_t seq = 0; seq < 100; seq++) {
        uint64_t now = 100 + (uint64_t)seq * 10;
        packet(wire, 42, 9000, seq, seq == 99);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, now), RTC_OK);
        rtc_rtp_pacer_run(&p, now, write_packet, &writer);
        ASSERT_EQ(writer.packets, (unsigned)seq + 1);
        ASSERT_EQ(p.bytes, 0);
        ASSERT_EQ(rtc_rtp_pacer_pending(&p), seq != 99);
    }
    ASSERT_EQ(p.frames_dropped, 0);
    ASSERT_EQ(writer.bytes, 120000);
    rtc_rtp_pacer_close(&p);
}

typedef struct {
    rtc_rtp_pacer_t *relay;
    uint64_t now;
    writer_t output;
    size_t max_queued;
} relay_t;

static int relay_packet(const uint8_t *wire, size_t len, void *user) {
    relay_t *relay = user;
    int rc = rtc_rtp_pacer_enqueue(relay->relay, wire, len, 1178, relay->now);
    if (relay->relay->bytes > relay->max_queued)
        relay->max_queued = relay->relay->bytes;
    return rc;
}

TEST(large_native_frame_streams_through_sfu_without_whole_frame_double_pacing) {
    rtc_rtp_pacer_t native = {.bitrate_bps = 500000};
    rtc_rtp_pacer_t sfu = {.bitrate_bps = 500000, .streaming = true};
    relay_t relay = {.relay = &sfu, .output = {.fail_after = -1}};
    uint8_t wire[1200];
    for (uint16_t seq = 0; seq < 100; seq++) {
        packet(wire, 42, 9000, seq, seq == 99);
        ASSERT_EQ(rtc_rtp_pacer_enqueue(&native, wire, sizeof(wire), 1178, 100), RTC_OK);
    }
    uint64_t first = 0, completed = 0;
    for (relay.now = 100; relay.now < 300; relay.now++) {
        rtc_rtp_pacer_run(&native, relay.now, relay_packet, &relay);
        rtc_rtp_pacer_run(&sfu, relay.now, write_packet, &relay.output);
        if (!first && relay.output.packets)
            first = relay.now;
        if (relay.output.packets == 100) {
            completed = relay.now;
            break;
        }
    }
    ASSERT_EQ(first, 100);
    ASSERT(completed > first && completed < 250);
    ASSERT(relay.max_queued < 120000 / 2);
    ASSERT_EQ(native.frames_dropped, 0);
    ASSERT_EQ(sfu.frames_dropped, 0);
    printf("    120000-byte native/SFU stream: first=%llu ms complete=%llu ms relay_peak=%zu "
           "bytes\n",
           (unsigned long long)(first - 100), (unsigned long long)(completed - 100),
           relay.max_queued);
    rtc_rtp_pacer_close(&native);
    rtc_rtp_pacer_close(&sfu);
}

TEST(streamed_frame_gap_and_stalled_queue_cancel_repairs_and_recover) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 500000, .streaming = true};
    writer_t writer = {.fail_after = -1};
    uint8_t wire[1200];
    packet(wire, 42, 9000, 0, false);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100), RTC_OK);
    rtc_rtp_pacer_run(&p, 100, write_packet, &writer);
    ASSERT_EQ(writer.packets, 1);
    rtc_rtp_pacer_run(&p, 300, write_packet, &writer);
    ASSERT(!rtc_rtp_pacer_repair(&p, wire, sizeof(wire), 300));
    ASSERT_EQ(p.frames_dropped, 1);
    ASSERT_EQ(p.packets_dropped, 0);
    packet(wire, 42, 12000, 1, false);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 301), RTC_OK);
    rtc_rtp_pacer_run(&p, 501, write_packet, &writer);
    ASSERT_EQ(writer.packets, 1);
    ASSERT_EQ(p.packets_dropped, 1);
    packet(wire, 42, 15000, 2, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 502), RTC_OK);
    rtc_rtp_pacer_run(&p, 502, write_packet, &writer);
    ASSERT_EQ(writer.packets, 2);
    rtc_rtp_pacer_close(&p);
}

TEST(streaming_cannot_evade_total_frame_bytes_or_assembly_lease) {
    rtc_rtp_pacer_t p = {.bitrate_bps = 500000, .streaming = true};
    writer_t writer = {.fail_after = -1};
    uint8_t wire[1200];
    bool rejected = false;
    for (uint16_t seq = 0; seq < 120; seq++) {
        uint64_t now = 100 + (uint64_t)seq * 10;
        packet(wire, 42, 9000, seq, false);
        if (rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, now) != RTC_OK) {
            rejected = true;
            break;
        }
        rtc_rtp_pacer_run(&p, now, write_packet, &writer);
        ASSERT_EQ(p.bytes, 0);
    }
    ASSERT(rejected);
    ASSERT_EQ(p.frames_dropped, 1);
    ASSERT(!rtc_rtp_pacer_pending(&p));
    rtc_rtp_pacer_close(&p);
    for (uint16_t seq = 0; seq < 50; seq++) {
        uint64_t now = 2000 + (uint64_t)seq * 100;
        packet(wire, 42, 12000, seq, false);
        int rc = rtc_rtp_pacer_enqueue(&p, wire, 12, 0, now);
        if (now - 2000 >= RTC_PACER_STREAM_ASSEMBLY_MS) {
            ASSERT_EQ(rc, RTC_ERR_NOMEM);
            break;
        }
        ASSERT_EQ(rc, RTC_OK);
        rtc_rtp_pacer_run(&p, now, write_packet, &writer);
    }
    ASSERT(!rtc_rtp_pacer_pending(&p));
    packet(wire, 42, 15000, 50, true);
    ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, 12, 0, 7000), RTC_OK);
    rtc_rtp_pacer_run(&p, 7000, write_packet, &writer);
    ASSERT(!rtc_rtp_pacer_pending(&p));
    rtc_rtp_pacer_close(&p);
}

typedef struct {
    size_t remaining, live;
} allocation_t;

static void *limited_allocate(size_t bytes, void *user) {
    allocation_t *state = user;
    if (!state->remaining)
        return NULL;
    if (state->remaining != SIZE_MAX)
        state->remaining--;
    void *memory = malloc(bytes);
    if (memory)
        state->live++;
    return memory;
}

static void limited_release(void *memory, void *user) {
    allocation_t *state = user;
    assert(state->live > 0);
    state->live--;
    free(memory);
}

TEST(allocation_failure_cancels_partial_frame_and_recovers) {
    for (unsigned mode = 0; mode < 2; mode++)
        for (size_t budget = 0; budget <= 3; budget++) {
            rtc_rtp_pacer_t p = {.bitrate_bps = 384000, .streaming = mode != 0};
            allocation_t allocation = {.remaining = budget};
            ASSERT_EQ(rtc_rtp_pacer_set_allocator(&p, limited_allocate, NULL, &allocation),
                      RTC_ERR_INVALID);
            ASSERT_EQ(rtc_rtp_pacer_set_allocator(&p, limited_allocate, limited_release,
                                                  &allocation),
                      RTC_OK);
            uint8_t wire[1200];
            packet(wire, 42, 9000, 1, true);
            ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 100),
                      budget < 3 ? RTC_ERR_NOMEM : RTC_OK);
            allocation.remaining = SIZE_MAX;
            packet(wire, 42, 12000, 2, true);
            ASSERT_EQ(rtc_rtp_pacer_enqueue(&p, wire, sizeof(wire), 1178, 101), RTC_OK);
            writer_t writer = {.fail_after = -1};
            rtc_rtp_pacer_run(&p, 101, write_packet, &writer);
            ASSERT_EQ(writer.packets, budget < 3 ? 1 : 2);
            ASSERT_EQ(p.bytes, 0);
            ASSERT_EQ(p.packets, 0);
            ASSERT_EQ(rtc_rtp_pacer_set_allocator(&p, NULL, NULL, NULL), RTC_ERR_INVALID);
            rtc_rtp_pacer_close(&p);
            ASSERT_EQ(allocation.live, 0);
            ASSERT_EQ(rtc_rtp_pacer_set_allocator(&p, NULL, NULL, NULL), RTC_OK);
        }
}

int main(void) {
    RUN_TEST(frame_staging_two_mtu_burst_and_deterministic_spacing);
    RUN_TEST(orphan_and_failed_frames_expire_without_resurrection);
    RUN_TEST(bounded_frame_memory_failure_and_recovery);
    RUN_TEST(socket_failure_cancels_unsent_frame_and_invalidates_repairs);
    RUN_TEST(stale_queue_deadline_drops_remainder_and_recovers);
    RUN_TEST(transport_and_source_bounds_and_selective_cancellation);
    RUN_TEST(shared_wire_and_packet_limits_cancel_the_whole_failed_frame);
    RUN_TEST(protection_failure_aborts_staging_without_double_counting_the_rejection);
    RUN_TEST(allocation_failure_cancels_partial_frame_and_recovers);
    RUN_TEST(large_atomic_frame_has_bounded_serialization_without_a_large_burst);
    RUN_TEST(streamed_frame_assembly_is_not_packet_queue_residence);
    RUN_TEST(large_native_frame_streams_through_sfu_without_whole_frame_double_pacing);
    RUN_TEST(streamed_frame_gap_and_stalled_queue_cancel_repairs_and_recover);
    RUN_TEST(streaming_cannot_evade_total_frame_bytes_or_assembly_lease);
    TEST_SUMMARY();
}
