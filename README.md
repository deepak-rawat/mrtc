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
| `MRTC_ENABLE_CLIENT_API` | `ON` | Builds peer connection, tracks, and data-channel APIs |
| `MRTC_ENABLE_SFU_API` | `ON` | Exposes SFU public headers and SFU-focused tests |
| `MRTC_ENABLE_TWCC` | `ON` | Builds transport-wide CC and GCC bandwidth estimator |
| `MRTC_ENABLE_RATE_CONTROL` | `ON` | Builds RR-based AIMD sender rate control |

The runtime transport core is always part of `libmrtc`. Client peer connections
and SFU APIs both use the same worker/listener/router/logical-transport stack;
`MRTC_ENABLE_SFU_API` controls public SFU API exposure, not whether the runtime
implementation is compiled.

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
./rtc/test_sfu_transport
./media/test_media_pipeline
./signaling/test_signaling
```

## License

Public domain / educational use. Not intended for production.
