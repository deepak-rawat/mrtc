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

/* ------------------------------------------------------------------ */
/*  Test: packetize a small frame (single payload)                     */
/* ------------------------------------------------------------------ */
TEST(vp8_packetize_small) {
    /* Small "VP8 frame" — fits in one payload */
    uint8_t frame[100];
    memset(frame, 0x42, sizeof(frame));

    rtc_vp8_payload_t payloads[16];
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

/* ------------------------------------------------------------------ */
/*  Test: packetize a large frame (multiple payloads)                  */
/* ------------------------------------------------------------------ */
TEST(vp8_packetize_large) {
    /* Large frame — exceeds VP8_MAX_FRAG_SIZE */
    size_t frame_size = VP8_MAX_FRAG_SIZE * 3 + 100;
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    ASSERT(frame != NULL);
    memset(frame, 0xAB, frame_size);

    rtc_vp8_payload_t payloads[16];
    int count = 0;
    int rc = rtc_vp8_packetize(frame, frame_size, false, payloads, 16, &count);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(count, 4); /* 3 full + 1 partial */

    /* First payload: S-bit set, marker=false */
    ASSERT((payloads[0].data[0] & 0x10) != 0); /* S-bit */
    ASSERT((payloads[0].data[0] & 0x20) != 0); /* N-bit (non-keyframe) */
    ASSERT(!payloads[0].marker);

    /* Middle payloads: S-bit clear, marker=false */
    ASSERT((payloads[1].data[0] & 0x10) == 0); /* S-bit clear */
    ASSERT(!payloads[1].marker);

    /* Last payload: marker=true */
    ASSERT(payloads[3].marker);

    printf("    large frame (%zu bytes): %d payloads\n", frame_size, count);
    free(frame);
}

/* ------------------------------------------------------------------ */
/*  Test: depacketize single-payload frame                             */
/* ------------------------------------------------------------------ */
TEST(vp8_depacketize_single) {
    rtc_vp8_depacketizer_t depac;
    rtc_vp8_depacketizer_init(&depac);

    /* Build a VP8 payload manually: [S-bit descriptor][data] */
    uint8_t vp8_data[50];
    memset(vp8_data, 0xCD, sizeof(vp8_data));

    uint8_t payload[VP8_PD_SIZE + sizeof(vp8_data)];
    payload[0] = 0x10; /* S-bit set, keyframe (N=0) */
    memcpy(payload + VP8_PD_SIZE, vp8_data, sizeof(vp8_data));

    const uint8_t *frame_out = NULL;
    size_t frame_len = 0;
    bool is_keyframe = false;

    int rc = rtc_vp8_depacketize(&depac, payload, sizeof(payload), 3000, true, &frame_out,
                                 &frame_len, &is_keyframe);
    ASSERT_EQ(rc, RTC_OK); /* Complete frame */
    ASSERT_EQ(frame_len, sizeof(vp8_data));
    ASSERT(is_keyframe);
    ASSERT_MEM_EQ(frame_out, vp8_data, sizeof(vp8_data));

    printf("    depacketized single-payload: %zu bytes, keyframe=%d\n", frame_len, is_keyframe);
}

/* ------------------------------------------------------------------ */
/*  Test: round-trip small frame                                       */
/* ------------------------------------------------------------------ */
TEST(vp8_roundtrip_small) {
    uint8_t frame[200];
    for (int i = 0; i < (int)sizeof(frame); i++)
        frame[i] = (uint8_t)(i & 0xFF);

    /* Packetize */
    rtc_vp8_payload_t payloads[4];
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

    rc = rtc_vp8_depacketize(&depac, payloads[0].data, payloads[0].len, 3000, payloads[0].marker,
                             &frame_out, &frame_len, &is_keyframe);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(frame_len, sizeof(frame));
    ASSERT(is_keyframe);
    ASSERT_MEM_EQ(frame_out, frame, sizeof(frame));

    printf("    small frame round-trip: %zu bytes preserved\n", frame_len);
}

/* ------------------------------------------------------------------ */
/*  Test: round-trip large frame                                       */
/* ------------------------------------------------------------------ */
TEST(vp8_roundtrip_large) {
    /* Create a frame larger than one payload */
    size_t frame_size = VP8_MAX_FRAG_SIZE * 2 + 500;
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    ASSERT(frame != NULL);
    for (size_t i = 0; i < frame_size; i++)
        frame[i] = (uint8_t)(i & 0xFF);

    /* Packetize */
    rtc_vp8_payload_t payloads[16];
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
        rc = rtc_vp8_depacketize(&depac, payloads[i].data, payloads[i].len, ts, payloads[i].marker,
                                 &frame_out, &frame_len, &is_keyframe);

        if (i < count - 1) {
            ASSERT(rc != RTC_OK); /* not last, need more */
        } else {
            ASSERT_EQ(rc, RTC_OK);
        }
    }

    ASSERT_EQ(frame_len, frame_size);
    ASSERT(!is_keyframe); /* N-bit was set */
    ASSERT_MEM_EQ(frame_out, frame, frame_size);

    printf("    large frame round-trip: %zu bytes, %d payloads\n", frame_len, count);
    free(frame);
}

/* ------------------------------------------------------------------ */
/*  Test: depacketizer rejects orphan fragments                        */
/* ------------------------------------------------------------------ */
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

    int rc = rtc_vp8_depacketize(&depac, payload, sizeof(payload), 3000, true, &frame_out,
                                 &frame_len, &is_keyframe);
    ASSERT(rc != RTC_OK); /* Should not produce a frame */

    printf("    correctly rejected orphan fragment\n");
}

/* ------------------------------------------------------------------ */
/*  Test: multiple frames in sequence                                  */
/* ------------------------------------------------------------------ */
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
        rtc_vp8_payload_t payloads[4];
        int count = 0;
        int rc = rtc_vp8_packetize(frame, sizeof(frame), (f == 0), payloads, 4, &count);
        ASSERT_EQ(rc, RTC_OK);

        ts += 3000; /* advance timestamp per frame */

        /* Feed to depacketizer */
        for (int i = 0; i < count; i++) {
            const uint8_t *frame_out = NULL;
            size_t frame_len = 0;
            bool is_kf = false;

            rc = rtc_vp8_depacketize(&depac, payloads[i].data, payloads[i].len, ts,
                                     payloads[i].marker, &frame_out, &frame_len, &is_kf);
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

/* ------------------------------------------------------------------ */
/*  Test: depacketize with 15-bit (M-bit) PictureID                    */
/*  RFC 7741 §4.2: if first PictureID byte's high bit is set, the ID  */
/*  spans 2 bytes rather than 1.                                       */
/* ------------------------------------------------------------------ */
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

    int rc = rtc_vp8_depacketize(&depac, payload, sizeof(payload), 4000, true, &frame_out,
                                 &frame_len, &is_keyframe);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(frame_len, sizeof(vp8_data));
    ASSERT_MEM_EQ(frame_out, vp8_data, sizeof(vp8_data));

    /* Sanity: same payload with M-bit clear is a 1-byte PictureID. The
     * descriptor would be 3 bytes (not 4), so a depacketizer that does NOT
     * honor the M-bit would have included payload[3]=0x34 as VP8 data. */
    printf("    depacketized 2-byte PictureID (M-bit): %zu bytes\n", frame_len);
}

/* ------------------------------------------------------------------ */
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

    TEST_SUMMARY();
}
