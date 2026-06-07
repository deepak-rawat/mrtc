# Core Transport Library (libmrtc)

The core library implements the WebRTC protocol stack: STUN/ICE checks, DTLS,
SRTP, RTP, RTCP, SDP, data channels, the client peer facade, and the shared
runtime transport used by both client and SFU APIs.

Follows the **Pion model**: codec-agnostic at the RTP payload level. Senders take already-encoded payloads. Receivers deliver raw RTP payloads. Encoding/decoding is handled by the optional `media` library on top.

## Usage Example

```c
rtc_init();

// ── Create peer connection with STUN server ──
rtc_config_t config = {0};
config.ice_servers[0].urls[0] = "stun:stun.l.google.com:19302";
config.ice_servers[0].url_count = 1;
config.ice_server_count = 1;
rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);

// ── Add tracks (creates transceivers, returns senders) ──
rtc_codec_t vp8 = { .payload_type = 96, .clock_rate = 90000 };
strcpy(vp8.mime_type, "video/VP8");
rtc_rtp_sender_t *video = rtc_peer_connection_add_track(pc, RTC_KIND_VIDEO, &vp8);

rtc_codec_t opus = { .payload_type = 111, .clock_rate = 48000, .channels = 2 };
strcpy(opus.mime_type, "audio/opus");
rtc_rtp_sender_t *audio = rtc_peer_connection_add_track(pc, RTC_KIND_AUDIO, &opus);

// ── Callbacks ──
rtc_peer_connection_on_connection_state(pc, on_state, ctx);
rtc_peer_connection_on_track(pc, on_track, ctx);

// ── Offer/Answer via signaling ──
rtc_desc_t offer;
rtc_peer_connection_create_offer(pc, &offer);
rtc_peer_connection_set_local_desc(pc, &offer);
send_to_signaling(offer.sdp);
// ... receive remote answer ...
rtc_peer_connection_set_remote_desc(pc, &answer);
// → ICE + DTLS + SRTP start automatically

// ── Send media (after CONNECTED) ──
// Payload = VP8 descriptor + encoded bitstream (app encodes)
rtc_rtp_sender_send(video, vp8_payload, len, 3000, true);  // 3000 = 90kHz/30fps
rtc_rtp_sender_send(audio, opus_payload, len, 960, false);  // 960 = 20ms at 48kHz

// ── Receive media ──
void on_track(rtc_rtp_receiver_t *receiver, void *user) {
    rtc_rtp_receiver_on_frame(receiver, on_recv, user);
}
void on_recv(const uint8_t *payload, size_t len,
             uint16_t seq, uint32_t timestamp,
             uint32_t ssrc, bool marker, void *user) {
    // raw RTP payload — app depacketizes + decodes
}

// ── Data channels ──
rtc_data_channel_init_t opts = { .ordered = true };
rtc_data_channel_t *dc = rtc_peer_connection_create_data_channel(pc, "chat", &opts);
rtc_data_channel_send(dc, data, len);

// ── Cleanup ──
rtc_peer_connection_close(pc);
rtc_peer_connection_destroy(pc);
rtc_cleanup();
```

## Protocol Stack

```
┌────────────────────────────────────┐
│ Client Peer API │ SFU API          │  rtc_peer.h/c, rtc_sfu.h
├────────────────────────────────────┤
│ Runtime Transport Core             │  worker, listener, router, transport
├────────────────────────────────────┤
│  SDP │ RTP │ SRTP │ Track/DC       │  rtc_sdp, rtc_rtp, rtc_srtp, rtc_track, rtc_data_channel
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

Public API lives in `include/rtc/`; internal/private helpers such as STUN/TURN live in `src/`.

Public headers:

- `rtc_common.h` — error codes, log macros, platform abstractions
- `rtc_peer.h` — `RTCPeerConnection` (lifecycle, SDP, ICE callbacks)
- `rtc_track.h` — `RTCRtpSender` / `Receiver` / `Transceiver`
- `rtc_data_channel.h` — `RTCDataChannel`
- `rtc_worker.h`, `rtc_listener.h`, `rtc_router.h`, `rtc_transport.h` — runtime transport/SFU primitives
- `rtc_producer.h`, `rtc_consumer.h`, `rtc_rtp_params.h` — SFU media graph types
- `rtc_stats.h` — `getStats()` snapshot reporting (RFC 7867 / W3C webrtc-stats subset)
- `rtc.h` — umbrella include

## Module Details

### Runtime Transport Core

The runtime core is always built into `libmrtc`. It is the only network runtime
used by the client peer facade and is also the public SFU foundation.

- **`rtc_worker_t`** — timer/scheduling shard used by logical transports.
- **`rtc_listener_t`** — owns a UDP packet I/O socket and demux maps. One listener can serve many logical transports on the same local port.
- **`rtc_router_t`** — media routing graph for SFU-style producer/consumer forwarding.
- **`rtc_transport_t`** — one logical ICE/DTLS/SRTP endpoint over a shared listener. Handles ICE-lite responses, full ICE checks, DTLS, SRTP/SRTCP, RTP/RTCP callbacks, and DTLS application data.
- **`rtc_producer_t` / `rtc_consumer_t`** — inbound and outbound media graph edges for SFU forwarding.

Client `rtc_peer_connection_t` creates a private worker/listener/router/transport internally. SFU users can create those same primitives directly when `MRTC_ENABLE_SFU_API` exposes the SFU public headers.

### Packet I/O (`rtc_packet_io.h/c`)

Owns a UDP socket and runs a background thread with a platform I/O poller. It is
now a low-level primitive used by `rtc_listener_t`, not a peer-connection
transport by itself.

- **Socket:** single UDP socket, bound to an ephemeral port
- **I/O Poller:** `rtc_poller_t` — epoll (Linux), kqueue (macOS), select (Windows)
- **Background thread:** runs the poller loop, fires timers, dispatches packets
- **Packet demux:** classifies incoming packets per RFC 7983 by first byte — STUN (0-3), DTLS (20-63), RTP/RTCP (128-191), TURN ChannelData (64-79)
- **Timers:** dynamic scheduler-backed callbacks; runtime transports use worker timers for ICE/DTLS/RTCP flows

Packet flow:
```
Recv:  UDP socket → demux (RFC 7983) → callback(type, data, from_addr)
Send:  rtc_packet_io_send(data, dest) → UDP sendto (thread-safe)
```

Key functions: `rtc_packet_io_init_ex()`, `rtc_packet_io_send()`, `rtc_packet_io_get_local_addr()`.

### ICE / STUN

The runtime transport implements the active logical ICE path used by peers and
SFU transports. It registers listener demux routes by local ufrag, STUN
transaction ID, and selected remote tuple.

The older `rtc_ice.h/c` agent remains as a protocol component and test target,
but `rtc_peer_connection_t` no longer owns an `rtc_ice_agent_t`.

**Candidate types:**
| Type | Gathering method |
|------|-----------------|
| HOST | Interface enumeration (getifaddrs / GetAdaptersAddresses) |
| SRFLX | STUN Binding Request to public server |
| RELAY | TURN allocation (not yet wired into gathering) |

**Algorithm:**
1. Listener exposes local host candidates and transport ICE credentials.
2. Remote SDP/trickle candidates populate the logical transport.
3. Full ICE transports send Binding Requests; ICE-lite transports answer Binding Requests.
4. First successful tuple becomes the selected remote tuple and routes DTLS/RTP/RTCP.

### STUN / TURN (`rtc/src/rtc_stun.h/c`, `rtc/src/rtc_turn.h/c`)

**STUN (RFC 5389):**
- Binding Request/Response with MESSAGE-INTEGRITY (HMAC-SHA1) + FINGERPRINT
- Used by ICE for server-reflexive candidate discovery and connectivity checks

**TURN (RFC 5766):**
- Allocate, Refresh, CreatePermission, ChannelBind methods
- Long-term credentials (MD5 + realm + nonce)
- ChannelData framing for peer communication
- Partially implemented — not yet wired into ICE gathering

### DTLS 1.2 (`rtc_dtls.h/c`)

Implements TLS-over-UDP handshake and SRTP key export.

- OpenSSL 3.0+ EVP API (modern crypto, no deprecated functions)
- Memory BIO mode: SSL reads/writes to memory buffers, app controls UDP transmission
- Self-signed certificates generated at init, SHA-256 fingerprint matching
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

- Master key/salt derived from DTLS export
- Per-packet: derive session key via PRF, encrypt payload, append auth tag
- Rollover counter (ROC) tracks sequence number wrap-around

**Thread safety.** `rtc_srtp_protect` / `rtc_srtp_unprotect` /
`rtc_srtp_protect_rtcp` / `rtc_srtp_unprotect_rtcp` serialize on a per-context
mutex (`rtc_srtp_ctx_t::lock`). Runtime transports own SRTP/SRTCP protection;
RTP sends, RTCP SR/RR emission, TWCC feedback, and NACK retransmits can touch the
same context from different runtime callbacks. Concurrent updates of `roc` /
`last_seq` / `srtcp_index` would otherwise reuse the AES-CM keystream, which
breaks confidentiality. `init` and `close` are not thread-safe and must not race
with protect/unprotect calls.

Key functions: `rtc_srtp_init()`, `rtc_srtp_protect()`, `rtc_srtp_unprotect()`

### RTP / RTCP (`rtc_rtp.h/c`, `rtc_rtcp.h/c`, `rtc_rtp_ext.h/c`)

**RTP (RFC 3550):** 12-byte header + payload. Session tracking (SSRC, seq, timestamp). Marker bit support. `rtc_rtp_build_with_ext()` / `rtc_rtp_session_send_with_ext()` emit a one-byte-form header extension block (X bit) alongside the payload; parse exposes `pkt.ext_data` / `pkt.ext_len` for downstream consumers.

**RTP header extensions (RFC 8285):** `rtc_rtp_ext.h/c` implements the one-byte form (ids 1–14, lengths 1–16). Helpers exist for `abs-send-time` (24-bit 6.18 fixed-point) and `transport-cc` (16-bit transport-wide seq).

**RTCP:** Sender Reports (SR) with NTP timestamps, Receiver Reports (RR) with jitter/loss/delay stats. Feedback messages: NACK / PLI / FIR (RFC 4585 + 5104) and Transport-CC (PT=205 FMT=15, draft-holmer-rmcat-transport-wide-cc) build + parse. SRTCP protect/unprotect for all outgoing/incoming RTCP. A 5s periodic timer emits SR/RR per active transceiver; a separate 100ms timer emits TWCC feedback when negotiated.

Statistics tracked:
```
Send: packets_sent, octets_sent, rtp_timestamp
Recv: packets_received, jitter, last_transit, last_sr_ntp
```

### SDP (`rtc_sdp.h/c`)

Offer/Answer generation and parsing (RFC 4566 WebRTC subset).

- Multi-media: audio + video + application (data channel) m= lines
- ICE credentials (ufrag, pwd), DTLS fingerprint + setup role
- Codec parameters (payload type, clock rate, channels)
- Candidate lines as SDP attributes
- `a=extmap:<id> <uri>` (RFC 8285) emit + parse with `rtc_sdp_media_add_extmap()` / `rtc_sdp_media_find_extmap_id()`. Peer connection auto-advertises transport-cc (id=5) on every audio/video m-section.

### Track / Transceiver (`rtc_track.h/c`)

RTP sender/receiver per media line. Opaque types — internal structs in `rtc_track.c`.

- **`rtc_rtp_sender_t`** — takes encoded payload, builds RTP header, and sends through a logical transport. The transport owns SRTP protection and selected tuple routing. Runs on main thread.
  ```c
  rtc_rtp_sender_send(sender, payload, len, samples, marker);
  ```
- **`rtc_rtp_receiver_t`** — delivers raw RTP payloads after runtime SRTP unprotect via callback. Includes seq, timestamp, SSRC, and marker.
  ```c
  typedef void (*rtc_on_frame_fn)(const uint8_t *payload, size_t len,
                                  uint16_t seq, uint32_t timestamp,
                                  uint32_t ssrc, bool marker, void *user);
  ```
- **`rtc_rtp_transceiver_t`** — pairs sender + receiver with direction (`sendrecv`, `sendonly`, `recvonly`, `inactive`) and MID (m= line index).

### Data Channels (`rtc_data_channel.h/c`)

Text/binary message exchange over DTLS. Fully functional with create, send, receive, and close.

Wire format: `[1B channel_id] [1B msg_type] [2B length BE] [payload]`

Message types: OPEN, ACK, DATA, CLOSE. Up to 16 concurrent channels (`RTC_DC_MAX_CHANNELS`). Manager handles OPEN/ACK handshake and dispatches via callbacks (on_open, on_message, on_close). Channels can be created locally via `rtc_peer_connection_create_data_channel()` or received remotely via `on_data_channel` callback.

Per-channel observability: `rtc_data_channel_bytes_sent` / `_bytes_received` expose cumulative byte counters. The W3C `bufferedAmount`, `bufferedAmountLowThreshold`, and `onbufferedamountlow` surface is implemented but `bufferedAmount` is always 0 today because sends are synchronous (no internal queue); the API exists for spec parity and future async backpressure.

Key functions: `rtc_data_channel_send()`, `rtc_data_channel_send_text()`, `rtc_data_channel_close()`

### Rate Control (`rtc_rate_control.h/c`)

AIMD (Additive Increase, Multiplicative Decrease) bitrate adaptation driven by RTCP Receiver Reports. Lives **inside each `rtc_rtp_sender_t`** (per-sender, since Phase 2.5) so an audio RR doesn't perturb a video sender and vice versa. The SSRC→sender hashmap routes each RR block to the originating sender.

- Loss < 2% → increase bitrate 5%
- Loss > 5% → decrease bitrate 20%
- RTT > 300ms → decrease bitrate 10%
- Loss > 10% → request keyframe
- Clamps to configurable [min, max] bitrate bounds

Key functions: `rtc_rate_control_create()`, `rtc_rate_control_on_rtcp_rr()`, `rtc_rate_control_get_bitrate()`, `rtc_rate_control_should_keyframe()`

### NACK retransmit buffer (`rtc_nack_buf.h/c`)

512-packet ring buffer per video sender, indexed by RTP sequence number. Stores post-SRTP packets so an incoming Generic NACK (RFC 4585 §6.2.1) can be served by re-sending the original wire packet through `rtc_transport_send_raw()` without re-encrypting. Created on connect for `RTC_KIND_VIDEO` senders.

### Transport-Wide CC (`rtc_twcc_sender.h/c`, `rtc_twcc_receiver.h/c`)

Wire-level implementation of draft-holmer-rmcat-transport-wide-cc-extensions.

- **Sender ring** — 1024 entries of `(twcc_seq, send_time_us, wire_size)` keyed by `twcc_seq & (RING-1)`. `rtc_rtp_sender_send()` assigns the next seq, writes the transport-cc extension into the RTP header, then records the post-SRTP wire size + send time after protect.
- **Receiver window** — 256 entries; `rtc_twcc_receiver_on_packet()` records arrivals. The 100ms feedback timer in the peer connection calls `rtc_twcc_receiver_build_feedback()` which emits an RTCP PT=205/FMT=15 packet using run-length and 14×1-bit status-vector chunks plus 1- or 2-byte 250µs receive deltas. The packet is then SRTCP-protected and sent.
- **Parse** — `rtc_rtcp_parse_twcc()` walks chunks and reconstructs absolute arrival time per packet (relative to the 24-bit reference time × 64ms).

### Bandwidth Estimator (`rtc_bwe.h/c`)

Simplified Google Congestion Control (draft-ietf-rmcat-gcc):

- Group packets by send time into ≤5ms bursts.
- Inter-group delay = `recv_delta − send_delta`, accumulated.
- Trendline filter (linear regression over a 20-sample sliding window) → slope.
- Adaptive threshold drives `{NORMAL, OVERUSE, UNDERUSE}`.
- Rate controller: NORMAL → multiplicative (×1.08) or additive (+50 kbps) increase; OVERUSE → decrease to 0.85 × recent throughput; UNDERUSE → hold.
- Loss controller (folded in via `rtc_bwe_on_loss`): <2% → +5%, 2–10% hold, >10% → ×(1−0.5·loss).
- Final target = `min(delay_estimate, loss_estimate)` clamped to `[min_bps, max_bps]`.
- Callback fires on >3% change or 1s elapsed.

One `rtc_bwe_t` per peer is created when transport-cc is negotiated. `rtc_peer.c` feeds each parsed TWCC feedback item via `rtc_twcc_sender_lookup`, plus RR `fraction_lost`, into the BWE; the resulting bitrate is exposed to applications via `rtc_peer_connection_on_bitrate_estimate()`.

### Peer Connection (`rtc_peer.h/c`)

High-level WebRTC-style API (mirrors RTCPeerConnection). It is a facade over the
same runtime transport core used by the SFU API:

```c
struct rtc_peer_connection {
  rtc_worker_t *runtime_worker;         // Runtime timer/scheduling shard
  rtc_listener_t *runtime_listener;     // Shared UDP listener
  rtc_router_t *runtime_router;         // Private routing graph
  rtc_transport_t *runtime_transport;   // Logical ICE/DTLS/SRTP endpoint
    rtc_rtp_transceiver_t transceivers[8]; // Media tracks
    rtc_dc_manager_t dc_manager;          // Data channels
    rtc_rate_controller_t *rate_ctrl;     // AIMD rate control
    rtc_desc_t local_desc, remote_desc;
    rtc_twcc_sender_t twcc_sender;        // outbound transport-wide seq history
    rtc_twcc_receiver_t twcc_receiver;    // inbound arrivals → feedback
    rtc_bwe_t *bwe;                       // GCC bandwidth estimator (per peer)
    _Atomic rtc_connection_state_t state;
};
```

**Lifecycle:** create → add_track → create_offer → set_local_desc → (signal) → set_remote_desc → (auto: runtime ICE → DTLS → SRTP) → send media → close → destroy

## Threading Model

- **Main thread:** API calls, frame capture, UI rendering
- **Listener packet I/O thread:** socket I/O, RFC 7983 demux, listener route dispatch
- **Worker timers:** logical transport ICE checks, DTLS retransmission, peer RTCP/TWCC timers
- Peer connection uses C11 `_Atomic` state flags for cross-thread observation
  (plain reads/writes are seq_cst, giving acquire/release publication of any
  protocol state written before the state transition).
- Packet I/O uses the dynamic timer scheduler for low-level callbacks. Runtime
  transports use worker timers and listener route maps rather than peer-owned
  packet I/O timers.

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
| `test_peer` | Full peer connection lifecycle, offer/answer |
| `test_rtcp` | SR/RR build/parse, jitter statistics |
| `test_rtcp_feedback` | NACK / PLI / FIR build + parse |
| `test_nack_buf` | NACK ring buffer store/lookup, wraparound |
| `test_twcc` | TWCC sender ring, receiver window, feedback round-trip |
| `test_bwe` | GCC steady increase, delay-induced decrease, loss clamp, callback |
| `test_data_channel` | Channel open/close, message send/recv |
| `test_turn` | TURN allocation, ChannelData framing, credentials |
| `test_rate_control` | AIMD algorithm, bitrate adaptation, keyframe requests |
| `test_worker` | Runtime worker lifecycle and timer scheduling |
| `test_listener` | Shared UDP listener candidates, demux, stats |
| `test_sfu_transport` | Logical transport ICE-lite, DTLS/SRTP, RTP/RTCP/data hooks |
| `test_sfu_ice` | Full ICE transport connecting to ICE-lite transport |
| `test_sfu_media` | Producer/consumer media graph forwarding |
| `test_sfu_timers` | Runtime timer behavior |
| `test_sfu_sender` | Peer RTP sender over logical transport |

## Known Limitations

- Local candidates are still produced synchronously; remote trickle candidate ingestion is implemented
- TURN relay candidates not yet wired into ICE gathering
- Always ICE-controlling role (no nomination negotiation)
- No RTCP compound packets (single SR or RR per interval)
- No full SRTP/SRTCP replay hardening audit yet
- BWE bitrate is exposed via callback but not auto-wired to media pipeline encoder bitrate (apps subscribe explicitly)
