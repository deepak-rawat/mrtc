# mrtc — Roadmap & Design Plan

## Design Philosophy

mrtc follows the **Pion model**: the `rtc` core is codec-agnostic and operates
at the RTP payload level. Senders take encoded payloads. Receivers deliver raw RTP
payloads. The `media` library is a separate layer that provides codecs, packetizers,
and pipeline orchestration — it depends on `rtc` and uses its public types directly.

```
┌───────────────────────────────────────────────────────────┐
│  Application / Conference                                 │
├───────────────────────────────────────────────────────────┤
│  media (depends on rtc)                                   │
│    ├─ Codecs: VP8 (libvpx), Opus (libopus)                │
│    ├─ VP8 packetizer/depacketizer (RFC 7741)              │
│    ├─ Jitter buffer, rate control                         │
│    └─ media_pipeline: encode-once fan-out + recv decode   │
│         holds rtc_rtp_sender_t* directly (typed)          │
├───────────────────────────────────────────────────────────┤
│  rtc (core, codec-agnostic)                               │
│    ├─ RTCPeerConnection, RTCRtpSender/Receiver            │
│    ├─ ICE, DTLS 1.2, SRTP (AES-128-CM)                   │
│    ├─ RTCP SR/RR, SDP, Data Channels                     │
│    └─ STUN/TURN client                                    │
├───────────────────────────────────────────────────────────┤
│  signaling (WebSocket client + server)                    │
├───────────────────────────────────────────────────────────┤
│  common (platform abstractions, types, logging)           │
└───────────────────────────────────────────────────────────┘
```

---

## Completed Work

**Core RTC** — ICE (host + SRFLX candidates), DTLS 1.2 (OpenSSL), SRTP,
RTP/RTCP SR/RR, SDP (multi-media), data channels, peer connection with
automatic ICE→DTLS→SRTP on both descriptions set. STUN binding, TURN client
(allocate, channel bind, channel data).

**Media** — VP8 encode/decode (libvpx), Opus encode/decode (libopus),
VP8 RTP packetizer/depacketizer (RFC 7741), jitter buffer, AIMD rate control,
multi-stream media pipeline with encode-once fan-out to N peers via typed
`rtc_rtp_sender_t*`, opaque stream handles for O(1) dispatch, lazy recv stream
registration.

**Signaling** — WebSocket client/server (libwebsockets + cJSON), JSON protocol,
meeting rooms, peer discovery, offer/answer/candidate relay.

**TURN Server** — Minimal UDP TURN server for testing (allocate, refresh,
create permission, channel bind/data, long-term credentials).

**Conference** — High-level API orchestrating rtc + media + signaling.
Push raw frames, receive decoded frames via callbacks. Automatic peer
connection lifecycle.

**Application** — SDL3 video conference app with camera capture, mic input,
tiled video grid, keyboard controls.

**Tests** — 18 test executables, 101 tests covering all components.

---

## Known Limitations

| Area | Limitation |
|---|---|
| Atomics | `volatile` used instead of `_Atomic` — no real memory ordering |
| RTCP | SR/RR integrated with periodic send, SRTCP, rate control feedback — no NACK, PLI, FIR, REMB |
| Bandwidth | AIMD rate controller driven by RTCP RR — no delay-based BWE |
| FEC | No forward error correction |
| ICE | No trickle ICE, no relay candidates, always controlling role |
| Stats | No `getStats()` API |
| Pipeline | Send/recv share one bloated struct with unused fields |
| SDP | `rtc_sdp.h` is public but only `rtc_peer.h` needs two types from it |
| Codec negotiation | Conference hardcodes VP8/Opus |
| Mute | Only at conference level, not at sender level |
| `on_track` | Passes `rtc_rtp_receiver_t*`, should pass `rtc_rtp_transceiver_t*` |

---

## Implementation Plan

### Phase 1: C11 Atomics + Design Cleanup

**1.1 — Replace `volatile` with `_Atomic` (`<stdatomic.h>`)**

Current `rtc_peer.c` uses `volatile` for cross-thread state flags. This provides
no memory ordering guarantees. C11 `_Atomic` is widely supported (GCC, Clang, MSVC
17+) and gives proper acquire/release semantics.

```c
// Before:
volatile rtc_connection_state_t connection_state;
// After:
_Atomic rtc_connection_state_t connection_state;
```

Keep `rtc_thread_t`, `rtc_mutex_t`, `rtc_cond_t` platform wrappers — `<threads.h>`
is optional in C11 and MSVC doesn't implement it.

**1.2 — Separate send/recv stream structs in media_pipeline**

Currently one `struct media_send_stream` with encoder fields (video_enc, audio_enc,
audio_out) and decoder fields (video_dec, audio_dec, vp8_depack, jitter_buffer)
that are never both used. Split into:

```c
typedef struct media_send_stream { /* encoder + label */ } media_send_stream_t;
typedef struct media_recv_stream { /* decoder + jb + depack */ } media_recv_stream_t;
```

Saves ~270KB per unused side per stream (VP8 depacketizer reassembly buffer alone
is 256KB).

**1.3 — Make `rtc_sdp.h` private**

`rtc_peer.h` only needs `SDP_MAX_SIZE` and `rtc_sdp_type_t`. Move those into
`rtc_peer.h`, move `rtc_sdp.h` to `rtc/src/`.

---

### Phase 2: RTCP Feedback (NACK, PLI, REMB)

**2.1 — NACK/PLI/FIR/REMB build + parse**

Extend `rtc_rtcp.c`:
```c
int rtc_rtcp_build_nack(buf, len, sender_ssrc, media_ssrc, lost_seqs, count);
int rtc_rtcp_build_pli(buf, len, sender_ssrc, media_ssrc);
int rtc_rtcp_build_fir(buf, len, sender_ssrc, media_ssrc, seq_nr);
int rtc_rtcp_build_remb(buf, len, sender_ssrc, media_ssrcs, count, bitrate_bps);
```

**2.2 — NACK buffer**

Circular buffer in sender indexed by sequence number (512 packets). On NACK
received, look up and retransmit via SRTP + transport.

```c
rtc_nack_buf_t *rtc_nack_buf_create(int max_packets);
void rtc_nack_buf_store(buf, pkt, len, seq);
const uint8_t *rtc_nack_buf_get(buf, seq, out_len);
```

**2.3 — Sender callbacks**

```c
typedef void (*rtc_on_pli_fn)(void *user);
typedef void (*rtc_on_remb_fn)(uint32_t bitrate_bps, void *user);

void rtc_rtp_sender_on_pli(sender, fn, user);
void rtc_rtp_sender_on_remb(sender, fn, user);
```

Media pipeline wires: `on_pli` → `video_encoder_request_keyframe()`,
`on_remb` → `video_encoder_set_bitrate()`.

**2.4 — RTCP dispatch in peer connection**

Wire `peer_transport_recv` to parse RTCP packet type and dispatch to matching
sender/receiver callbacks.

---

### Phase 3: Bandwidth Estimation

**3.1 — RTP header extensions**

```c
int rtc_rtp_ext_write(buf, len, id, data, data_len);
int rtc_rtp_ext_parse(ext_data, ext_len, id, out, out_len);
```

Key extensions: `abs-send-time`, `transport-cc`, `audio-level`, `mid`.
SDP `a=extmap:` negotiation in `rtc_sdp.c`.

**3.2 — Transport-CC (RFC 8888)**

Sender records per-packet send times. Receiver tracks arrival times, builds
RTCP Transport Feedback periodically. Sender compares send/recv deltas.

**3.3 — GCC (Google Congestion Control)**

Delay-based + loss-based estimator. Fires callback when estimate changes.
Pipeline adjusts encoder bitrate.

```c
rtc_bwe_t *rtc_bwe_create(initial_bitrate_bps);
void rtc_bwe_on_delay_feedback(bwe, send_delta, recv_delta, pkt_size);
void rtc_bwe_on_loss(bwe, fraction_lost);
uint32_t rtc_bwe_get_bitrate(bwe);
```

---

### Phase 4: Forward Error Correction

UlpFEC (RFC 5109) — XOR-based, used by Chrome for VP8.

```c
int rtc_fec_encode(encoder, rtp_pkt, len, fec_pkt, fec_len);
int rtc_fec_decode(decoder, pkt, len, on_recovered, user);
```

Send: `payload → RTP → FEC encode → SRTP → transport`
Recv: `transport → SRTP → FEC decode → receiver callback`

SDP negotiation: `a=rtpmap:XX ulpfec/90000`.

---

### Phase 5: Stats + API Cleanup

**5.1 — Stats API**

```c
typedef struct {
    uint64_t packets_sent, bytes_sent;
    uint32_t nack_count, pli_count, fir_count;
    uint32_t target_bitrate, retransmitted_packets;
} rtc_sender_stats_t;

typedef struct {
    uint64_t packets_received, bytes_received, packets_lost;
    uint32_t jitter, nack_count, pli_count;
} rtc_receiver_stats_t;

int rtc_rtp_sender_get_stats(sender, out);
int rtc_rtp_receiver_get_stats(receiver, out);
```

**5.2 — API improvements**

- `on_track` passes `rtc_rtp_transceiver_t*` instead of just receiver
- `rtc_rtp_sender_set_enabled()` for mute at sender level
- Codec negotiation from SDP (remove hardcoded VP8/Opus in conference)
- `rtc_rtp_sender_replace_track()` for camera → screen share switch

---

### Phase 6: ICE Improvements

- ICE trickle — send candidates as gathered instead of all-in-SDP
- TURN relay candidate integration into ICE gathering
- ICE-controlled role support (currently always controlling)

---

### Phase 7: Application Features

- Screen sharing (second video track via platform capture API)
- Noise suppression (RNNoise, single-header C library)
- Echo cancellation (SpeexDSP AEC)
- File transfer over data channels (chunked binary)

---

### Phase 8: Scale

- SFU server (selective forwarding unit) — enables 10+ participants
- Simulcast (multi-layer encode, SDP `a=simulcast:` + `a=rid:`)
- Server-side recording (WebM mux of VP8+Opus)

---

### Integration Tests (parallel with above)

| Test | Description |
|------|-------------|
| `test_loopback` | Two peer connections in one process, VP8 frame exchange |
| `test_turn_relay` | Two ICE agents relay through in-process TURN server |
| `test_signaling_e2e` | Two signaling clients exchange offer/answer through server |
| `test_peer_join_leave` | 3 peers join meeting, one leaves, verify remaining connected |

---

## Dependencies

| Library | License | Used by |
|---|---|---|
| OpenSSL 3.0+ | Apache-2.0 | rtc (DTLS, SRTP, HMAC) |
| libvpx | BSD | media (VP8) |
| libopus | BSD | media (Opus) |
| libwebsockets | MIT | signaling |
| cJSON | MIT | signaling |
| SDL3 | zlib | app |
