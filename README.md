# mrtc — Minimal WebRTC in C (vibe-coded)

A minimal, working WebRTC implementation in C, entirely vibe-coded with AI.
Built to learn how WebRTC works under the hood and to evaluate AI-assisted
development for complex systems programming projects.

Implements the full protocol stack (ICE, DTLS, SRTP, RTP, SDP) from scratch,
with VP8/Opus media codecs and a multi-party video conference application.

Follows the **Pion model**: the `rtc` core is codec-agnostic and operates at the
RTP payload level. The `media` library provides optional codecs and pipeline
orchestration on top. Runtime and client API functions use the `rtc_` prefix;
higher-level libraries use their existing component prefixes.

## Features

- **STUN/ICE + TURN client support** — NAT traversal, candidate gathering, and connectivity checks using external TURN relay services
- **DTLS 1.2 + SRTP/SRTCP** — encrypted media (AES-128/256-GCM, or AES-128-CM with HMAC-SHA1-80)
- **RTP/RTCP** — media packets, sender/receiver reports, jitter/loss stats
- **RTP header extensions** (RFC 8285 one-byte form) with SDP `a=extmap:` negotiation
- **RTCP feedback** — NACK / PLI / FIR plus per-video NACK retransmit buffer
- **Transport-Wide Congestion Control** (draft-holmer-rmcat-transport-wide-cc) end-to-end
- **GCC bandwidth estimator** — trendline + loss controller; exposes target via callback
- **SDP** — multi-media session description (audio + video + data channels)
- **Data Channels** — standard SCTP over DTLS with DCEP, reliable/partial-reliable delivery, fragmentation, and backpressure
- **Runtime transport core** — worker, shared UDP listener, logical transports, router, producer, consumer
- **Peer Connection** — high-level client API backed by the same runtime transport core used by SFU APIs
- **Default SFU conferencing** — signaling server orchestrates publishers/subscribers using the existing router, transport, producer, and consumer primitives; explicit `--mesh` fallback
- **VP8** (libvpx) + **Opus** (libopus) — video/audio codecs with RTP packetization
- **Media pipeline** — encode-once fan-out, jitter buffer, per-sender rate control
- **Conference library** — SFU publish/subscribe orchestration, participant reuse, trickle ICE and decoded media callbacks
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
| **usrsctp** | SCTP transport for standard WebRTC data channels | client |
| **SDL3** | Camera, audio, graphics | app/conf_sdl |
| **CMake** >= 3.14 | Build system | all |

## Building

### macOS

Install the Xcode Command Line Tools and Homebrew dependencies:

```bash
xcode-select --install
brew install cmake ninja pkgconf openssl@4 libwebsockets cjson libvpx opus sdl3 libusrsctp
```

Homebrew's `openssl@4` is keg-only, so pass its prefix explicitly when
configuring. Using `brew --prefix` keeps the command portable between Apple
Silicon and Intel Macs:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@4)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
ctest --test-dir build --output-on-failure
```

The `conf_sdl` target is generated as a macOS application bundle so the system
can request camera, microphone, and local-network permission:

```bash
./build/app/conf_sdl/conf_sdl.app/Contents/MacOS/conf_sdl --meeting test
```

macOS may also ask whether the signaling server can accept incoming network
connections. Allow it when testing with other devices. If camera, microphone,
or local-network access was previously denied, re-enable it under **System
Settings > Privacy & Security**.

### Linux

```bash
sudo apt install build-essential cmake ninja-build pkg-config libssl-dev \
  libwebsockets-dev libcjson-dev libvpx-dev libopus-dev libusrsctp-dev
mkdir build && cd build && cmake .. && make
```

SDL3 is optional at configure time. Install its development package when
available to build the native conference application and its UI tests.

### Windows (MSYS2 UCRT64)

```bash
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,openssl,ninja,pkgconf,libwebsockets,cjson,libvpx,opus,sdl3,usrsctp}
mkdir build && cd build && cmake -G Ninja .. && ninja
```

Build options:

| Option | Default | Notes |
|---|---:|---|
| `MRTC_ENABLE_CLIENT_API` | `ON` | Builds the `libmrtc_client` peer-connection facade on top of `libmrtc` |
| `MRTC_ENABLE_TWCC` | `ON` | Builds transport-wide CC and GCC bandwidth estimator |
| `MRTC_ENABLE_RATE_CONTROL` | `ON` | Builds RR-based AIMD sender rate control |

The runtime, routing, and client components produce these static libraries:

- `libmrtc` — always built. Runtime transport core (worker, listener,
  transport) plus the protocol primitives.
- `libmrtc_routing` — built when `MRTC_ENABLE_ROUTING_API=ON`. Server-side
  routing primitives (router, producer, consumer) on top of `libmrtc`.
- `libmrtc_client` — built when `MRTC_ENABLE_CLIENT_API=ON`. The
  WebRTC-style `RTCPeerConnection` facade (tracks, data channels,
  stats). Links `libmrtc` publicly.

The client facade keeps the public WebRTC-style sender/receiver APIs stable,
while RTP/RTCP sender and receiver protocol state is implemented by the low-level
rtc RTP stream API. This keeps packet accounting, NACK/PLI feedback handling, and
SR/RR emission in the rtc protocol layer without requiring applications to use
those stream helpers directly.

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

The client also supports one authenticated UDP TURN URL and a relay-only policy.
TURN-over-TCP/TLS and multiple TURN URLs are not supported. For active ICE
restart, exchange the refreshed credentials in a new offer/answer cycle.
Live negotiation supports track and direction changes; changing an established
codec or payload type is rejected. Glare is explicit and can be handled through
`rtc_peer_connection_rollback()`.

The conference API and `conf_sdl --stun` accept a bare hostname/IP as well as a
`stun:` URL. The app defaults to `stun:stun.l.google.com:19302`; use a URL to
specify a port. For a localhost-only test, `--stun ""`
disables external STUN discovery; it is not required for local SFU connectivity.

## Running

Run these commands from the `build/` directory:

```bash
# Start signaling server
./signaling/signaling_server 8080

# Chat (text over data channels)
./app/chat/chat --meeting test          # run in 2+ terminals

# Video conference (SDL3)
./app/conf_sdl/conf_sdl --meeting test  # M=mute, V=camera, Q=quit
```

The server defaults to SFU mode and loopback binding. The same conference
client discovers the mode from signaling; no separate SFU client binary is
needed. Use `./signaling/signaling_server 8080 --mesh` for legacy mesh mode.
SFU mode requires both client and routing APIs to be enabled.

For testing on other devices, explicitly bind and announce the reachable
server address, for example:

```bash
./signaling/signaling_server 8080 \
  --listen-ip 0.0.0.0 --announced-ip 192.168.1.20
```

`--media-port` selects the shared publisher UDP port; subscription connections
use separate ephemeral UDP listeners. This keeps independent DTLS associations
on distinguishable network tuples. Account for these ports in a test firewall;
the example is not a production deployment recipe.

On macOS, run the conference executable inside its generated application
bundle instead:

```bash
./app/conf_sdl/conf_sdl.app/Contents/MacOS/conf_sdl --meeting test
```

## Tests

Tests are standalone executables using the project's minimal C harness.
Use CTest discovery rather than a hard-coded test count:

```bash
# Run all tests (from build/)
ctest -N
ctest --output-on-failure

# Or individually
./rtc/test_stun
./client/test_peer
./routing/test_routing_transport
./media/test_media_pipeline
./signaling/test_signaling
```

GitHub Actions runs native builds and tests on Linux, macOS, and Windows, plus
an AddressSanitizer build on Linux. SDL3-dependent tests run where SDL3 is
available.

Install coturn (`brew install coturn` or the distribution's `coturn` package)
to run real relay tests. On Unix, `test_network_turn` reports a skip when
coturn is unavailable; `test_peer_turn` is registered when coturn is found.
Unit TURN packet tests are not a substitute for these relay tests.

### Browser interoperability and conference churn

The optional local harness uses Node.js 26 and pinned Playwright dependencies.
After building the native binaries, run this from the repository root:

```bash
cd app/browser_interop
npm ci
npx playwright install chromium
MRTC_BUILD_DIR=../../build npm test
MRTC_BUILD_DIR=../../build npm run test:churn
```

`npm test` requires real decoded audio/video, SCTP/DCEP messages,
live publisher direction/restart continuity and join/leave/reconnect cleanup.
It also verifies CLI chat in SFU and mesh modes, including batched input/quit.
It uses synthetic media and silent audio playout,
without camera access or audible test tones. `test:churn` validates 120 native
conference generations. Both launch isolated local fixtures and stop them on
exit. `MRTC_CHROME_PATH` optionally selects an installed Chrome executable;
`MRTC_SIGNALING_PORT` overrides the fixture's signaling port.

Browser tests and native churn also run in a separate Linux CI job.

## License

Public domain / educational use. Not intended for production.
