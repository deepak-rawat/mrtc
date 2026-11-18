# Client Peer Connection Facade (libmrtc_client)

WebRTC-style peer-connection API mirroring the W3C `RTCPeerConnection`
interface. Built on top of the runtime transport core in `libmrtc`.

`libmrtc_client` is built when `MRTC_ENABLE_CLIENT_API=ON` (default).
It links the shared `libmrtc_sctp` backend and `libmrtc`, so consumers only need to link
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

Client headers install under `include/rtc/` alongside runtime/shared SCTP
headers. Peer/track/stats/facade headers live in `client/include/rtc/`;
shared data-channel/SCTP headers now live in `rtc/include/rtc/`.

- `rtc_peer.h` — `RTCPeerConnection` (lifecycle, SDP, ICE callbacks, stats)
- `rtc_track.h` — `RTCRtpSender` / `Receiver` / `Transceiver`
- `rtc_data_channel.h` — `RTCDataChannel`
- `rtc_stats.h` — `getStats()` snapshot reporting (W3C webrtc-stats subset)
- `rtc_client.h` — umbrella include (pulls in `rtc.h` + the four above)
- `rtc_sctp.h` — shared endpoint helpers for runtime/SFU consumers, implemented
  by `mrtc_sctp` rather than the native facade

## Module Details

### Peer Connection (`rtc_peer.h/c`)

High-level WebRTC-style API. A peer connection is a facade over a private
runtime stack — it acquires a shared default `rtc_worker_t` and
`rtc_listener_t`, and instantiates a single `rtc_transport_t` for the session.

```c
struct rtc_peer_connection {
    rtc_client_runtime_t *runtime;        // shared worker + listener
    rtc_transport_t *runtime_transport;   // private ICE/DTLS/SRTP/RTP endpoint
    rtc_rtp_transceiver_t transceivers[8]; // media tracks (core send/recv streams)
    rtc_dc_manager_t dc_manager;          // data channels
    rtc_media_session_t media_session;    // per-peer RTP routing + RTCP + SR/RR
    rtc_desc_t local_desc, remote_desc;
    uint8_t twcc_ext_id;                  // negotiated transport-cc id
    _Atomic rtc_connection_state_t state;
};
```

The per-peer media plane is the core `rtc_media_session` (owned as
`media_session`): it installs the transport's RTP router (per-SSRC sink +
payload-type resolver), routes inbound RTCP (SR -> receive stream;
RR / NACK / PLI / FIR -> send stream), and runs the periodic SR / RR
timer. Inbound RTP no longer round-trips through the peer: the transport
owns an SSRC -> receive-stream demux that the session populates from SDP
(and a payload-type resolver for un-signalled SSRCs), so a parsed packet
goes transport -> receive stream -> app `on_frame` on the worker thread.
The TWCC sender/receiver and the GCC bandwidth estimator live in the
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

### Data Channels (`rtc/include/rtc/rtc_data_channel.h`, `rtc/src/rtc_data_channel.c`)

The SCTP/DCEP backend, public data channel API, and internal manager now live in
the shared `mrtc_sctp` target under `rtc/`. The client facade links this target;
the SFU uses the same implementation without linking the client. Transport-only
applications initialize/clean up the stack with `rtc_sctp_init()` /
`rtc_sctp_cleanup()`; client applications retain `rtc_client_init()` /
`rtc_client_cleanup()`.

Text/binary messages use standard SCTP over DTLS through **usrsctp >= 0.9.5**.
DCEP OPEN/ACK establishes in-band channels; negotiated channels use an explicit
stream ID on both endpoints. DTLS role determines in-band stream-ID parity.
The old custom DTLS framing is not used.

The manager handles reliable, unordered, retransmit-limited, and
lifetime-limited delivery, SCTP fragmentation/reassembly, empty-message PPIDs,
and stream resets on close. Channels can be created locally via
`rtc_peer_connection_create_data_channel()` or received through
`on_data_channel`. Application callbacks and association changes are serialized
with the peer's worker.

Per-channel observability: `rtc_data_channel_bytes_sent` /
`_bytes_received` expose cumulative byte counters. The W3C
`bufferedAmount`, `bufferedAmountLowThreshold`, and `onbufferedamountlow`
reflect SCTP backpressure and threshold crossings. Callers must handle a send
failure instead of assuming every message has been queued.

Key functions: `rtc_data_channel_send()`,
`rtc_data_channel_send_text()`, `rtc_data_channel_close()`.

### Live Negotiation and ICE Restart

The shared native runtime reserves 32 I/O entries for its common listener and
per-peer relay allocations, including overlapping small-room graphs. A focused
construction regression creates eight relay-configured peers on that runtime;
the old eight-entry budget could not fit eight allocations plus the listener.
This is a socket-budget check, not an eight-person relay media/capacity claim.

Repeated offer/answer cycles can add or remove tracks and change directions
after a connection is established. Changes are applied on the worker rather
than by mutating live protocol objects from the application thread.

Glare is rejected explicitly; applications can call
`rtc_peer_connection_rollback()` before accepting the competing offer.
Changing an established codec or payload type is not supported and is rejected
without changing the current descriptions.

Before media starts, an answerer can adopt the offered payload type for a
matching configured codec (for example Opus 109 instead of local preference
111). Sender/receiver stream mappings and the answer change together.
Established mappings remain immutable. Active RTP sections require rtcp-mux;
rejected RTP sections remain rejected/inactive. Feedback and RTP extension IDs
in answers are intersected with the offer rather than independently assigned.

Negotiated single-source REMB constrains the named video sender's target used
by the native encoder pipeline; unrelated/unnegotiated bounds are ignored.
Limits expire after ten seconds without refresh. A zero limit pauses that RTP
sender's wire output without stopping audio or signaling.

`rtc_peer_connection_restart_ice()` supports an active connection. The
application exchanges the new ICE credentials through another offer/answer
cycle; a successful restart preserves the SCTP association and RTP streams.

The client supports one authenticated UDP/TCP `turn:` or TLS `turns:` URL,
including relay-only gathering and asynchronous candidate/end-of-candidates
callbacks. TLS uses verified server identity and the default trust store or
`rtc_config_t.turn_tls_ca_file`. Multiple TURN URLs remain rejected.

### Stats Reporting (`rtc_stats.h`, `rtc_peer.c`)

`rtc_peer_connection_get_stats()` snapshots per-transceiver counters into
a `rtc_stats_report_t` (subset of W3C `RTCStatsReport`). The values are
monotonic counters; snapshots are serialized with the runtime worker.

### Integrated Protocol Features

The peer connection wires up several `libmrtc` features as soon as the
SDP negotiation indicates they are in scope. The algorithms themselves
are documented in [../rtc/ARCHITECTURE.md](../rtc/ARCHITECTURE.md).

- **Per-sender AIMD rate control** — one `rtc_rate_controller_t` lives
  inside each video send stream; the RTCP router routes each parsed RR
  block to the originating send stream.
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
| `test_peer_integration` | Live track changes, glare/rollback, late data channels, and ICE restart continuity |
| `test_peer_turn` | Relay-only media/data, refresh, and restart against a local coturn fixture |

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
- Established codec/payload-type changes are rejected; live track and
  direction changes are supported.
- TURN supports one authenticated UDP/TCP/TLS URL, without server/address failover.
- Multiple peer connections share one default runtime; per-peer worker
  isolation is not a public knob.
