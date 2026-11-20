#include "rtc/rtc.h"
#include "rtc/rtc_media_session.h"
#include "rtc/rtc_rtp_stream.h"
#include "rtc_rtp_pacer.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t remaining, live;
    bool write_fails;
} fuzz_state_t;

static void *allocate(size_t bytes, void *user) {
    fuzz_state_t *state = user;
    if (!state->remaining)
        return NULL;
    if (state->remaining != SIZE_MAX)
        state->remaining--;
    void *memory = malloc(bytes);
    if (memory)
        state->live++;
    return memory;
}

static void release(void *memory, void *user) {
    fuzz_state_t *state = user;
    assert(state->live > 0);
    state->live--;
    free(memory);
}

static int write_packet(const uint8_t *wire, size_t len, void *user) {
    (void)wire;
    assert(len >= RTP_HEADER_SIZE && len <= RTC_PACER_BURST_BYTES);
    return ((fuzz_state_t *)user)->write_fails ? RTC_ERR_SOCKET : RTC_OK;
}

static void receive_frame(const uint8_t *data, size_t len, uint16_t seq, uint32_t timestamp,
                          uint32_t ssrc, bool marker, void *user) {
    (void)data;
    (void)len;
    (void)seq;
    (void)timestamp;
    (void)ssrc;
    (void)marker;
    (void)user;
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    rtc_init();
    if (rtc_set_log_file(NULL, false) != RTC_OK)
        abort();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size || size > 8192)
        return 0;
    rtc_media_session_t session;
    if (rtc_media_session_init(&session, NULL, NULL) != RTC_OK)
        return 0;
    rtc_rtp_recv_stream_t *streams[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        rtc_rtp_recv_stream_config_t config = {
            .payload_type = (uint8_t)(96 + i), .clock_rate = 90000, .local_ssrc = i + 1};
        streams[i] = rtc_rtp_recv_stream_create(&config);
        if (!streams[i]) {
            rtc_media_session_close(&session);
            rtc_rtp_recv_stream_destroy(streams[0]);
            return 0;
        }
        rtc_rtp_recv_stream_set_active(streams[i], true);
        rtc_rtp_recv_stream_on_frame(streams[i], receive_frame, NULL);
        if (rtc_media_session_add_receiver(&session, streams[i]) != RTC_OK)
            abort();
    }
    rtc_rtp_pacer_t pacer = {.bitrate_bps = 384000, .streaming = (data[0] & 128) != 0};
    fuzz_state_t state = {.remaining = SIZE_MAX};
    if (rtc_rtp_pacer_set_allocator(&pacer, allocate, release, &state) != RTC_OK)
        abort();
    uint64_t now = rtc_time_ms();
    for (unsigned operation = 0; size >= 3 && operation < 128; operation++) {
        uint8_t control = data[0];
        size_t length = (((size_t)data[1] << 8) | data[2]) % 2048;
        data += 3;
        size -= 3;
        if (length > size)
            length = size;
        const uint8_t *block = data;
        data += length;
        size -= length;
        unsigned index = (control >> 3) & 1;
        rtc_rtp_packet_t packet;
        switch (control & 7) {
            case 0:
            case 5:
                if (length < 11 || rtc_rtp_build(&packet, (uint8_t)(96 + index),
                                                 (uint16_t)((uint16_t)block[8] << 8 | block[9]),
                                                 u32(block + 4), u32(block), (block[10] & 1) != 0,
                                                 block + 11, length - 11) != RTC_OK)
                    break;
                if ((control & 7) == 5) {
                    (void)rtc_rtp_pacer_enqueue(&pacer, packet.buf, packet.buf_len,
                                                packet.payload_len, now);
                } else {
                    rtc_rtp_recv_stream_t *stream = rtc_media_session_resolve(&session, &packet);
                    if (stream)
                        rtc_rtp_recv_stream_on_packet(stream, &packet);
                }
                break;
            case 1:
                if (rtc_rtp_parse(&packet, block, length) == RTC_OK) {
                    rtc_rtp_recv_stream_t *stream = rtc_media_session_resolve(&session, &packet);
                    if (stream)
                        rtc_rtp_recv_stream_on_packet(stream, &packet);
                }
                break;
            case 2:
                rtc_media_session_handle_rtcp(&session, block, length);
                break;
            case 3:
                if (length >= 4)
                    rtc_media_session_bind_receiver(&session, streams[index], u32(block));
                break;
            case 4:
                rtc_rtp_recv_stream_set_active(streams[index], (control & 16) != 0);
                now += length ? (uint64_t)block[0] * 1000 : 1;
                rtc_rtp_recv_stream_tick(streams[0], now);
                rtc_rtp_recv_stream_tick(streams[1], now);
                break;
            case 6:
                state.remaining = length ? block[0] % 5 : SIZE_MAX;
                if (length >= 4) {
                    if (control & 16)
                        rtc_rtp_pacer_forget(&pacer, u32(block));
                    else
                        rtc_rtp_pacer_discard(&pacer, u32(block));
                }
                break;
            case 7:
                state.write_fails = (control & 16) != 0;
                now += length ? block[0] : 1;
                rtc_rtp_pacer_run(&pacer, now, write_packet, &state);
                break;
        }
        assert(pacer.bytes <= RTC_PACER_MAX_BYTES && pacer.packets <= 1024);
    }
    rtc_rtp_pacer_close(&pacer);
    assert(state.live == 0 && pacer.bytes == 0 && pacer.packets == 0);
    rtc_media_session_close(&session);
    rtc_rtp_recv_stream_destroy(streams[0]);
    rtc_rtp_recv_stream_destroy(streams[1]);
    return 0;
}
