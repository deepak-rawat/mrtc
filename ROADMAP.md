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
│    ├─ Runtime transport: worker/listener/router/transport │
│    ├─ Client API: RTCPeerConnection, sender/receiver      │
│    ├─ SFU API: router, logical transport, producer/consumer│
│    ├─ ICE/STUN checks, DTLS 1.2, SRTP (AES-128-CM)        │
│    └─ RTCP SR/RR, SDP, Data Channels, TURN client         │
├───────────────────────────────────────────────────────────┤
│  signaling (WebSocket client + server)                    │
├───────────────────────────────────────────────────────────┤
│  common (platform abstractions, types, logging)           │
└───────────────────────────────────────────────────────────┘
```

---

## Completed Work

**Core RTC** — ICE (host + SRFLX candidates), DTLS 1.2 (OpenSSL), SRTP/SRTCP,
RTP/RTCP SR/RR, RTP header extensions (RFC 8285 one-byte form), SDP (multi-media
with `a=extmap:` negotiation), data channels (wire protocol with OPEN/ACK
handshake, up to 16 concurrent channels), peer connection with automatic
ICE→DTLS→SRTP on both descriptions set. The peer facade and SFU APIs share the
same always-built runtime transport core: worker, shared UDP listener, router,
logical transport, producers, and consumers. STUN binding, TURN client (allocate,
channel bind, channel data). Per-sender AIMD rate control driven by RTCP RR.
RTCP feedback (NACK / PLI / FIR) with per-video NACK retransmit buffer.
Transport-Wide Congestion Control end-to-end (draft-holmer-rmcat-transport-wide-cc):
sender tagging, periodic feedback, parser. Per-peer Google Congestion Control
bandwidth estimator (delay trendline + loss controller) exposed via
`rtc_peer_connection_on_bitrate_estimate()`. Trickle candidate ingestion through
`rtc_peer_connection_add_ice_candidate()` is implemented.

**Media** — VP8 encode/decode (libvpx), Opus encode/decode (libopus),
VP8 RTP packetizer/depacketizer (RFC 7741), jitter buffer, AIMD rate control,
multi-stream media pipeline with encode-once fan-out to N peers via typed
`rtc_rtp_sender_t*`, opaque stream handles for O(1) dispatch, lazy recv stream
registration. Video debug utilities (IVF dump, frame checksum, PSNR), RTP
sequence gap tracking.

**Signaling** — WebSocket client/server (libwebsockets + cJSON), JSON protocol,
meeting rooms, peer discovery, offer/answer/candidate relay.

**Conference** — High-level API orchestrating rtc + media + signaling.
Push raw frames, receive decoded frames via callbacks. Automatic peer
connection lifecycle.

**Application** — SDL3 video conference app with camera capture, mic input,
tiled video grid, keyboard controls. Chat app (terminal) for text messaging
over data channels.

**Tests** — 40 test executables covering protocol primitives, runtime transport,
client peer connections, SFU media forwarding, media, signaling, and apps.

---

## Implementation Plan

### Phase 1: C11 Atomics + Design Cleanup — ✅ COMPLETE

_See git log: `bc399a3` (1.1), `d2d4111` (1.3), `2ff83b7` (1.2), `8a9a243` + follow-up (1.4)._

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

**1.4 — O(1) SSRC→receiver demux**

Current peer connection scans the receiver list linearly on every inbound RTP
packet. Replace with a hashmap keyed by SSRC (use `rtc_u32_map` from `common`):

```c
rtc_u32_map_t ssrc_to_receiver;   // SSRC → rtc_rtp_receiver_t*
rtc_u32_map_t ssrc_to_sender;     // SSRC → rtc_rtp_sender_t* (for RTCP RR demux)
```

Populate when transceivers are created / remote SSRCs are learned from SDP.
Required prerequisite for Phase 2.5 (per-sender RR demux).

---

### Phase 2: RTCP Feedback (NACK, PLI, FIR) — ✅ COMPLETE

**2.1 — NACK/PLI/FIR build + parse**

Extend `rtc_rtcp.c`:
```c
int rtc_rtcp_build_nack(buf, len, sender_ssrc, media_ssrc, lost_seqs, count);
int rtc_rtcp_build_pli(buf, len, sender_ssrc, media_ssrc);
int rtc_rtcp_build_fir(buf, len, sender_ssrc, media_ssrc, seq_nr);
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

void rtc_rtp_sender_on_pli(sender, fn, user);
```

Media pipeline wires: `on_pli` → `video_encoder_request_keyframe()`.

**2.4 — RTCP dispatch in peer connection**

Runtime logical transports deliver plaintext RTCP to `peer_handle_plain_rtcp`,
which parses RTCP packet type and dispatches to matching sender/receiver
callbacks.

**2.5 — Per-sender rate control + RR demux**

Today a single `rtc_rate_control_t` is shared by all video senders and the media
pipeline only consumes feedback from the first peer. Move the rate controller
*into* each `rtc_rtp_sender_t`:

```c
struct rtc_rtp_sender_t {
    uint32_t ssrc;
    rtc_rate_control_t rate_ctrl;   // per-sender, not shared
    rtc_nack_buf_t *nack_buf;
    ...
};
```

When a Receiver Report arrives, use the SSRC→sender hashmap from 1.4 to feed
loss / jitter / RTT into the correct sender's rate controller. Media pipeline
queries each sender's target bitrate independently and encodes per peer
(or picks min across peers for shared encoder).

---

### Phase 3: Bandwidth Estimation — ✅ COMPLETE

**3.1 — RTP header extensions (RFC 8285)**

`rtc_rtp_ext.{h,c}` — one-byte form write/parse, helpers for abs-send-time and
transport-cc. `rtc_rtp_build_with_ext()` / `rtc_rtp_session_send_with_ext()` emit
the extension block and set the X bit. SDP `a=extmap:` negotiation in
`rtc_sdp.c` with `rtc_sdp_media_add_extmap()` / `rtc_sdp_media_find_extmap_id()`.

**3.2 — Transport-CC (draft-holmer-rmcat-transport-wide-cc-extensions)**

`rtc_twcc_sender.{h,c}` — 1024-entry seq ring of `(twcc_seq, send_time_us, wire_size)`.
`rtc_twcc_receiver.{h,c}` — 256-packet arrival window; builds RTCP PT=205 FMT=15
feedback using run-length + status-vector chunks and 1/2-byte 250µs deltas.
Peer connection auto-emits `a=extmap:5 <transport-cc URI>` on audio/video, tags
every outgoing RTP with the extension, and runs a 100ms feedback timer.

**3.3 — GCC (Google Congestion Control)**

`rtc_bwe.{h,c}` — simplified delay-based + loss-based estimator:
group packets into 5ms bursts → trendline filter over a 20-sample window →
adaptive overuse threshold → `{NORMAL, OVERUSE, UNDERUSE}` state drives a rate
controller (multiplicative/additive increase, 0.85 × recent-throughput decrease).
Loss controller folds RR `fraction_lost` into the same target. Per-peer BWE
consumes parsed TWCC feedback in `rtc_peer.c` and fires
`rtc_peer_connection_on_bitrate_estimate()`.

```c
typedef void (*rtc_on_bitrate_estimate_fn)(uint32_t bitrate_bps, void *user);
void rtc_peer_connection_on_bitrate_estimate(rtc_peer_connection_t *pc,
                                             rtc_on_bitrate_estimate_fn fn, void *user);
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

- ICE gathering is still synchronous for local candidates; remote trickle
    candidate ingestion is implemented through `rtc_peer_connection_add_ice_candidate()`
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

- Full SFU server application on top of the runtime router/producer/consumer APIs
    — enables 10+ participants
- Simulcast (multi-layer encode, SDP `a=simulcast:` + `a=rid:`)
- Server-side recording (WebM mux of VP8+Opus)

---

### Integration Tests (parallel with above)

| Test | Description |
|------|-------------|
| `test_loopback` | Two peer connections in one process, VP8 frame exchange |
| `test_turn_relay` | Two ICE agents relay through an external TURN service such as coturn |
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
