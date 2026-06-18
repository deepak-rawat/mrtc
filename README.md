# mrtc — Minimal WebRTC in C (vibe-coded)

A minimal, working WebRTC implementation in C, entirely vibe-coded with AI.
Built to learn how WebRTC works under the hood and to evaluate AI-assisted
development for complex systems programming projects.

Implements the full protocol stack (ICE, DTLS, SRTP, RTP, SDP) from scratch,
with VP8/Opus media codecs and a multi-party video conference application.

Follows the **Pion model**: the `rtc` core is codec-agnostic and operates at the
RTP payload level. The `media` library provides optional codecs and pipeline
orchestration on top. All public API functions use the `rtc_` prefix.

## Features

- **STUN/ICE + TURN client support** — NAT traversal, candidate gathering, and connectivity checks using external TURN relay services
- **DTLS 1.2 + SRTP/SRTCP** — encrypted media (AES-128-CM, HMAC-SHA1-80)
- **RTP/RTCP** — media packets, sender/receiver reports, jitter/loss stats
- **RTP header extensions** (RFC 8285 one-byte form) with SDP `a=extmap:` negotiation
- **RTCP feedback** — NACK / PLI / FIR plus per-video NACK retransmit buffer
- **Transport-Wide Congestion Control** (draft-holmer-rmcat-transport-wide-cc) end-to-end
- **GCC bandwidth estimator** — trendline + loss controller; exposes target via callback
- **SDP** — multi-media session description (audio + video + data channels)
- **Data Channels** — message framing over DTLS (OPEN/ACK handshake, up to 16 channels)
- **Runtime transport core** — worker, shared UDP listener, logical transports, router, producer, consumer
- **Peer Connection** — high-level client API backed by the same runtime transport core used by SFU APIs
- **SFU primitives** — shared listener, router, logical transports, producers, and consumers for server-side media forwarding
- **VP8** (libvpx) + **Opus** (libopus) — video/audio codecs with RTP packetization
- **Media pipeline** — encode-once fan-out, jitter buffer, per-sender rate control
- **Conference library** — multi-peer orchestration
- **Signaling** — WebSocket client + server for meeting management
- **Applications** — `chat` (terminal text messaging) and `conf_sdl` (SDL3 video conference)

See [ARCHITECTURE.md](ARCHITECTURE.md) for design details and project structure.
See [ROADMAP.md](ROADMAP.md) for planned improvements.

## Dependencies

| Library | Purpose | Required by |
|---------|---------|-------------|
| **OpenSSL 3.0+** | DTLS, SRTP, HMAC, random | common, rtc |
| **libwebsockets** | WebSocket protocol | signaling |
| **cJSON** | JSON parsing | signaling |
| **libvpx** | VP8 codec | media |
| **libopus** | Opus codec | media |
| **SDL3** | Camera, audio, graphics | app/conf_sdl |
| **CMake** >= 3.14 | Build system | all |

## Building

```bash
# Linux
sudo apt install build-essential cmake libssl-dev
mkdir build && cd build && cmake .. && make

# Windows (MSYS2 UCRT64)
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,openssl,ninja,libwebsockets,cjson}
mkdir build && cd build && cmake -G Ninja .. && ninja
```

Build options:

| Option | Default | Notes |
|---|---:|---|
| `MRTC_ENABLE_CLIENT_API` | `ON` | Builds the `libmrtc_client` peer-connection facade on top of `libmrtc` |
| `MRTC_ENABLE_TWCC` | `ON` | Builds transport-wide CC and GCC bandwidth estimator |
| `MRTC_ENABLE_RATE_CONTROL` | `ON` | Builds RR-based AIMD sender rate control |

The `rtc/` component produces two static libraries:

- `libmrtc` — always built. Runtime transport core (worker, listener,
  transport) plus the protocol primitives.
- `libmrtc_routing` — built when `MRTC_ENABLE_ROUTING_API=ON`. Server-side
  routing primitives (router, producer, consumer) on top of `libmrtc`.
- `libmrtc_client` — built when `MRTC_ENABLE_CLIENT_API=ON`. The
  WebRTC-style `RTCPeerConnection` facade (tracks, data channels,
  stats). Links `libmrtc` publicly.

## ICE / NAT Traversal

Local host candidates are gathered from all active non-loopback interfaces and
included in offers/answers immediately. For peers behind NAT, configure a STUN
server so mrtc can also gather server-reflexive candidates and trickle them via
`rtc_peer_connection_on_ice_candidate` after `set_local_desc`.

```c
rtc_config_t config = {0};
config.stun_server = "stun.l.google.com";
config.stun_port = 19302;

rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
```

The existing `ice_servers` form is also accepted:

```c
rtc_config_t config = {0};
config.ice_servers[0].urls[0] = "stun:stun.l.google.com:19302";
config.ice_servers[0].url_count = 1;
config.ice_server_count = 1;
```

Applications should forward every non-NULL candidate callback over signaling and
treat a NULL candidate as end-of-candidates. With STUN configured, that NULL
sentinel may arrive asynchronously after `rtc_peer_connection_set_local_desc`
returns.

## Running

```bash
# Start signaling server
./signaling/signaling_server 8080

# Chat (text over data channels)
./app/chat/chat --meeting test          # run in 2+ terminals

# Video conference (SDL3)
./app/conf_sdl/conf_sdl --meeting test  # M=mute, V=camera, Q=quit
```

## Tests

40 test executables. No external framework.

```bash
# Run all tests (from build/)
ctest --output-on-failure

# Or individually
./rtc/test_stun
./rtc/test_peer
./routing/test_routing_transport
./media/test_media_pipeline
./signaling/test_signaling
```

## License

Public domain / educational use. Not intended for production.
