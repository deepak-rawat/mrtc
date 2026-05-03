# Media Library (libmrtc_media)

Codec support (VP8, Opus), media pipeline for multi-stream encode/decode, jitter buffer, and adaptive rate control.

Depends on `libmrtc` (rtc core) — the pipeline calls `rtc_rtp_sender_send()` directly for send fan-out.

## Architecture

```
Send Side (encode once, fan-out to N peers):
  Raw frame → Pipeline → Encoder → VP8 fragment → rtc_rtp_sender_send(peer1)
                                                 → rtc_rtp_sender_send(peer2)
                                                 → ...

Recv Side (per remote SSRC, direct dispatch via handle):
  rtc_rtp_receiver callback → Pipeline recv handle → Depacketize → Decode → App
```

## Usage Example

```c
// 1. Create pipeline with renderer for decoded remote frames
media_renderer_t renderer = {
    .on_video_frame  = my_on_video,
    .on_audio_samples = my_on_audio,
    .user = app_ctx,
};
media_pipeline_config_t cfg = {
    .default_video_codec = "VP8",
    .default_audio_codec = "opus",
    .renderer = &renderer,
};
media_pipeline_t *pipeline = media_pipeline_create(&cfg);

// 2. Add send streams (creates encoder internally, returns opaque handle)
media_send_stream_t *camera = media_pipeline_add_send_stream(
    pipeline, "camera", true, "VP8", 640, 480, 30, 500);
media_send_stream_t *mic = media_pipeline_add_send_stream(
    pipeline, "mic", false, "opus", 0, 0, 0, 32000);

// 3. Register peers for fan-out (after WebRTC connection established)
media_pipeline_add_send_peer(pipeline, "alice",
                             alice_video_sender, alice_audio_sender);
media_pipeline_add_send_peer(pipeline, "bob",
                             bob_video_sender, bob_audio_sender);

// 4. Push frames — encodes ONCE, sends to ALL registered peers
media_pipeline_push_video(pipeline, camera, &frame);
media_pipeline_push_audio(pipeline, mic, &audio);

// 5. Receive — register lazily on first packet, then direct dispatch
media_recv_stream_t *alice_cam = NULL;
void on_recv_video_rtp(..., uint32_t ssrc, ...) {
    if (!alice_cam)
        alice_cam = media_pipeline_add_recv_stream(pipeline, "alice",
                                                   "camera", ssrc, true, "VP8");
    media_pipeline_recv_rtp(pipeline, alice_cam, payload, len, seq, ts, marker);
    // → VP8 depacketize → decode → renderer.on_video_frame("alice", "camera", frame)
}

// 6. Cleanup
media_pipeline_remove_send_peer(pipeline, "alice");
media_pipeline_remove_peer(pipeline, "alice");  // removes recv streams
media_pipeline_destroy(pipeline);
```

## Modules

### Video Codec (`video_codec.h/c`, `codec_vp8.c`)

Vtable-based codec abstraction supporting multiple backends:

```c
typedef struct video_encoder_ops {
    int (*init)(void *ctx, int w, int h, int bitrate_kbps, int fps);
    int (*encode)(void *ctx, const video_frame_t *frame, video_packet_t *out);
    int (*set_bitrate)(void *ctx, int bitrate_kbps);
    int (*request_keyframe)(void *ctx);
    void (*close)(void *ctx);
} video_encoder_ops_t;
```

**Implementations:**
- **VP8** (`codec_vp8.c`) — libvpx backend, full encode/decode support
- **H.264** — stub ready for OpenH264
- **FFmpeg** — stub ready for FFmpeg backend

**Frame format:** I420 (YUV 4:2:0 planar)

### Audio Codec (`audio_codec.h/c`, `codec_opus.c`)

Same vtable pattern as video codecs.

**Implementation:**
- **Opus** (`codec_opus.c`) — libopus, RFC 7587, 48kHz mono/stereo

**Frame format:** Interleaved PCM S16, 48kHz

### VP8 Packetizer (`vp8_packetizer.h/c`)

RFC 7741 RTP payload format for VP8. Two APIs:

- `rtc_vp8_fragment()` — stateless, produces VP8 descriptor+data payloads (no RTP header). Used by media_pipeline for fan-out to multiple peer senders.
- `rtc_vp8_packetize()` — stateful, produces complete RTP packets with headers. Used for direct send paths.

VP8 payload descriptor (1 byte):
```
  X: Extension (0)
  N: Non-reference frame flag
  S: Start-of-partition bit
  PID: Partition index
```

Depacketizer (`rtc_vp8_depacketize()`) reassembles fragments into complete VP8 frames using marker bit for frame boundary detection.

### Jitter Buffer (`jitter_buffer.h/c`)

Packet reordering and adaptive delay compensation for playout.

- Buffers incoming RTP packets by sequence number
- Tracks inter-arrival jitter and adjusts playout delay dynamically
- Drops packets exceeding max delay threshold
- Default: 80ms target delay, 500ms max delay

### Rate Control (`rate_control.h/c`)

AIMD (Additive Increase, Multiplicative Decrease) bitrate adaptation.

- Observes RTCP Receiver Reports (fraction_lost, jitter)
- Increases bitrate when loss is low, decreases on high loss
- Requests keyframes from encoder when needed

Note: Will be replaced by rtc-layer bandwidth estimation (GCC/REMB) in future.

### Media Pipeline (`media_pipeline.h/c`)

Orchestrates multi-stream encode/decode with per-stream routing and multi-peer fan-out. All functions use opaque handles for O(1) dispatch — no string or SSRC lookups on the hot path.

```
Pipeline manages:
  Send streams:  media_send_stream_t* → encoder → VP8 fragment → fan-out
  Send peers:    peer_id → (video_sender, audio_sender) rtc_rtp_sender_t*
  Recv streams:  media_recv_stream_t* → depacketizer → decoder → renderer
```

**Send API:**
- `media_pipeline_add_send_stream(label, is_video, codec, ...)` → `media_send_stream_t*`
- `media_send_stream_get_ssrc(stream)` → SSRC
- `media_pipeline_push_video(pipeline, stream, frame)` — encode → fragment → fan-out
- `media_pipeline_push_audio(pipeline, stream, audio)` — encode → fan-out
- `media_pipeline_add_send_peer(peer_id, video_sender, audio_sender)` — register peer
- `media_pipeline_remove_send_peer(peer_id)` — unregister peer

**Recv API:**
- `media_pipeline_add_recv_stream(peer_id, label, ssrc, is_video, codec)` → `media_recv_stream_t*`
- `media_pipeline_recv_rtp(pipeline, stream, data, len, seq, ts, marker)` — direct dispatch
- `media_pipeline_remove_recv_stream(ssrc)` — remove by SSRC
- `media_pipeline_remove_peer(peer_id)` — remove all recv streams for a peer

**Renderer callbacks:** `on_video_frame(peer_id, label, frame)`, `on_audio_samples(peer_id, label, audio)`

### Test Utilities (`test_pattern.c`, `test_tone.c`)

Generate synthetic media when no capture device is available:
- **Test pattern:** color bars or gradient I420 frames
- **Test tone:** 440Hz sine wave PCM samples

## Dependencies

- **libvpx** — VP8 encode/decode
- **libopus** — Opus encode/decode
- **libmrtc** — rtc core (rtc_rtp_sender_send, rtc_types, rtc_rtp)
- **libmrtc_common** — types, logging

## Tests

| Test | Coverage |
|------|----------|
| `test_vp8` | VP8 RTP packetization/depacketization, fragmentation, round-trip |
| `test_video_codec` | Encoder/decoder creation, encode/decode round-trip |
| `test_audio_codec` | Opus encode/decode, sample rate handling |
| `test_jitter_buffer` | Packet reordering, delay compensation, drop logic |
| `test_rate_control` | AIMD algorithm, keyframe requests |
| `test_media_pipeline` | Stream handles, send fan-out, recv routing, peer add/remove |
