/*
 * VP8 RTP packetization/depacketization tests.
 *
 * Tests:
 *   1. Packetize a small frame (single payload)
 *   2. Packetize a large frame (multiple payloads)
 *   3. Depacketize single-payload frame
 *   4. Packetize → depacketize round-trip (small frame)
 *   5. Packetize → depacketize round-trip (large frame)
 *   6. VP8 payload descriptor flags (S-bit, N-bit)
 *   7. Depacketizer handles missing start fragment
 *   8. Multiple frames in sequence
 */
#include "vp8_packetizer.h"
#include "test_harness.h"

TEST(vp8_packetize_small) {
    /* Small "VP8 frame" — fits in one payload */
    uint8_t frame[100];
    memset(frame, 0x42, sizeof(frame));

    video_payload_t payloads[16];
    int count = 0;
    int rc = rtc_vp8_packetize(frame, sizeof(frame), true, payloads, 16, &count);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(count, 1);

    /* Single payload: should have marker=true */
    ASSERT(payloads[0].marker);

    /* Payload should be VP8_PD_SIZE + 100 bytes */
    ASSERT_EQ(payloads[0].len, VP8_PD_SIZE + sizeof(frame));

    /* VP8 payload descriptor: S-bit set, N-bit clear (keyframe) */
    ASSERT((payloads[0].data[0] & 0x10) != 0); /* S-bit: start of partition */
    ASSERT((payloads[0].data[0] & 0x20) == 0); /* N-bit clear = keyframe */

    printf("    small frame: %d payload(s), len=%zu bytes\n", count, payloads[0].len);
}

TEST(vp8_packetize_large) {
    /* Large frame — exceeds VP8_MAX_FRAG_SIZE */
    size_t frame_size = VP8_MAX_FRAG_SIZE * 3 + 100;
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    ASSERT(frame != NULL);
    memset(frame, 0xAB, frame_size);

    video_payload_t payloads[16];
    int count = 0;
    int rc = rtc_vp8_packetize(frame, frame_size, false, payloads, 16, &count);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(count, 4); /* 3 full + 1 partial */

    /* First payload: S-bit set, marker=false */
    ASSERT((payloads[0].data[0] & 0x10) != 0); /* S-bit */
    ASSERT((payloads[0].data[0] & 0x20) == 0); /* Interframes may be references. */
    ASSERT(!payloads[0].marker);

    /* Middle payloads: S-bit clear, marker=false */
    ASSERT((payloads[1].data[0] & 0x10) == 0); /* S-bit clear */
    ASSERT(!payloads[1].marker);

    /* Last payload: marker=true */
    ASSERT(payloads[3].marker);

    printf("    large frame (%zu bytes): %d payloads\n", frame_size, count);
    free(frame);
}

TEST(vp8_depacketize_single) {
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    /* Build a VP8 payload manually: [S-bit descriptor][data] */
    uint8_t vp8_data[50];
    memset(vp8_data, 0xCC, sizeof(vp8_data));

    uint8_t payload[VP8_PD_SIZE + sizeof(vp8_data)];
    payload[0] = 0x10; /* S-bit set, keyframe (N=0) */
    memcpy(payload + VP8_PD_SIZE, vp8_data, sizeof(vp8_data));

    const uint8_t *frame_out = NULL;
    size_t frame_len = 0;
    bool is_keyframe = false;

    int rc = rtc_vp8_depacketize(&depac, payload, sizeof(payload), 1, 3000, true, &frame_out,
                                 &frame_len, &is_keyframe);
    ASSERT_EQ(rc, RTC_OK); /* Complete frame */
    ASSERT_EQ(frame_len, sizeof(vp8_data));
    ASSERT(is_keyframe);
    ASSERT_MEM_EQ(frame_out, vp8_data, sizeof(vp8_data));

    printf("    depacketized single-payload: %zu bytes, keyframe=%d\n", frame_len, is_keyframe);
}

TEST(vp8_roundtrip_small) {
    uint8_t frame[200];
    for (int i = 0; i < (int)sizeof(frame); i++)
        frame[i] = (uint8_t)(i & 0xFF);

    /* Packetize */
    video_payload_t payloads[4];
    int count = 0;
    int rc = rtc_vp8_packetize(frame, sizeof(frame), true, payloads, 4, &count);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(count, 1);

    /* Depacketize */
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    const uint8_t *frame_out = NULL;
    size_t frame_len = 0;
    bool is_keyframe = false;

    rc = rtc_vp8_depacketize(&depac, payloads[0].data, payloads[0].len, 1, 3000, payloads[0].marker,
                             &frame_out, &frame_len, &is_keyframe);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(frame_len, sizeof(frame));
    ASSERT(is_keyframe);
    ASSERT_MEM_EQ(frame_out, frame, sizeof(frame));

    printf("    small frame round-trip: %zu bytes preserved\n", frame_len);
}

TEST(vp8_roundtrip_large) {
    /* Create a frame larger than one payload */
    size_t frame_size = VP8_MAX_FRAG_SIZE * 2 + 500;
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    ASSERT(frame != NULL);
    for (size_t i = 0; i < frame_size; i++)
        frame[i] = (uint8_t)(i & 0xFF);
    frame[0] = 1;

    /* Packetize */
    video_payload_t payloads[16];
    int count = 0;
    int rc = rtc_vp8_packetize(frame, frame_size, false, payloads, 16, &count);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(count > 1);

    /* Depacketize all payloads (all share same timestamp) */
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    const uint8_t *frame_out = NULL;
    size_t frame_len = 0;
    bool is_keyframe = false;

    uint32_t ts = 6000; /* arbitrary shared timestamp */
    for (int i = 0; i < count; i++) {
        rc = rtc_vp8_depacketize(&depac, payloads[i].data, payloads[i].len, (uint16_t)i, ts,
                                 payloads[i].marker, &frame_out, &frame_len, &is_keyframe);

        if (i < count - 1) {
            ASSERT(rc != RTC_OK); /* not last, need more */
        } else {
            ASSERT_EQ(rc, RTC_OK);
        }
    }

    ASSERT_EQ(frame_len, frame_size);
    ASSERT(!is_keyframe); /* VP8 P bit is set. */
    ASSERT_MEM_EQ(frame_out, frame, frame_size);

    printf("    large frame round-trip: %zu bytes, %d payloads\n", frame_len, count);
    free(frame);
}

TEST(vp8_depacketize_no_start) {
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    /* Build a payload WITHOUT S-bit (continuation) */
    uint8_t payload[VP8_PD_SIZE + 50];
    payload[0] = 0x00; /* No S-bit */
    memset(payload + VP8_PD_SIZE, 0xEE, 50);

    const uint8_t *frame_out = NULL;
    size_t frame_len = 0;
    bool is_keyframe = false;

    int rc = rtc_vp8_depacketize(&depac, payload, sizeof(payload), 1, 3000, true, &frame_out,
                                 &frame_len, &is_keyframe);
    ASSERT(rc != RTC_OK); /* Should not produce a frame */

    printf("    correctly rejected orphan fragment\n");
}

TEST(vp8_multiple_frames) {
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    int frames_decoded = 0;
    uint32_t ts = 0;

    for (int f = 0; f < 5; f++) {
        /* Create a frame with unique content */
        uint8_t frame[300];
        memset(frame, (uint8_t)(f + 1), sizeof(frame));

        /* Packetize */
        video_payload_t payloads[4];
        int count = 0;
        int rc = rtc_vp8_packetize(frame, sizeof(frame), (f == 0), payloads, 4, &count);
        ASSERT_EQ(rc, RTC_OK);

        ts += 3000; /* advance timestamp per frame */

        /* Feed to depacketizer */
        for (int i = 0; i < count; i++) {
            const uint8_t *frame_out = NULL;
            size_t frame_len = 0;
            bool is_kf = false;

            rc = rtc_vp8_depacketize(&depac, payloads[i].data, payloads[i].len,
                                     (uint16_t)(f * 4 + i), ts, payloads[i].marker, &frame_out,
                                     &frame_len, &is_kf);
            if (rc == RTC_OK) {
                ASSERT_EQ(frame_len, sizeof(frame));
                uint8_t expected_byte = (uint8_t)(f + 1);
                ASSERT_EQ(frame_out[0], expected_byte);
                ASSERT_EQ(frame_out[frame_len - 1], expected_byte);
                frames_decoded++;
            }
        }
    }

    ASSERT_EQ(frames_decoded, 5);
    printf("    %d frames packetized and depacketized in sequence\n", frames_decoded);
}

/* RFC 7741 §4.2: if the first PictureID byte's high bit is set, the ID
 * spans 2 bytes rather than 1. */
TEST(vp8_depacketize_long_picid) {
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    uint8_t vp8_data[20];
    memset(vp8_data, 0xAB, sizeof(vp8_data));

    /* Build payload with X-bit + I-bit + 2-byte PictureID (M-bit set). */
    uint8_t payload[4 + sizeof(vp8_data)];
    payload[0] = 0x90;        /* X=1, S=1 (start) */
    payload[1] = 0x80;        /* I=1, L=0, T=0, K=0 */
    payload[2] = 0x80 | 0x12; /* PictureID byte 1: M-bit set + upper 7 bits */
    payload[3] = 0x34;        /* PictureID byte 2: lower 8 bits */
    memcpy(payload + 4, vp8_data, sizeof(vp8_data));

    const uint8_t *frame_out = NULL;
    size_t frame_len = 0;
    bool is_keyframe = false;

    int rc = rtc_vp8_depacketize(&depac, payload, sizeof(payload), 1, 4000, true, &frame_out,
                                 &frame_len, &is_keyframe);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(frame_len, sizeof(vp8_data));
    ASSERT_MEM_EQ(frame_out, vp8_data, sizeof(vp8_data));

    /* Sanity: same payload with M-bit clear is a 1-byte PictureID. The
     * descriptor would be 3 bytes (not 4), so a depacketizer that does NOT
     * honor the M-bit would have included payload[3]=0x34 as VP8 data. */
    printf("    depacketized 2-byte PictureID (M-bit): %zu bytes\n", frame_len);
}

TEST(vp8_reorder_loss_wrap_and_metadata) {
    uint8_t frame[VP8_MAX_FRAG_SIZE * 2 + 20];
    memset(frame, 0x54, sizeof(frame));
    video_payload_t payloads[4];
    int count;
    ASSERT_EQ(rtc_vp8_packetize(frame, sizeof(frame), true, payloads, 4, &count), RTC_OK);
    ASSERT_EQ(count, 3);
    rtc_vp8_depacketizer_t d;
    rtc_vp8_depacketizer_init(&d);
    const uint8_t *out;
    size_t len;
    bool key;
    /* Last arrives before first; duplicate and wrap must not corrupt output. */
    ASSERT(rtc_vp8_depacketize(&d, payloads[2].data, payloads[2].len, 0, 100, true, &out, &len,
                               &key) != RTC_OK);
    ASSERT(rtc_vp8_depacketize(&d, payloads[0].data, payloads[0].len, 65534, 100, false, &out, &len,
                               &key) != RTC_OK);
    ASSERT(rtc_vp8_depacketize(&d, payloads[0].data, payloads[0].len, 65534, 100, false, &out, &len,
                               &key) != RTC_OK);
    ASSERT_EQ(rtc_vp8_depacketize(&d, payloads[1].data, payloads[1].len, 65535, 100, false, &out,
                                  &len, &key),
              RTC_OK);
    ASSERT_EQ(len, sizeof(frame));
    ASSERT_MEM_EQ(out, frame, len);
    ASSERT(key);
    /* Neither a missing middle nor a missing end can produce a frame. */
    ASSERT(rtc_vp8_depacketize(&d, payloads[0].data, payloads[0].len, 1, 200, false, &out, &len,
                               &key) != RTC_OK);
    ASSERT(rtc_vp8_depacketize(&d, payloads[2].data, payloads[2].len, 3, 200, true, &out, &len,
                               &key) != RTC_OK);
    ASSERT_EQ(len, 0);
    ASSERT(rtc_vp8_depacketize(&d, payloads[0].data, payloads[0].len, 4, 300, false, &out, &len,
                               &key) != RTC_OK);
    ASSERT_EQ(d.dropped_frames, 1);
    /* S on partition one is not a new frame start. */
    uint8_t partition[] = {0x11, 0};
    ASSERT(rtc_vp8_depacketize(&d, partition, sizeof(partition), 6, 400, true, &out, &len, &key) !=
           RTC_OK);
    ASSERT_EQ(d.dropped_frames, 2);
    /* Keyframe P=0 even with N=1; interframe P=1 even with N=0. */
    uint8_t key_payload[] = {0x30, 0};
    ASSERT_EQ(rtc_vp8_depacketize(&d, key_payload, sizeof(key_payload), 7, 500, true, &out, &len,
                                  &key),
              RTC_OK);
    ASSERT(key);
    uint8_t inter_payload[] = {0x10, 1};
    ASSERT_EQ(rtc_vp8_depacketize(&d, inter_payload, sizeof(inter_payload), 8, 600, true, &out,
                                  &len, &key),
              RTC_OK);
    ASSERT(!key);
    ASSERT(rtc_vp8_depacketize(&d, key_payload, sizeof(key_payload), 7, 500, true, &out, &len,
                               &key) != RTC_OK);
}

TEST(vp8_padding_preserves_sequence_continuity_without_creating_frames) {
    rtc_vp8_depacketizer_t d;
    rtc_vp8_depacketizer_init(&d);
    uint8_t start[] = {0x10, 2, 3}, end[] = {0, 4};
    const uint8_t *out;
    size_t len;
    bool key;
    ASSERT_EQ(rtc_vp8_depacketize(&d, start, sizeof(start), 65533, 3000, false, &out, &len, &key),
              RTC_ERR_GENERIC);
    ASSERT_EQ(rtc_vp8_depacketize(&d, NULL, 0, 65534, 3000, true, &out, &len, &key),
              RTC_ERR_GENERIC);
    ASSERT_EQ(rtc_vp8_depacketize(&d, end, sizeof(end), 65535, 3000, true, &out, &len, &key),
              RTC_OK);
    uint8_t expected[] = {2, 3, 4};
    ASSERT_EQ(len, sizeof(expected));
    ASSERT_MEM_EQ(out, expected, sizeof(expected));
    ASSERT_EQ(rtc_vp8_depacketize(&d, NULL, 0, 0, 3000, false, &out, &len, &key), RTC_ERR_GENERIC);
    ASSERT_EQ(rtc_vp8_depacketize(&d, NULL, 0, 1, 6000, false, &out, &len, &key), RTC_ERR_GENERIC);
    ASSERT_EQ(rtc_vp8_depacketize(&d, start, sizeof(start), 2, 6000, true, &out, &len, &key),
              RTC_OK);
    ASSERT_EQ(d.dropped_frames, 0);
    ASSERT_EQ(rtc_vp8_depacketize(&d, start, sizeof(start), 4, 9000, true, &out, &len, &key),
              RTC_OK);
    ASSERT_EQ(d.dropped_frames, 1);
}

TEST(video_payload_format_reassembly_isolated_and_loss_reported) {
    const video_payload_format_t *format = video_payload_format_find("VP8");
    ASSERT(format != NULL);
    ASSERT(video_payload_format_find("H264") == NULL);
    void *a = format->create_depacketizer();
    void *b = format->create_depacketizer();
    ASSERT(a && b);
    uint8_t input_a[2000] = {0};
    uint8_t input_b[2000] = {1};
    memset(input_a + 1, 0x31, sizeof(input_a) - 1);
    memset(input_b + 1, 0x72, sizeof(input_b) - 1);
    video_payload_t payloads_a[2], payloads_b[2];
    int count_a, count_b;
    ASSERT_EQ(format->packetize(input_a, sizeof(input_a), true, payloads_a, 2, &count_a), RTC_OK);
    ASSERT_EQ(format->packetize(input_b, sizeof(input_b), false, payloads_b, 2, &count_b), RTC_OK);
    ASSERT_EQ(count_a, 2);
    ASSERT_EQ(count_b, 2);
    video_payload_frame_t frame;
    ASSERT_EQ(format->depacketize(a, payloads_a[0].data, payloads_a[0].len, 65535, 3000, false,
                                  &frame),
              RTC_ERR_GENERIC);
    ASSERT_EQ(format->depacketize(b, payloads_b[1].data, payloads_b[1].len, 0, 3000, true, &frame),
              RTC_ERR_GENERIC);
    ASSERT_EQ(format->depacketize(b, payloads_b[0].data, payloads_b[0].len, 65535, 3000, false,
                                  &frame),
              RTC_OK);
    ASSERT_EQ(frame.len, sizeof(input_b));
    ASSERT_MEM_EQ(frame.data, input_b, sizeof(input_b));
    ASSERT(!frame.is_keyframe);
    ASSERT_EQ(frame.dropped_frames, 0);
    ASSERT_EQ(format->depacketize(a, payloads_a[1].data, payloads_a[1].len, 0, 3000, true, &frame),
              RTC_OK);
    ASSERT_EQ(frame.len, sizeof(input_a));
    ASSERT_MEM_EQ(frame.data, input_a, sizeof(input_a));
    ASSERT(frame.is_keyframe);
    ASSERT_EQ(format->depacketize(a, NULL, 0, 1, 3000, false, &frame), RTC_ERR_GENERIC);
    ASSERT_EQ(frame.dropped_frames, 0);
    ASSERT_EQ(format->depacketize(a, payloads_a[0].data, payloads_a[0].len, 2, 6000, false, &frame),
              RTC_ERR_GENERIC);
    ASSERT_EQ(format->depacketize(a, payloads_a[0].data, payloads_a[0].len, 4, 9000, false, &frame),
              RTC_ERR_GENERIC);
    ASSERT_EQ(frame.dropped_frames, 1);
    format->destroy_depacketizer(a);
    format->destroy_depacketizer(b);
}

int main(void) {
    printf("========================================\n");
    printf("  VP8 RTP Packetization Tests\n");
    printf("========================================\n\n");

    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(vp8_packetize_small);
    RUN_TEST(vp8_packetize_large);
    RUN_TEST(vp8_depacketize_single);
    RUN_TEST(vp8_roundtrip_small);
    RUN_TEST(vp8_roundtrip_large);
    RUN_TEST(vp8_depacketize_no_start);
    RUN_TEST(vp8_multiple_frames);
    RUN_TEST(vp8_depacketize_long_picid);
    RUN_TEST(vp8_reorder_loss_wrap_and_metadata);
    RUN_TEST(vp8_padding_preserves_sequence_continuity_without_creating_frames);

    RUN_TEST(video_payload_format_reassembly_isolated_and_loss_reported);
    TEST_SUMMARY();
}
