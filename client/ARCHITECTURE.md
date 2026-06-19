# Client Peer Connection Facade (libmrtc_client)

WebRTC-style peer-connection API mirroring the W3C `RTCPeerConnection`
interface. Built on top of the runtime transport core in `libmrtc`.

`libmrtc_client` is built when `MRTC_ENABLE_CLIENT_API=ON` (default).
It links `libmrtc` publicly, so consumers only need to link
`mrtc_client` — the runtime headers and library are pulled in
transitively.

For runtime / SFU consumers that drive worker / listener / router /
transport / producer / consumer directly, see
[../rtc/ARCHITECTURE.md](../rtc/ARCHITECTURE.md). The client facade
described here is a layer on top.

## Usage Example

```c
rtc_client_init();

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
rtc_client_cleanup();
```

## Public Headers

All client headers install under `include/rtc/` alongside the runtime
headers. On disk they live in `client/include/rtc/`.

- `rtc_peer.h` — `RTCPeerConnection` (lifecycle, SDP, ICE callbacks, stats)
- `rtc_track.h` — `RTCRtpSender` / `Receiver` / `Transceiver`
- `rtc_data_channel.h` — `RTCDataChannel`
- `rtc_stats.h` — `getStats()` snapshot reporting (W3C webrtc-stats subset)
- `rtc_client.h` — umbrella include (pulls in `rtc.h` + the four above)

## Module Details

### Peer Connection (`rtc_peer.h/c`, `rtc_peer_packets.c`)

High-level WebRTC-style API. A peer connection is a facade over a private
runtime stack — it acquires a shared default `rtc_worker_t` and
`rtc_listener_t`, and instantiates a single `rtc_transport_t` for the session.

```c
struct rtc_peer_connection {
    rtc_client_runtime_t *runtime;        // shared worker + listener
    rtc_transport_t *runtime_transport;   // private ICE/DTLS/SRTP/RTP endpoint
    rtc_rtp_transceiver_t transceivers[8]; // media tracks (core send/recv streams)
    rtc_dc_manager_t dc_manager;          // data channels
    rtc_rtcp_router_t rtcp_router;         // inbound RTCP -> stream routing
    rtc_desc_t local_desc, remote_desc;
    uint8_t twcc_ext_id;                  // negotiated transport-cc id
    _Atomic rtc_connection_state_t state;
};
```

Inbound RTP no longer round-trips through the peer: the transport owns an
SSRC -> receive-stream demux that the peer populates from SDP (and a
payload-type resolver for un-signalled SSRCs), so a parsed packet goes
transport -> receive stream -> app `on_frame` on the worker thread. The
TWCC sender/receiver and the GCC bandwidth estimator live in the
transport (it is transport-*wide* congestion control); the peer only
negotiates the extension id and forwards the bitrate estimate.

**Lifecycle:** create → add_track → create_offer → set_local_desc →
(signal) → set_remote_desc → (auto: runtime ICE → DTLS → SRTP) → send
media → close → destroy

**Shared runtime.** All peer connections in a process share one
`rtc_client_runtime_t` (worker + listener), reference-counted
and torn down by `rtc_client_cleanup()` before it calls the underlying
`rtc_cleanup()`. This keeps the ephemeral UDP port count down (one
socket for many peer connections) without exposing the sharing to the
application.

**Lifecycle entry points.** `rtc_client_init()` (declared in
[rtc_client.h](include/rtc/rtc_client.h)) calls `rtc_init()` and then
brings up the shared runtime. `rtc_client_cleanup()` tears down the
shared runtime and then calls `rtc_cleanup()`. Apps linking
`libmrtc_client` should call this pair; the bare `rtc_init` /
`rtc_cleanup` are for runtime-only consumers.

### Track / Transceiver (`rtc_track.h/c`)

RTP sender/receiver per media line. Opaque types — internal structs in
[client/src/rtc_peer_internal.h](src/rtc_peer_internal.h). The sender and
receiver are thin handles over the core `rtc_rtp_send_stream` /
`rtc_rtp_recv_stream`; the core streams stay codec-agnostic (payload type
+ clock rate only), so the WebRTC media model — `rtc_codec_t` descriptor
+ `rtc_kind_t` — lives on the facade and the calls just forward.

- **`rtc_rtp_sender_t`** — takes encoded payload, builds RTP header, and
  sends through the peer's logical transport. The transport (in `libmrtc`)
  owns SRTP protection and selected-tuple routing. Runs on the caller
  thread.
  ```c
  rtc_rtp_sender_send(sender, payload, len, samples, marker);
  ```
- **`rtc_rtp_receiver_t`** — delivers raw RTP payloads after runtime
  SRTP unprotect via callback. Includes seq, timestamp, SSRC, and marker.
  ```c
  typedef void (*rtc_on_frame_fn)(const uint8_t *payload, size_t len,
                                  uint16_t seq, uint32_t timestamp,
                                  uint32_t ssrc, bool marker, void *user);
  ```
- **`rtc_rtp_transceiver_t`** — pairs sender + receiver with direction
  (`sendrecv`, `sendonly`, `recvonly`, `inactive`) and MID (m= line index).

### Data Channels (`rtc_data_channel.h/c`)

Text/binary message exchange over DTLS. Fully functional with create,
send, receive, and close.

Wire format: `[1B channel_id] [1B msg_type] [2B length BE] [payload]`

Message types: OPEN, ACK, DATA, CLOSE. Up to 16 concurrent channels
(`RTC_DC_MAX_CHANNELS`). The manager handles OPEN/ACK handshake and
dispatches via callbacks (`on_open`, `on_message`, `on_close`). Channels
can be created locally via `rtc_peer_connection_create_data_channel()` or
received remotely via the `on_data_channel` callback.

Per-channel observability: `rtc_data_channel_bytes_sent` /
`_bytes_received` expose cumulative byte counters. The W3C
`bufferedAmount`, `bufferedAmountLowThreshold`, and `onbufferedamountlow`
surface is implemented but `bufferedAmount` is always 0 today because
sends are synchronous (no internal queue); the API exists for spec parity
and future async backpressure.

Key functions: `rtc_data_channel_send()`,
`rtc_data_channel_send_text()`, `rtc_data_channel_close()`.

### Stats Reporting (`rtc_stats.h`, `rtc_peer.c`)

`rtc_peer_connection_get_stats()` snapshots per-transceiver counters into
a `rtc_stats_report_t` (subset of W3C `RTCStatsReport`). The values are
monotonic counters; the read is best-effort lock-free across the main
and runtime threads.

### Integrated Protocol Features

The peer connection wires up several `libmrtc` features as soon as the
SDP negotiation indicates they are in scope. The algorithms themselves
are documented in [../rtc/ARCHITECTURE.md](../rtc/ARCHITECTURE.md).

- **Per-sender AIMD rate control** — one `rtc_rate_controller_t` lives
  inside each send stream; the RTCP router routes each parsed RR block to
  the originating send stream.
- **NACK retransmit buffer** — 512-packet ring buffer per video send
  stream; incoming Generic NACK blocks are served by re-sending the
  cached post-SRTP wire packet through the runtime transport.
- **Transport-Wide CC** — owned by the transport. The send stream writes
  the transport-cc extension into outbound RTP and records send time +
  wire size into the transport's TWCC sender ring; the transport records
  arrivals and its 100 ms timer emits an SRTCP-protected feedback packet.
- **Bandwidth estimator** — the transport owns one `rtc_bwe_t` when
  transport-cc is negotiated. Each parsed TWCC feedback item plus RR
  `fraction_lost` is fed in; the estimate is surfaced via the transport
  and forwarded to the application through
  `rtc_peer_connection_on_bitrate_estimate()`. Apps subscribe explicitly
  — the bitrate is *not* auto-wired to any media encoder.

## Threading Model

The peer connection inherits the runtime's threading model:

- **Main thread** — API calls, `add_track`, `create_offer`/`answer`,
  `set_local`/`remote_desc`, `sender_send`, `close`, `destroy`. Transport
  control / teardown calls marshal onto the worker thread internally.
- **Worker loop thread** (from `libmrtc`) — the single background thread:
  socket I/O drain, RFC 7983 demux, per-route dispatch to this peer's
  transport, and all logical-transport timers (ICE checks, DTLS
  retransmission, RTCP / TWCC feedback emission).

State observable to callers is published through C11 `_Atomic` flags on
the peer connection (`signaling_state`, `ice_connection_state`,
`connection_state`). Plain seq-cst loads / stores act as acquire / release
fences, so any protocol state written before a transition is visible to
the reader.

## Tests

| Test | Coverage |
|------|----------|
| `test_peer` | Full peer-connection lifecycle, offer/answer, ICE candidates |
| `test_data_channel` | Channel open/close, message send/recv, multi-channel |
| `test_rtp_sender_loopback` | Client RTP sender end-to-end over a real runtime transport |

`test_rtp_sender_loopback` brings up an in-process runtime (worker /
listener / router / transport + DTLS / SRTP loopback) as fixture and
exercises `rtc_rtp_sender_send()` through it — it is in `client/tests/`
because the *system under test* is the client sender path even though
the scaffolding touches runtime internals.

## Known Limitations

- BWE bitrate is exposed via callback but not auto-wired to media
  pipeline encoder bitrate; applications must subscribe explicitly.
- Stats report covers per-transceiver RTP counters only; ICE / DTLS /
  data-channel counters are accessible only through the runtime
  transport's `rtc_transport_get_stats()` and per-channel byte counters.
- Always ICE-controlling role (no nomination negotiation).
- Single peer-connection-per-process is supported, but all peers share
  one default runtime; per-peer worker isolation is not yet a public knob.
