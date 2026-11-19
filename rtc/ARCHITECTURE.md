# Core RTC Runtime Library (libmrtc)

`libmrtc` is the runtime transport core plus the protocol primitives:
worker, shared UDP listener, logical transport, STUN / ICE, DTLS, SRTP,
RTP / RTCP, RTP demux, RTCP router, SDP, NACK buffer, TWCC, BWE. It is
always built and is the common base for both client peer connections and
server-side routing.

The WebRTC-style peer-connection facade lives in
[`libmrtc_client`](../client/ARCHITECTURE.md). Server-side router / producer /
consumer primitives live in [`libmrtc_routing`](../routing/ARCHITECTURE.md).
The sibling `mrtc_sctp` target owns the shared usrsctp/DCEP implementation,
`rtc_sctp.h`, `rtc_data_channel.h`, and the internal data channel manager.
It is built when either client or routing APIs are enabled, so server-side
SCTP does not require `mrtc_client`. Runtime-only SCTP users call
`rtc_sctp_init()` / `rtc_sctp_cleanup()` around their endpoint lifetimes.
Room envelopes and topology policy remain above this transport layer.

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
rtc_transport_set_remote_dtls_parameters(transport, &remote_dtls);
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
- `rtc_rtp_stream.h` — composed RTP send/recv streams, shared by client and routing
- `rtc_interceptor.h` — composable RTCP interceptor chain (report / NACK / PLI
  built-ins + custom)
- `rtc_media_session.h` — per-peer RTP/RTCP media session (RTP routing, RTCP
  feedback, SR/RR), used by the client peer connection

Internal / private helpers (`rtc_ice.h`, `rtc_dtls.h`, `rtc_srtp.h`,
`rtc_stun.h`, `rtc_turn.h`, `rtc_rtp_demux.h`, `rtc_nack_buf.h`,
`rtc_rate_control.h`, `rtc_twcc_sender.h`, `rtc_twcc_receiver.h`, `rtc_bwe.h`,
`rtc_packet_io.h`, `rtc_poller.h`, `rtc_timer_sched.h`) live in `src/`. The
congestion-control primitives (NACK buffer, AIMD rate control, TWCC, BWE) are
encapsulated by the transport and the send/recv streams. Routing additionally
reuses the internal AIMD controller for downstream receiver-report estimates,
instead of duplicating its algorithm; this is a private build dependency, not
an additional public protocol API.

## Module Details

### Runtime Transport Core

The runtime core is the shared substrate used by both client and routing
consumers.

- **`rtc_worker_t`** — the single event-loop thread for a runtime shard.
  Owns the I/O poller, the dynamic timer scheduler, and a cross-thread
  task queue. It drains registered sockets, fires timers, and runs
  marshalled control / teardown work, so all per-transport protocol
  state (ICE, DTLS, SRTP) stays single-threaded without per-object locks.
- Internal stream users can register/update read/write interests on that same
  worker. Write readiness is disabled after draining pending writes so an idle
  connected stream does not spin the event loop; socket errors also dispatch.
- **`rtc_listener_t`** — owns a UDP packet I/O socket and demux maps.
  One listener can serve many logical transports on the same local port.
- **`rtc_transport_t`** — one logical ICE / DTLS / SRTP endpoint over a
  shared listener. Handles ICE-lite responses, full ICE checks, DTLS,
  SRTP / SRTCP, and DTLS application data. On the media plane it owns an
  **RTP demux router** (`rtc_rtp_demux_t`): consumers bind SSRC → stream
  via `rtc_transport_bind_rtp()` and parsed RTP dispatches straight to the
  bound stream on the worker thread (no callback hop back into the
  consumer). It also owns **Transport-Wide CC**: the TWCC sender history,
  the receiver arrival window + 100 ms feedback timer, and the GCC
  bandwidth estimator, enabled via `rtc_transport_enable_twcc()` once SDP
  negotiates the transport-cc extension. `rtc_transport_on_rtp()` remains
  as a single-stream raw fallback for packets the demux did not route.

The client `rtc_peer_connection_t` (in `libmrtc_client`) acquires a
shared worker and listener and creates a private transport internally,
exposing a spec-shaped API on top. Server-side media routing uses `libmrtc_routing`
to add routers, producers, consumers, and SSRC dispatch on top of core
transports.

### Packet I/O (`rtc_packet_io.h/c`)

Owns a UDP socket and classifies packets per RFC 7983. It is a *passive*
low-level primitive used by `rtc_listener_t`: it has no thread and no
timers of its own. The owning `rtc_worker_t` registers the socket with
its poller and calls `rtc_packet_io_drain()` when the socket becomes
readable, so every recv callback fires on the worker loop thread.

- **Socket:** single UDP socket, bound to an ephemeral port
- **Drain:** `rtc_packet_io_drain()` does one non-blocking batch read
  (recvmmsg on Linux, recvmsg / recvfrom elsewhere) and dispatches each
  packet to the recv callback
- **Packet demux:** classifies incoming packets per RFC 7983 by first
  byte — STUN (0-3), DTLS (20-63), RTP/RTCP (128-191), TURN ChannelData
  (64-79)

Packet flow:
```
Recv:  worker poller → rtc_packet_io_drain() → demux (RFC 7983) → callback(type, data, from)
Send:  rtc_packet_io_send(data, dest) → UDP sendto (thread-safe)
```

Key functions: `rtc_packet_io_init_ex()`, `rtc_packet_io_drain()`,
`rtc_packet_io_send()`, `rtc_packet_io_get_local_addr()`.

### ICE / STUN

Listener STUN timer creation/cancellation runs on its owning worker, including
publication of zero-delay timer handles. Resolving a STUN hostname occurs on
the configuring caller before the listener is activated, not inside the media
loop. Worker-thread callers must supply numeric STUN addresses; an unsupported
hostname is reported and host gathering remains available without claiming a
server-reflexive candidate.

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
| RELAY | Worker-owned TURN allocation and permission/channel lifecycle |

**Algorithm:**
1. Listener exposes local host candidates and transport ICE credentials.
2. Remote SDP / trickle candidates populate the logical transport.
3. Full ICE transports perform authenticated connectivity checks using
   controlling/controlled roles, tie-breakers, and nomination. ICE-lite
   transports answer checks.
4. The nominated remote tuple routes DTLS / RTP / RTCP. Consent checks
   detect a peer that stops responding; active ICE restart can replace
   credentials and reselect the path.

`rtc_transport_set_ice_controlling()` selects the full-ICE role.
`rtc_transport_restart_ice()` restarts an established transport's ICE state
without requiring a new transport object.

**Candidate eligibility is not connectivity.** The shared parser retains valid
numeric, mDNS-host and TCP metadata in both SDP and trickle candidates. Malformed
attributes fail explicitly. UDP transports classify TCP alternatives as
ineligible and defer `.local` hosts without unicast DNS lookup or worker
blocking. An authenticated incoming check can then supply the peer-reflexive
numeric tuple. Stale ICE username fragments do not mutate a restarted path.

`remote_candidates_deferred` and `remote_candidates_ineligible` in transport
stats make these classifications observable; they do not set selected/SRTP
readiness. There is no multicast-DNS resolver. The legacy standalone ICE agent
requires numeric UDP inputs; browser connections use the active runtime path.

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
- `rtc_transport_set_turn_server()` attaches the runtime relay path;
  `rtc_transport_get_relay_candidate()` exposes its allocated candidate
- Each allocation has an isolated worker-owned socket, avoiding allocation
  collisions between logical transports sharing the normal UDP listener
- Allocation, permission, and channel refresh and cleanup are timer-driven
- UDP/TCP/TLS client-to-server transport with UDP relay allocations.
- TCP/TLS uses bounded framing/partial writes on the existing worker. TLS
  verifies server identity and trust; no insecure certificate bypass exists.
- Hostname resolution occurs on the configuring caller, never the media
  worker. Up to four unique results are retained, placing an alternate address
  family second. Core callers may supply a copied numeric list instead.
- Before allocation, alternatives are tried sequentially with a five-second
  per-address budget and 100 ms worker checks. The host, credentials and CA path
  are owned across retries; TLS verifies the original identity on every attempt.
  Failed I/O is removed before replacement, without increasing registered I/O
  slots per logical transport. Pending retry timers are cancelled on close.
- Exhaustion retains a failed relay runtime rather than enabling a direct path.
  Stats expose attempted-address count and terminal failure. The first
  allocation pins the address; multiple-server selection, post-allocation
  migration and RFC 8489 authentication v2 remain separate work.

### DTLS (`rtc_dtls.h/c`)

Implements TLS-over-UDP handshake and SRTP key export.

- Minimum DTLS 1.2, with no maximum pinned; OpenSSL 4 can negotiate DTLS 1.3
- Datagram-preserving memory BIOs on OpenSSL 3.2+ and an explicit MTU of 1200
  keep handshake datagram boundaries intact
- OpenSSL 3.0+ EVP API (modern crypto, no deprecated functions)
- Memory BIO mode: SSL reads / writes to memory buffers, app controls
  UDP transmission
- Self-signed certificates generated at init; remote SHA-256 fingerprints
  supplied through `rtc_transport_set_remote_dtls_parameters()` are pinned
  during the handshake
- SRTP key export via `SRTP_PROTECTION_PROFILE` after handshake

States: `NEW` → `CONNECTING` → `CONNECTED` → `FAILED` / `CLOSED`

Handshake flow:
```
Active side:   dtls_handshake() → SSL_do_handshake() → wBIO → send via UDP
Passive side:  receive DTLS → feed to rBIO → SSL processes → may write to wBIO
After:         dtls_export_srtp_keys() → client/server keys + salts
```

### SRTP (`rtc_srtp.h/c`)

Three DTLS-SRTP protection profiles, selected by the handshake (offered in
this preference order): `SRTP_AEAD_AES_256_GCM` and `SRTP_AEAD_AES_128_GCM`
(RFC 7714) and `SRTP_AES128_CM_SHA1_80` (RFC 3711, AES-128-CM + HMAC-SHA1-80).

- Master key / salt derived from DTLS export (16- or 32-byte key; 14-byte
  salt for AES-CM, 12-byte for GCM)
- AES-CM: derive session keys via PRF, encrypt payload, append 80-bit
  HMAC tag. GCM: single AEAD pass (AES-128 or AES-256) with the RTP header
  as associated data and a 128-bit tag (no separate HMAC key)
- Per-SSRC rollover counter (ROC) + replay window: bundled streams share
  one key but keep independent sequence spaces

**Thread safety.** `rtc_srtp_protect` / `rtc_srtp_unprotect` /
`rtc_srtp_protect_rtcp` / `rtc_srtp_unprotect_rtcp` serialize on a
per-context mutex (`rtc_srtp_ctx_t::lock`). Runtime transports own
SRTP / SRTCP protection; RTP sends, RTCP SR / RR emission, TWCC
feedback, and NACK retransmits can touch the same context from
different runtime callbacks. Concurrent updates of the per-SSRC rollover
state / `srtcp_index` would otherwise reuse the keystream, which breaks
confidentiality. `init` and `close` are not thread-safe and must not
race with protect / unprotect calls.

Key functions: `rtc_srtp_init()` / `rtc_srtp_init_profile()`,
`rtc_srtp_protect()`, `rtc_srtp_unprotect()`.

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
unprotect for all outgoing / incoming RTCP. Inbound RTCP is parsed and
routed by the core `rtc_media_session` (SR → the receive stream,
RR / NACK / PLI / FIR → the named send stream); the session also runs a
5 s SR / RR emission timer over its streams, and the transport runs the
100 ms TWCC feedback timer when negotiated.

Statistics tracked:
```
Send: packets_sent, octets_sent, rtp_timestamp
Recv: packets_received, jitter, last_transit, last_sr_ntp
```

### RTP streams, demux & media session (`rtc_rtp_stream.h`, `rtc_rtp_demux.h`, `rtc_media_session.h`)

- **`rtc_rtp_send_stream` / `rtc_rtp_recv_stream`** — composed RTP/RTCP
  streams owning sequencing, RTCP stats, NACK retransmit, PLI rate
  limiting, and SR / RR emission. Codec-agnostic: they track only payload
  type + clock rate (the WebRTC codec descriptor + kind live on the client
  facade).
- **`rtc_rtp_demux`** — the single SSRC → consumer routing primitive. A
  uniform sink plus an optional resolver (first-packet match, e.g. by
  payload type, then auto-bind). Owned by the transport and reused by the
  client peer connection (SSRC → receive stream) and the SFU router
  (SSRC → producer), so neither carries its own SSRC map or callback hop.
- **`rtc_media_session`** — the per-peer media plane over one transport.
  It owns the send/receive streams, installs the transport's RTP router
  (per-SSRC sink + resolver) and drives the RTCP plane through
  an **interceptor chain** (`rtc_interceptor`): it splits each inbound
  compound RTCP packet into sub-packets and dispatches them, plus a periodic
  tick, to an ordered list of interceptors. The built-ins replicate the
  former hardcoded switch — a *report* interceptor (SR → receive stream,
  RR → send stream + RR loss → the transport bandwidth estimator, and SR/RR
  emission on tick), a *NACK responder*, and a *PLI/FIR responder* — and
  applications can append their own (REMB, RFC 8888, stats, logging) with
  `rtc_media_session_add_interceptor()`. The resolver routes an unbound SSRC
  first by the **MID header extension** (RFC 8843/8852, when negotiated),
  then by payload type, so bundled m-sections that share a payload type land
  on the right receive stream. This is the reusable glue that previously
  lived hand-written in the client.

Receive-source lifetime is independent of track/session lifetime. Compound
framing and SR/RR/BYE bodies are validated before dispatch. Known-source BYE
unbinds the receiver; replacement retires the old SSRC before binding the new
one. A bounded 32-entry history suppresses late retired-source RTP for five
deterministic report intervals, normally 25 seconds; overflow closes the
transport with an error. Explicit binding clears the matching retirement.
SR/RR source lookup scans the bounded receiver list; bound RTP remains O(1).

Known RTP/SR/RR activity refreshes the same five-interval inactivity lease.
Expiry unbinds the source but permits fresh same-SSRC resumption. Source change
resets receive statistics/recovery and notifies the owning-worker callback;
each nonzero binding advances a source generation. Neither retirement nor
expiry resets SRTP replay/nonce state. Session start/stop and report timer
publication/cancellation are serialized on the owning worker.

The transport owns bounded RTCP membership under its send mutex. Authenticated
RTP SSRCs/CSRCs, report/feedback senders and validated SDES chunks add remote
members; successful outgoing packets record local activity. BYE removes only
the listed sources. There are at most 128 entries; input exhaustion closes the
transport, while local exhaustion returns a logged resource error.

Reports use live member/sender counts and shared protected-packet-size EWMA
(including conservative IPv6/UDP overhead), not a fixed four-source estimate.
Sender roles expire after two sender intervals and members after five
deterministic receiver intervals. Timer/reverse reconsideration handles group
growth/shrinkage; healthy routing report drivers poll deadlines within 100 ms.
Idle local sources switch from SR to RR after losing sender status. The
transport stats snapshot exposes the live counts.
When native send/receive streams share a local SSRC, the media session combines
their report state: one schedule emits SR plus the receiver's RR block (or RR
once idle), rather than counting two independent reporters for one SSRC.

SR RTP/NTP pairs advance from the newest transmitted RTP timestamp and its
monotonic anchor at the configured codec clock rate. Equal-timestamp fragments
and reordered packets do not rebase that anchor. Explicit clock/random-draw
primitives support deterministic rollover, idle and reconsideration tests.
Original publisher capture-clock correlation across SFU legs is not yet
preserved by this send-anchor approximation.

SRTP retains up to 64 SSRC contexts in each send-RTP, receive-RTP and
receive-SRTCP table for the entire key lifetime. It never evicts replay windows
or rollover counters. Receive entries are allocated only after authentication;
capacity exhaustion closes the receiving transport, while send exhaustion
returns `RTC_ERR_NOMEM`. A new keyed transport is required to reclaim capacity,
not ICE restart or source expiry.

### SDP (`rtc_sdp.h/c`)

Offer / Answer generation and parsing (RFC 4566 WebRTC subset).

- Multi-media: audio + video + application (data channel) m= lines
- ICE credentials (ufrag, pwd), DTLS fingerprint + setup role
- Codec parameters (payload type, clock rate, channels)
- Candidate lines as SDP attributes
- `a=extmap:<id> <uri>` (RFC 8285) emit + parse with
  `rtc_sdp_media_add_extmap()` /
  `rtc_sdp_media_find_extmap_id()`. The client peer connection
  auto-advertises the MID extension (id=4) and transport-cc (id=5) on every
  audio / video m-section.

### Rate Control (`rtc_rate_control.h/c`, internal)

AIMD (Additive Increase, Multiplicative Decrease) bitrate adaptation
driven by RTCP Receiver Reports. Internal: each video send stream
allocates a controller in `rtc_rtp_send_stream_arm_video`, and the RTCP
router routes each RR block to it via `rtc_rtp_send_stream_on_rr`.

- Loss < 2% → increase bitrate 5%
- Loss > 5% → decrease bitrate 20%
- RTT > 300 ms → decrease bitrate 10%
- Loss > 10% → request keyframe
- Clamps to configurable `[min, max]` bitrate bounds

Key functions: `rtc_rate_control_create()`,
`rtc_rate_control_on_rtcp_rr()`, `rtc_rate_control_get_bitrate()`,
`rtc_rate_control_should_keyframe()`.

### NACK retransmit buffer (`rtc_nack_buf.h/c`, internal)

512-packet ring buffer indexed by RTP sequence number. Stores post-SRTP
packets so an incoming Generic NACK (RFC 4585 §6.2.1) can be served by
re-sending the original wire packet through `rtc_transport_send_raw()`
without re-encrypting. Each video send stream allocates one when armed.

### Transport-Wide CC (`rtc_twcc_sender.h/c`, `rtc_twcc_receiver.h/c`, internal)

Wire-level implementation of
`draft-holmer-rmcat-transport-wide-cc-extensions`.

- **Sender ring** — 1024 entries of
  `(twcc_seq, send_time_us, wire_size)` keyed by `twcc_seq & (RING-1)`.
  When `rtc_rtp_sender_send()` runs, the send stream assigns the next seq
  from the transport-owned ring, writes the transport-cc extension into
  the RTP header, then records the post-SRTP wire size + send time after
  protect.
- **Receiver window** — 256 entries; `rtc_twcc_receiver_on_packet()`
  records arrivals (driven by the transport's inbound RTP path). The
  100 ms feedback timer in the transport calls
  `rtc_twcc_receiver_build_feedback()` which emits an RTCP
  PT=205/FMT=15 packet using run-length and 14×1-bit status-vector
  chunks plus 1- or 2-byte 250 µs receive deltas. The packet is then
  SRTCP-protected and sent.
- **Parse** — `rtc_rtcp_parse_twcc()` walks chunks and reconstructs
  absolute arrival time per packet (relative to the 24-bit reference
  time × 64 ms).

### Bandwidth Estimator (`rtc_bwe.h/c`, internal)

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

The transport owns one `rtc_bwe_t` when transport-cc is negotiated
(gated by its `enable_twcc` config and seeded by
`initial_outgoing_bitrate_bps`) and surfaces the result via
`rtc_transport_on_bitrate_estimate()`, which the client peer connection
forwards to `rtc_peer_connection_on_bitrate_estimate()`.

## Threading Model

The worker owns the event-loop thread per runtime shard. Control operations,
packet dispatch, and transport timers run there; the hot send path can run
on the application thread.

- **Main / app thread:** API calls into runtime primitives, frame
  capture, UI rendering. Transport control, query, and teardown calls
  are marshalled onto the worker thread (synchronously) so the caller
  never mutates worker-owned ICE / DTLS control state directly. Media sends
  protect packets through the transport's synchronized SRTP context.
- **Worker loop thread:** drains the listener socket, runs RFC 7983
  demux and route dispatch, fires timers (ICE checks, DTLS
  retransmission, application-owned RTCP / TWCC), and executes
  marshalled tasks (control ops, socket (de)registration, teardown).

Packet dispatch, timers, and control/teardown are serialized by the worker.
The hot RTP/RTCP send path checks the atomic published flags
`selected_remote_valid` and `srtp_ready`; SRTP/SRTCP counters and cryptographic
state are protected by the per-context mutex described above.

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
| `test_media_session` | Per-peer media session: RTCP feedback routing (NACK / PLI) |
| `test_interceptor` | RTCP interceptor chain: dispatch order, tick, ownership |
| `test_rtp_demux` | SSRC → consumer demux: bind / resolve / dispatch |
| `test_nack_buf` | NACK ring buffer store/lookup, wraparound |
| `test_twcc` | TWCC sender ring, receiver window, feedback round-trip |
| `test_bwe` | GCC steady increase, delay-induced decrease, loss clamp, callback |
| `test_turn` | TURN allocation, ChannelData framing, credentials |
| `test_network_runtime` | Fingerprint binding, ICE roles/consent/restart, and runtime relay lifecycle |
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

- TURN supports bounded pre-allocation address failover for one identity over
  UDP/TCP/verified TLS. Multiple-server failover, live allocation migration and
  authentication v2 are not implemented.
- Reports and feedback are compound SR/RR + SDES, with feedback capability
  intersection in SDP. Reports use bounded live membership, sender expiry and
  timer/reverse reconsideration for the supported unicast profile.
  Incoming/outgoing BYE and inactivity expiry are implemented; SSRC collision
  recovery, original publisher capture-clock correlation across SFU legs and
  large-group BYE backoff remain incomplete.
- Replay windows and fingerprint checks are implemented, but this educational
  stack has not undergone an independent production security audit.
