# Core Transport Library (libmrtc)

The core library implements the WebRTC protocol stack: ICE, DTLS, SRTP, RTP, RTCP, SDP, data channels, and peer connection management.

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
│      Peer Connection API           │  rtc_peer.h/c
├────────────────────────────────────┤
│  SDP │ RTP │ SRTP │ Track/DC       │  rtc_sdp, rtc_rtp, rtc_srtp, rtc_track, rtc_data_channel
├────────────────────────────────────┤
│  DTLS (OpenSSL)                    │  rtc_dtls.h/c
├────────────────────────────────────┤
│  ICE Agent                         │  rtc_ice.h/c
├────────────────────────────────────┤
│  STUN Client / TURN Client         │  rtc_stun.h/c, rtc_turn.h/c
├────────────────────────────────────┤
│  Transport Layer (UDP + Threading) │  rtc_transport.h/c
├────────────────────────────────────┤
│  Platform Poller (epoll/kqueue)    │  rtc_poller.h/c
└────────────────────────────────────┘
```

Each layer maps to a header/source pair in `include/rtc/` and `src/`.

## Module Details

### Transport Layer (`rtc_transport.h/c`)

Owns the UDP socket and runs a background thread with a platform I/O poller.

- **Socket:** single UDP socket, bound to an ephemeral port
- **I/O Poller:** `rtc_poller_t` — epoll (Linux), kqueue (macOS), select (Windows)
- **Background thread:** runs the poller loop, fires timers, dispatches packets
- **Packet demux:** classifies incoming packets per RFC 7983 by first byte — STUN (0-3), DTLS (20-63), RTP/RTCP (128-191), TURN ChannelData (64-79)
- **Timers:** up to 16 scheduled callbacks (used for DTLS retransmit, ICE checks)

Packet flow:
```
Recv:  UDP socket → demux (RFC 7983) → callback(type, data, from_addr)
Send:  rtc_transport_send(data, dest) → UDP sendto (thread-safe)
```

Key functions: `rtc_transport_init()`, `rtc_transport_send()`, `rtc_transport_set_recv_callback()`, `rtc_transport_set_remote()`

### ICE Agent (`rtc_ice.h/c`)

Handles candidate gathering and connectivity checks.

**Candidate types:**
| Type | Gathering method |
|------|-----------------|
| HOST | Interface enumeration (getifaddrs / GetAdaptersAddresses) |
| SRFLX | STUN Binding Request to public server |
| RELAY | TURN allocation (not yet wired into gathering) |

**Algorithm:**
1. Gather local candidates (host + STUN server-reflexive)
2. When remote SDP arrives, parse credentials + remote candidates
3. Run connectivity checks: STUN Binding Requests ordered by priority
4. First successful pair → selected, transport gets `remote_addr`

All ICE work runs on the transport thread via callbacks. Main thread reads `selected_remote` after `ICE_STATE_CONNECTED`.

### STUN / TURN (`rtc_stun.h/c`, `rtc_turn.h/c`)

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

Key functions: `rtc_srtp_init()`, `rtc_srtp_protect()`, `rtc_srtp_unprotect()`

### RTP / RTCP (`rtc_rtp.h/c`, `rtc_rtcp.h/c`)

**RTP (RFC 3550):** 12-byte header + payload. Session tracking (SSRC, seq, timestamp). Marker bit support.

**RTCP:** Sender Reports (SR) with NTP timestamps, Receiver Reports (RR) with jitter/loss/delay stats.

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

### Track / Transceiver (`rtc_track.h/c`)

RTP sender/receiver per media line. Opaque types — internal structs in `rtc_track.c`.

- **`rtc_rtp_sender_t`** — takes encoded payload, builds RTP header, SRTP-protects, sends via transport. Runs on main thread.
  ```c
  rtc_rtp_sender_send(sender, payload, len, samples, marker);
  ```
- **`rtc_rtp_receiver_t`** — delivers raw RTP payloads (after SRTP unprotect) via callback on transport thread. Includes seq, timestamp, SSRC, and marker.
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

Key functions: `rtc_data_channel_send()`, `rtc_data_channel_send_text()`, `rtc_data_channel_close()`

### Rate Control (`rtc_rate_control.h/c`)

AIMD (Additive Increase, Multiplicative Decrease) bitrate adaptation driven by RTCP Receiver Reports.

- Loss < 2% → increase bitrate 5%
- Loss > 5% → decrease bitrate 20%
- RTT > 300ms → decrease bitrate 10%
- Loss > 10% → request keyframe
- Clamps to configurable [min, max] bitrate bounds

Key functions: `rtc_rate_control_create()`, `rtc_rate_control_on_rtcp_rr()`, `rtc_rate_control_get_bitrate()`, `rtc_rate_control_should_keyframe()`

### Peer Connection (`rtc_peer.h/c`)

High-level WebRTC-style API (mirrors RTCPeerConnection). Owns all other components:

```c
struct rtc_peer_connection {
    rtc_transport_t transport;            // Socket + I/O thread
    rtc_ice_agent_t ice;                  // Candidate management
    rtc_dtls_transport_t dtls;            // Handshake + key export
    rtc_srtp_ctx_t srtp_send, srtp_recv;  // Encryption contexts
    rtc_rtp_transceiver_t transceivers[8]; // Media tracks
    rtc_dc_manager_t dc_manager;          // Data channels
    rtc_rate_controller_t *rate_ctrl;     // AIMD rate control
    rtc_desc_t local_desc, remote_desc;
    volatile rtc_connection_state_t state;
};
```

**Lifecycle:** create → add_track → create_offer → set_local_desc → (signal) → set_remote_desc → (auto: ICE → DTLS → SRTP) → send media → close → destroy

## Threading Model

- **Main thread:** API calls, frame capture, UI rendering
- **Transport thread:** protocol state machine, socket I/O, packet callbacks
- Peer connection uses volatile state flags for cross-thread observation
- Transport mutex protects timer queue + callback registration

## Tests

| Test | Coverage |
|------|----------|
| `test_stun` | STUN Binding Req/Resp, MESSAGE-INTEGRITY, FINGERPRINT |
| `test_rtp` | RTP header parsing, session tracking, seq/ts increment |
| `test_srtp` | AES-CM encryption, HMAC-SHA1, wrong-key rejection |
| `test_dtls` | DTLS handshake via memory BIOs, SRTP key export |
| `test_ice` | Candidate gathering, connectivity checks, loopback e2e |
| `test_sdp` | SDP generation/parsing round-trip |
| `test_sdp_video` | Multi-media SDP (audio + video), codec params |
| `test_transport` | Socket init, packet demux, timer management |
| `test_peer` | Full peer connection lifecycle, offer/answer |
| `test_rtcp` | SR/RR build/parse, jitter statistics |
| `test_data_channel` | Channel open/close, message send/recv |
| `test_turn` | TURN allocation, ChannelData framing, credentials |
| `test_rate_control` | AIMD algorithm, bitrate adaptation, keyframe requests |

## Known Limitations

- No trickle ICE (all candidates gathered before offer; `add_ice_candidate()` is a stub)
- TURN relay candidates not yet wired into ICE gathering
- Always ICE-controlling role (no nomination negotiation)
- No RTCP compound packets (single SR or RR per interval)
- No RTCP feedback messages (NACK, PLI, FIR, REMB)
- No SRTP replay protection
- Single rate controller shared across all senders per peer
- SSRC→receiver lookup is O(n) linear scan (should use hashmap)
- `volatile` used instead of `_Atomic` for cross-thread state flags
