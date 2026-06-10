# Core RTC Runtime Library (libmrtc)

`libmrtc` is the runtime transport core plus the protocol primitives:
worker, shared UDP listener, logical transport, STUN / ICE, DTLS, SRTP,
RTP / RTCP, SDP, NACK buffer, TWCC, BWE. It is always built and is the
common base for both client peer connections and server-side routing.

The WebRTC-style peer-connection facade lives in
[`libmrtc_client`](../client/ARCHITECTURE.md). Server-side router / producer /
consumer primitives live in [`libmrtc_routing`](../routing/ARCHITECTURE.md).

Follows the **Pion model**: codec-agnostic at the RTP payload level.
Senders take already-encoded payloads. Receivers deliver raw RTP
payloads. Encoding / decoding is handled by the optional `media` library
on top.

## Usage Example (driving the runtime directly)

This is direct use of the runtime primitives, no peer-connection facade and no
routing graph. Client applications should use the higher-level
[`libmrtc_client`](../client/ARCHITECTURE.md); server routing should use
[`libmrtc_routing`](../routing/ARCHITECTURE.md).

```c
rtc_init();

// ── Process-wide runtime: one worker, one shared UDP listener ──
rtc_worker_t   *worker   = rtc_worker_create(NULL);
rtc_listener_t *listener = rtc_listener_create(worker, NULL);
// ── One logical transport per remote endpoint ──
rtc_transport_t *transport = rtc_transport_create(worker,
    &(rtc_transport_config_t){
        .listener = listener,
        .ice_mode = RTC_ICE_MODE_LITE,
    });

// ── Pull local ICE creds + DTLS fingerprint, ship in your SDP answer ──
rtc_ice_parameters_t ice;
rtc_dtls_parameters_t dtls;
rtc_transport_get_ice_parameters(transport, &ice);
rtc_transport_get_dtls_parameters(transport, &dtls);

// ── Feed in the remote side from your signaling layer ──
rtc_transport_set_remote_ice_parameters(transport, &remote_ice);
rtc_transport_add_remote_candidate(transport, &remote_candidate);
rtc_transport_start_dtls(transport);

// ── Tear down ──
rtc_transport_destroy(transport);
rtc_listener_destroy(listener);
rtc_worker_destroy(worker);
rtc_cleanup();
```

## Protocol Stack

```
┌────────────────────────────────────┐
│ Runtime Transport Core             │  worker, listener, transport
├────────────────────────────────────┤
│  SDP │ RTP │ SRTP                  │  rtc_sdp, rtc_rtp, rtc_srtp
├────────────────────────────────────┤
│  DTLS (OpenSSL)                    │  rtc_dtls.h/c
├────────────────────────────────────┤
│  ICE/STUN Checks                   │  rtc_stun.h/c, rtc_transport.c
├────────────────────────────────────┤
│  STUN Client / TURN Client         │  rtc_stun.h/c, rtc_turn.h/c
├────────────────────────────────────┤
│  Packet I/O (UDP + Threading)      │  rtc_packet_io.h/c
├────────────────────────────────────┤
│  Platform Poller (epoll/kqueue)    │  rtc_poller.h/c
└────────────────────────────────────┘
```

Public headers live in `include/rtc/` and install under `include/rtc/`
unconditionally:

- `rtc_common.h` — error codes, log macros, platform abstractions
- `rtc.h` — library lifecycle (`rtc_init`/`rtc_cleanup`)
- `rtc_worker.h`, `rtc_listener.h`, `rtc_transport.h` — runtime transport primitives
- `rtc_rtp.h`, `rtc_rtcp.h`, `rtc_sdp.h`, `rtc_rtp_ext.h`, `rtc_rtp_params.h` —
  protocol helpers and signaling parameter types
- `rtc_nack_buf.h`, `rtc_rate_control.h`, `rtc_twcc_*.h`, `rtc_bwe.h` — RTP control
  primitives shared by client and routing layers

Internal / private helpers (`rtc_ice.h`, `rtc_dtls.h`, `rtc_srtp.h`,
`rtc_stun.h`, `rtc_turn.h`,
`rtc_packet_io.h`, `rtc_poller.h`, `rtc_timer_sched.h`) live in `src/`.

## Module Details

### Runtime Transport Core

The runtime core is the shared substrate used by both client and routing
consumers.

- **`rtc_worker_t`** — timer / scheduling shard used by logical
  transports.
- **`rtc_listener_t`** — owns a UDP packet I/O socket and demux maps.
  One listener can serve many logical transports on the same local port.
- **`rtc_transport_t`** — one logical ICE / DTLS / SRTP endpoint over a
  shared listener. Handles ICE-lite responses, full ICE checks, DTLS,
  SRTP / SRTCP, RTP / RTCP callbacks, and DTLS application data.

The client `rtc_peer_connection_t` (in `libmrtc_client`) creates a
private worker / listener / transport internally and exposes a
spec-shaped API on top. Server-side media routing uses `libmrtc_routing`
to add routers, producers, consumers, and SSRC dispatch on top of core
transports.

### Packet I/O (`rtc_packet_io.h/c`)

Owns a UDP socket and runs a background thread with a platform I/O
poller. It is a low-level primitive used by `rtc_listener_t`, not a
peer-connection transport by itself.

- **Socket:** single UDP socket, bound to an ephemeral port
- **I/O Poller:** `rtc_poller_t` — epoll (Linux), kqueue (macOS),
  select (Windows)
- **Background thread:** runs the poller loop, fires timers, dispatches
  packets
- **Packet demux:** classifies incoming packets per RFC 7983 by first
  byte — STUN (0-3), DTLS (20-63), RTP/RTCP (128-191), TURN ChannelData
  (64-79)
- **Timers:** dynamic scheduler-backed callbacks; runtime transports use
  worker timers for ICE / DTLS / RTCP flows

Packet flow:
```
Recv:  UDP socket → demux (RFC 7983) → callback(type, data, from_addr)
Send:  rtc_packet_io_send(data, dest) → UDP sendto (thread-safe)
```

Key functions: `rtc_packet_io_init_ex()`, `rtc_packet_io_send()`,
`rtc_packet_io_get_local_addr()`.

### ICE / STUN

The runtime transport implements the active logical ICE path. It
registers listener demux routes by local ufrag, STUN transaction ID, and
selected remote tuple.

The older `rtc_ice.h/c` agent remains as a standalone protocol component
and test target; the active runtime transport does not own an
`rtc_ice_agent_t`.

**Candidate types:**
| Type | Gathering method |
|------|-----------------|
| HOST | Interface enumeration (getifaddrs / GetAdaptersAddresses) |
| SRFLX | STUN Binding Request to public server |
| RELAY | TURN allocation (not yet wired into gathering) |

**Algorithm:**
1. Listener exposes local host candidates and transport ICE credentials.
2. Remote SDP / trickle candidates populate the logical transport.
3. Full ICE transports send Binding Requests; ICE-lite transports answer
   Binding Requests.
4. First successful tuple becomes the selected remote tuple and routes
   DTLS / RTP / RTCP.

### STUN / TURN (`src/rtc_stun.h/c`, `src/rtc_turn.h/c`)

**STUN (RFC 5389):**
- Binding Request / Response with MESSAGE-INTEGRITY (HMAC-SHA1) +
  FINGERPRINT
- Used by ICE for server-reflexive candidate discovery and connectivity
  checks

**TURN (RFC 5766):**
- Allocate, Refresh, CreatePermission, ChannelBind methods
- Long-term credentials (MD5 + realm + nonce)
- ChannelData framing for peer communication
- Partially implemented — not yet wired into ICE gathering

### DTLS 1.2 (`rtc_dtls.h/c`)

Implements TLS-over-UDP handshake and SRTP key export.

- OpenSSL 3.0+ EVP API (modern crypto, no deprecated functions)
- Memory BIO mode: SSL reads / writes to memory buffers, app controls
  UDP transmission
- Self-signed certificates generated at init, SHA-256 fingerprint
  matching
- SRTP key export via `SRTP_PROTECTION_PROFILE` after handshake

States: `NEW` → `CONNECTING` → `CONNECTED` → `FAILED` / `CLOSED`

Handshake flow:
```
Active side:   dtls_handshake() → SSL_do_handshake() → wBIO → send via UDP
Passive side:  receive DTLS → feed to rBIO → SSL processes → may write to wBIO
After:         dtls_export_srtp_keys() → client/server keys + salts
```

### SRTP (`rtc_srtp.h/c`)

AES-128-CM encryption with HMAC-SHA1-80 authentication (RFC 3711).

- Master key / salt derived from DTLS export
- Per-packet: derive session key via PRF, encrypt payload, append auth
  tag
- Rollover counter (ROC) tracks sequence number wrap-around

**Thread safety.** `rtc_srtp_protect` / `rtc_srtp_unprotect` /
`rtc_srtp_protect_rtcp` / `rtc_srtp_unprotect_rtcp` serialize on a
per-context mutex (`rtc_srtp_ctx_t::lock`). Runtime transports own
SRTP / SRTCP protection; RTP sends, RTCP SR / RR emission, TWCC
feedback, and NACK retransmits can touch the same context from
different runtime callbacks. Concurrent updates of `roc` / `last_seq`
/ `srtcp_index` would otherwise reuse the AES-CM keystream, which
breaks confidentiality. `init` and `close` are not thread-safe and
must not race with protect / unprotect calls.

Key functions: `rtc_srtp_init()`, `rtc_srtp_protect()`,
`rtc_srtp_unprotect()`.

### RTP / RTCP (`rtc_rtp.h/c`, `rtc_rtcp.h/c`, `rtc_rtp_ext.h/c`)

**RTP (RFC 3550):** 12-byte header + payload. Session tracking (SSRC,
seq, timestamp). Marker bit support. `rtc_rtp_build_with_ext()` /
`rtc_rtp_session_send_with_ext()` emit a one-byte-form header extension
block (X bit) alongside the payload; parse exposes `pkt.ext_data` /
`pkt.ext_len` for downstream consumers.

**RTP header extensions (RFC 8285):** `rtc_rtp_ext.h/c` implements the
one-byte form (ids 1–14, lengths 1–16). Helpers exist for
`abs-send-time` (24-bit 6.18 fixed-point) and `transport-cc` (16-bit
transport-wide seq).

**RTCP:** Sender Reports (SR) with NTP timestamps, Receiver Reports (RR)
with jitter / loss / delay stats. Feedback messages: NACK / PLI / FIR
(RFC 4585 + 5104) and Transport-CC (PT=205 FMT=15,
draft-holmer-rmcat-transport-wide-cc) build + parse. SRTCP protect /
unprotect for all outgoing / incoming RTCP. The client peer connection
in `libmrtc_client` schedules a 5 s periodic timer for SR / RR per
active transceiver and a 100 ms timer for TWCC feedback when negotiated.

Statistics tracked:
```
Send: packets_sent, octets_sent, rtp_timestamp
Recv: packets_received, jitter, last_transit, last_sr_ntp
```

### SDP (`rtc_sdp.h/c`)

Offer / Answer generation and parsing (RFC 4566 WebRTC subset).

- Multi-media: audio + video + application (data channel) m= lines
- ICE credentials (ufrag, pwd), DTLS fingerprint + setup role
- Codec parameters (payload type, clock rate, channels)
- Candidate lines as SDP attributes
- `a=extmap:<id> <uri>` (RFC 8285) emit + parse with
  `rtc_sdp_media_add_extmap()` /
  `rtc_sdp_media_find_extmap_id()`. The client peer connection
  auto-advertises transport-cc (id=5) on every audio / video m-section.

### Rate Control (`rtc_rate_control.h/c`)

AIMD (Additive Increase, Multiplicative Decrease) bitrate adaptation
driven by RTCP Receiver Reports. The algorithm lives here; the client
peer connection wires one controller into each `rtc_rtp_sender_t` so
audio and video adapt independently.

- Loss < 2% → increase bitrate 5%
- Loss > 5% → decrease bitrate 20%
- RTT > 300 ms → decrease bitrate 10%
- Loss > 10% → request keyframe
- Clamps to configurable `[min, max]` bitrate bounds

Key functions: `rtc_rate_control_create()`,
`rtc_rate_control_on_rtcp_rr()`, `rtc_rate_control_get_bitrate()`,
`rtc_rate_control_should_keyframe()`.

### NACK retransmit buffer (`rtc_nack_buf.h/c`)

512-packet ring buffer indexed by RTP sequence number. Stores post-SRTP
packets so an incoming Generic NACK (RFC 4585 §6.2.1) can be served by
re-sending the original wire packet through `rtc_transport_send_raw()`
without re-encrypting. The client peer connection allocates one per
video sender on connect.

### Transport-Wide CC (`rtc_twcc_sender.h/c`, `rtc_twcc_receiver.h/c`)

Wire-level implementation of
`draft-holmer-rmcat-transport-wide-cc-extensions`.

- **Sender ring** — 1024 entries of
  `(twcc_seq, send_time_us, wire_size)` keyed by `twcc_seq & (RING-1)`.
  When the client peer's `rtc_rtp_sender_send()` runs it assigns the
  next seq, writes the transport-cc extension into the RTP header, then
  records the post-SRTP wire size + send time after protect.
- **Receiver window** — 256 entries; `rtc_twcc_receiver_on_packet()`
  records arrivals. The 100 ms feedback timer in the peer connection
  calls `rtc_twcc_receiver_build_feedback()` which emits an RTCP
  PT=205/FMT=15 packet using run-length and 14×1-bit status-vector
  chunks plus 1- or 2-byte 250 µs receive deltas. The packet is then
  SRTCP-protected and sent.
- **Parse** — `rtc_rtcp_parse_twcc()` walks chunks and reconstructs
  absolute arrival time per packet (relative to the 24-bit reference
  time × 64 ms).

### Bandwidth Estimator (`rtc_bwe.h/c`)

Simplified Google Congestion Control (draft-ietf-rmcat-gcc):

- Group packets by send time into ≤5 ms bursts.
- Inter-group delay = `recv_delta − send_delta`, accumulated.
- Trendline filter (linear regression over a 20-sample sliding window)
  → slope.
- Adaptive threshold drives `{NORMAL, OVERUSE, UNDERUSE}`.
- Rate controller: NORMAL → multiplicative (×1.08) or additive
  (+50 kbps) increase; OVERUSE → decrease to 0.85 × recent throughput;
  UNDERUSE → hold.
- Loss controller (folded in via `rtc_bwe_on_loss`): <2% → +5%, 2–10%
  hold, >10% → ×(1 − 0.5·loss).
- Final target = `min(delay_estimate, loss_estimate)` clamped to
  `[min_bps, max_bps]`.
- Callback fires on >3% change or 1 s elapsed.

The client peer connection owns one `rtc_bwe_t` per peer when
transport-cc is negotiated and surfaces the result via
`rtc_peer_connection_on_bitrate_estimate()`.

## Threading Model

- **Main thread:** API calls into runtime primitives, frame capture, UI
  rendering.
- **Listener packet I/O thread:** socket I/O, RFC 7983 demux, listener
  route dispatch.
- **Worker timers:** logical transport ICE checks, DTLS retransmission,
  application-owned RTCP / TWCC timers scheduled on the worker.

Packet I/O uses the dynamic timer scheduler for low-level callbacks.
Runtime transports use worker timers and listener route maps rather than
peer-owned packet I/O timers.

## Tests

| Test | Coverage |
|------|----------|
| `test_stun` | STUN Binding Req/Resp, MESSAGE-INTEGRITY, FINGERPRINT |
| `test_rtp` | RTP header parsing, session tracking, seq/ts increment |
| `test_rtp_ext` | RFC 8285 one-byte ext write/parse, build_with_ext round-trip |
| `test_srtp` | AES-CM encryption, HMAC-SHA1, wrong-key rejection |
| `test_dtls` | DTLS handshake via memory BIOs, SRTP key export |
| `test_ice` | Candidate gathering, connectivity checks, loopback e2e |
| `test_sdp` | SDP generation/parsing round-trip, extmap |
| `test_sdp_video` | Multi-media SDP (audio + video), codec params |
| `test_packet_io` | Socket init, packet demux, timer management |
| `test_poller` | Platform poller (epoll/kqueue/select) behavior |
| `test_timer_sched` | Dynamic timer scheduler |
| `test_rtcp` | SR/RR build/parse, jitter statistics |
| `test_rtcp_feedback` | NACK / PLI / FIR build + parse |
| `test_nack_buf` | NACK ring buffer store/lookup, wraparound |
| `test_twcc` | TWCC sender ring, receiver window, feedback round-trip |
| `test_bwe` | GCC steady increase, delay-induced decrease, loss clamp, callback |
| `test_turn` | TURN allocation, ChannelData framing, credentials |
| `test_rate_control` | AIMD algorithm, bitrate adaptation, keyframe requests |
| `test_worker` | Runtime worker lifecycle and timer scheduling |
| `test_listener` | Shared UDP listener candidates, demux, stats |
Routing tests (`test_routing_transport`, `test_routing_ice`,
`test_routing_media`, `test_routing_timers`) live in
[`routing/tests/`](../routing/ARCHITECTURE.md#tests).

Client-facade tests (`test_peer`, `test_data_channel`,
`test_rtp_sender_loopback`) live in
[`client/tests/`](../client/ARCHITECTURE.md#tests).

## Known Limitations

- Local candidates are still produced synchronously; remote trickle
  candidate ingestion is implemented.
- TURN relay candidates not yet wired into ICE gathering.
- Always ICE-controlling role on the active runtime transport (no
  nomination negotiation).
- No RTCP compound packets (single SR or RR per interval).
- No full SRTP / SRTCP replay hardening audit yet.
