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
RTP/RTCP SR/RR, SDP (multi-media), data channels (wire protocol with
OPEN/ACK handshake, up to 16 concurrent channels), peer connection with
automatic ICE→DTLS→SRTP on both descriptions set. STUN binding, TURN client
(allocate, channel bind, channel data). AIMD rate control driven by RTCP RR.

**Media** — VP8 encode/decode (libvpx), Opus encode/decode (libopus),
VP8 RTP packetizer/depacketizer (RFC 7741), jitter buffer, AIMD rate control,
multi-stream media pipeline with encode-once fan-out to N peers via typed
`rtc_rtp_sender_t*`, opaque stream handles for O(1) dispatch, lazy recv stream
registration. Video debug utilities (IVF dump, frame checksum, PSNR), RTP
sequence gap tracking.

**Signaling** — WebSocket client/server (libwebsockets + cJSON), JSON protocol,
meeting rooms, peer discovery, offer/answer/candidate relay.

**TURN Server** — Minimal UDP TURN server for testing (allocate, refresh,
create permission, channel bind/data, long-term credentials).

**Conference** — High-level API orchestrating rtc + media + signaling.
Push raw frames, receive decoded frames via callbacks. Automatic peer
connection lifecycle.

**Application** — SDL3 video conference app with camera capture, mic input,
tiled video grid, keyboard controls. Chat app (terminal) for text messaging
over data channels.

**Tests** — 23 test executables, 140 tests covering all components.

---

## Known Bugs (by severity)

### High

| Bug | Location | Description |
|---|---|---|
| SRTP no buffer size check | `rtc_srtp.c` | `protect` appends auth tag without verifying buffer capacity. API lacks `buflen` parameter. |
| ICE/transport socket race | `rtc_ice.c` `rtc_ice_connect` | Synchronous `recvfrom()` on same socket transport thread polls. Either thread steals packets. |
| Rate controller data race | `rtc_rate_control.c` | Written on transport thread, read on main thread, no mutex. Torn reads on multi-word fields. |
| CRC32 table init race | `rtc_stun.c` | Global `crc32_table[]` first-init with no synchronization. Two threads → corrupted table → broken STUN FINGERPRINT. |
| No SRTP replay protection | `rtc_srtp.c` | RFC 3711 requires replay list. Captured packets can be replayed. |
| Destroy without close → UAF | `rtc_peer.c` `destroy` | `free(pc)` while transport thread still running → use-after-free. |
| STUN IPv6 OOB read | `rtc_stun.c` | Checks `alen >= 8` but IPv6 mapped address needs 20 bytes. Reads 12 bytes past buffer. |

### Medium

| Bug | Location | Description |
|---|---|---|
| Relay candidates labeled "host" | `rtc_sdp.c` | Missing `ICE_CANDIDATE_RELAY` case → defaults to "host". TURN relay broken with compliant peers. |
| `select()` unsafe for fd ≥ 1024 | `rtc_ice.c` / `rtc_stun.c` | `FD_SET` on high fds overflows `fd_set` stack buffer on Linux. |
| Signaling config shallow copy | `signaling_client.c` | String pointers from stack config become dangling after caller returns. |
| Jitter buffer recursive pop | `jitter_buffer.c` | Up to 64 recursion levels skipping lost packets. Stack overflow risk. |
| VP8 2-byte PictureID not handled | `vp8_packetizer.c` | RFC 7741 M-bit extension ignored. Breaks interop with extended PictureID peers. |
| DTLS app data buffer too small | `rtc_peer.c` | 2048-byte `SSL_read` buffer vs 65535 max data channel message size. Large messages fragmented mid-message. |
| TURN nonce never rotated | `turn_handler.c` | Single nonce for server lifetime. Enables replay attacks. |
| RTCP loss calc ignores initial seq | `rtc_rtcp.c` | `packets_expected = highest_seq + 1` assumes seq started at 0. Wrong loss stats → wrong rate control. |

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
