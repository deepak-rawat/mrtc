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

- **STUN/ICE/TURN** — NAT traversal, candidate gathering, connectivity checks, relay
- **DTLS 1.2 + SRTP** — encrypted media (AES-128-CM, HMAC-SHA1-80)
- **RTP/RTCP** — media packets, sender/receiver reports, jitter/loss stats
- **SDP** — multi-media session description (audio + video + data channels)
- **Data Channels** — message framing over DTLS
- **Peer Connection** — high-level WebRTC API
- **VP8** (libvpx) + **Opus** (libopus) — video/audio codecs with RTP packetization
- **Media pipeline** — encode-once fan-out, jitter buffer, rate control
- **Conference library** — multi-peer orchestration
- **Signaling** — WebSocket client + server for meeting management
- **Applications** — `chat` (terminal) and `conf_sdl` (SDL3 video conference)

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
| **CMake** >= 3.10 | Build system | all |

## Building

```bash
# Linux
sudo apt install build-essential cmake libssl-dev
mkdir build && cd build && cmake .. && make

# Windows (MSYS2 UCRT64)
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,openssl,ninja,libwebsockets,cjson}
mkdir build && cd build && cmake -G Ninja .. && ninja
```

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

20 executables, 119 tests. No external framework.

```bash
# Run all tests (from build/)
for t in rtc/test_*.exe media/test_*.exe signaling/test_*.exe; do ./$t; done

# Or individually
./rtc/test_stun
./rtc/test_peer
./media/test_media_pipeline
./signaling/test_signaling
```

## License

Public domain / educational use. Not intended for production.
