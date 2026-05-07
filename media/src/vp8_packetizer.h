/*
 * VP8 RTP Payload Format (RFC 7741) - Packetization and depacketization.
 *
 * Packetizes VP8 frames into MTU-sized payloads with VP8 descriptors,
 * and reassembles them on the receive side.
 *
 * VP8 RTP payload descriptor (simplified, 1-byte form):
 *   0 1 2 3 4 5 6 7
 *  +-+-+-+-+-+-+-+-+
 *  |X|R|N|S|R| PID |
 *  +-+-+-+-+-+-+-+-+
 *
 *  X: Extension present (we use 0)
 *  R: Reserved
 *  N: Non-reference frame (0 = reference frame)
 *  S: Start of VP8 partition (1 = first packet of frame)
 *  PID: Partition index (0 for single-partition VP8)
 */
#ifndef RTC_MEDIA_VP8_PACKETIZER_H
#define RTC_MEDIA_VP8_PACKETIZER_H

#include <rtc/rtc_common.h>

/* VP8 payload descriptor size (1-byte minimal form) */
#define VP8_PD_SIZE 1

/* Safe MTU for RTP payload (matches RTP_MAX_PACKET in rtc_rtp.h) */
#define VP8_MTU_SIZE 1400

/* Max VP8 data per payload (minus descriptor) */
#define VP8_MAX_FRAG_SIZE (VP8_MTU_SIZE - VP8_PD_SIZE)

/* Max reassembly buffer (640x480 keyframe can be ~100KB+) */
#define VP8_MAX_FRAME_SIZE (256 * 1024)

/* Max VP8 payload size (descriptor + VP8 data) */
#define VP8_MAX_PAYLOAD (VP8_PD_SIZE + VP8_MAX_FRAG_SIZE)

/* VP8 payload (descriptor + VP8 data, no RTP header) */
typedef struct {
    uint8_t data[VP8_MAX_PAYLOAD]; /* VP8 descriptor + VP8 data */
    size_t len;
    bool marker; /* true for last payload of frame */
} rtc_vp8_payload_t;

/* VP8 depacketizer: reassembles payloads into complete frames */
typedef struct {
    uint8_t frame_buf[VP8_MAX_FRAME_SIZE];
    size_t frame_len;
    uint32_t frame_timestamp; /* RTP timestamp of current frame */
    bool collecting;          /* true if we're mid-frame */
    bool got_start;           /* true if we've seen the S-bit payload */
} rtc_vp8_depacketizer_t;

/*
 * Packetize a VP8 frame into MTU-sized payloads (VP8 descriptor + VP8 data).
 * No RTP header is added — suitable for use with rtc_rtp_sender_send()
 * which builds RTP + SRTP independently per peer.
 *
 * Input:
 *   frame, frame_len: raw VP8 bitstream for one frame
 *   is_keyframe: true if this is an IDR/keyframe
 *
 * Output:
 *   payloads: array of payload outputs (caller allocates)
 *   max_payloads: size of payloads array
 *   payload_count: number of payloads actually produced
 *
 * Returns RTC_OK or error.
 */
int rtc_vp8_packetize(const uint8_t *frame, size_t frame_len, bool is_keyframe,
                      rtc_vp8_payload_t *payloads, int max_payloads, int *payload_count);

/* Initialize VP8 depacketizer */
int rtc_vp8_depacketizer_init(rtc_vp8_depacketizer_t *d);

/*
 * Feed a VP8 RTP payload to the depacketizer.
 *
 * Input:
 *   payload, len: VP8 RTP payload (descriptor + VP8 data)
 *   timestamp: RTP timestamp (used for frame boundary detection)
 *   marker: RTP marker bit (true = last payload of frame)
 *
 * Output:
 *   frame_out: pointer set to the reassembled frame buffer (internal)
 *   frame_len: length of reassembled frame
 *   is_keyframe: set to true if frame is a keyframe
 *
 * Returns:
 *   RTC_OK when a complete frame is assembled
 *   RTC_ERR_GENERIC when more payloads are needed (not an error)
 *   Other negative values on actual error
 */
int rtc_vp8_depacketize(rtc_vp8_depacketizer_t *d, const uint8_t *payload, size_t len,
                        uint32_t timestamp, bool marker, const uint8_t **frame_out,
                        size_t *frame_len, bool *is_keyframe);

#endif /* RTC_MEDIA_VP8_PACKETIZER_H */
