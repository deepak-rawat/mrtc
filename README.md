# mrtc — Minimal WebRTC in C (vibe-coded)

A small educational RTC stack, built to learn WebRTC internals and evaluate
AI-assisted systems programming. It is not a production WebRTC service.

Media/transport hardening and its unfulfilled qualification gates are tracked
in [PRODUCTION.md](PRODUCTION.md). Application authentication is deferred;
passing the development tests is not a production-readiness claim.

Native clients use a codec-agnostic C transport runtime, optional VP8/Opus
media, and a WebRTC-style peer facade. The browser client uses the browser's
own WebRTC implementation, not native codecs compiled to WebAssembly.

## Components

| Component | Role |
|---|---|
| [common/](common/ARCHITECTURE.md) | Portable sockets, threads, logging and containers |
| [rtc/](rtc/ARCHITECTURE.md) | Workers, listeners, ICE/DTLS/SRTP/RTP; shared SCTP backend |
| [client/](client/ARCHITECTURE.md) | Native PeerConnection/track facade |
| [routing/](routing/ARCHITECTURE.md) | Producer/consumer packet forwarding |
| [sfu/](sfu/ARCHITECTURE.md) | Independent SFU session engine, no WebSocket/meeting dependency |
| [signaling/](signaling/ARCHITECTURE.md) | Versioned room protocol and WebSocket client |
| [conference/](conference/ARCHITECTURE.md) | Native participant/capture state and staged transport graphs |
| [app/server/](app/server/ARCHITECTURE.md) | Unified launcher, room policy, WebSocket/HTTP control loop |
| [app/common/](app/common/) | Shared native app sessions and synthetic sources |
| [app/cli/](app/cli/README.md) | Headless audio/video, terminal chat and JSONL automation |
| [app/desktop/](app/desktop/ARCHITECTURE.md) | Native SDL conferencing interface |
| [app/web/](app/web/README.md) | Real browser conferencing UI |
| [tests/interop/](tests/interop/README.md) | Automated tests driving the actual web UI and native clients |

See [ARCHITECTURE.md](ARCHITECTURE.md) for ownership and dependencies,
[PROTOCOL.md](signaling/PROTOCOL.md) for the wire contract, and
[RFC_COMPLIANCE.md](RFC_COMPLIANCE.md) for implementation limits.

## What Runs Where

`mrtc_server` is one process. Its control loop handles HTTP/WebSocket messages
and room membership; a separate SFU media worker handles transports and packet
forwarding. The components communicate through stable room/participant/session
IDs and copied events, not borrowed WebSocket-peer pointers.

The server can serve the built browser UI too. **No Node process or separate SFU
executable is needed in normal use.** Node is only for frontend development,
building assets and optional tests.

The old `build/signaling/signaling_server` command is still built from the same
entrypoint. Use `build/app/server/mrtc_server` for new scripts.

## Dependencies

| Library/tool | Purpose |
|---|---|
| OpenSSL 3.0+ | Crypto, DTLS and SRTP |
| libwebsockets + cJSON | HTTP/WebSocket transport and JSON |
| usrsctp >= 0.9.5 | Shared standard SCTP/DCEP backend |
| libvpx + libopus | Native media codecs; not needed by a server-only build |
| SDL3 | Optional native video UI |
| CMake >= 3.14 | Native build |
| Node >= 22.18 | Optional web build/tests; Node 26 is used in CI |

### macOS

```bash
xcode-select --install
brew install cmake ninja pkgconf openssl@4 libwebsockets cjson \
  libvpx opus sdl3 libusrsctp clang-format
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@4)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build --parallel 4
```

### Linux

```bash
sudo apt install build-essential cmake ninja-build pkg-config libssl-dev \
  libwebsockets-dev libcjson-dev libvpx-dev libopus-dev libusrsctp-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
```

Install SDL3 development files when available to build the native gallery.
Without SDL3, the server, chat and non-UI tests still build.

### Windows — MSYS2 UCRT64

```bash
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,openssl,ninja,pkgconf,libwebsockets,cjson,libvpx,opus,sdl3,usrsctp}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
```

## Build the Web Client

All commands below are from the repository root unless stated otherwise.

```bash
npm --prefix app/web ci
npm --prefix app/web run build
```

Generated assets live in ignored `app/web/dist`; TypeScript sources and lockfiles
are committed. Browser source does not become a dependency of the native C build.

## Start Everything

```bash
./build/app/server/mrtc_server 8080 --web-root app/web/dist
```

Open **http://localhost:8080** in browser tabs. Enter a name and room, then click
**Join**. Leave Server URL empty for the same origin's `/ws` endpoint.

Capture starts after Join and permission approval. The explicit synthetic-media
option generates video/tone for device-free testing; permission errors never
silently become fake media.

For native clients in the same room:

```bash
# macOS — run the executable inside the app bundle, in two terminals
./build/app/desktop/mrtc_desktop.app/Contents/MacOS/mrtc_desktop --meeting demo --name Alice

# Linux / Windows (MSYS2)
./build/app/desktop/mrtc_desktop --meeting demo --name Alice

# Headless audio/video plus terminal chat; no devices or SDL required
./build/app/cli/mrtc_cli --meeting demo --name Alice
```

Both native clients use the same session setup and synthetic media generation.
The desktop provides a join screen, local preview, named gallery, status banner,
mute controls and chat. Add `--camera` and/or `--mic` for real devices; explicit
device failures are reported rather than silently replaced by synthetic media.
The headless CLI encodes synthetic video/audio and decodes remote media without
opening devices, windows or speakers.
Desktop text currently uses SDL's ASCII atlas: UTF-8 is preserved, but non-ASCII
glyphs display as `?`. See the [desktop guide](app/desktop/ARCHITECTURE.md).

The old `build/app/chat/chat` and `build/app/conf_sdl/conf_sdl` (macOS app bundle
on macOS) remain compatibility entrypoints. New scripts should use `mrtc_cli`
and `mrtc_desktop`.

### Automated headless end-to-end testing

```bash
# Start an isolated real server and controlled native clients; no browser needed
MRTC_BUILD_DIR=build npm --prefix tests/interop run test:native
```

The Node controller launches `mrtc_cli --control stdio` and exchanges JSON Lines:

```json
{"version":1,"id":1,"command":"join","server":"ws://127.0.0.1:8080/ws","room":"demo","name":"Alice","video":"synthetic","audio":"synthetic"}
{"version":1,"id":2,"command":"stats"}
{"version":1,"id":3,"command":"leave"}
{"version":1,"id":4,"command":"quit"}
```

Command responses are correlated by ID. Joining is asynchronous: an accepted
command is not proof of media delivery. Tests require fresh decoded audio/video
from each expected peer in the committed epoch, exact chat delivery and roster
changes. Local call generations isolate leave/rejoin cycles. stdout is reserved
for structured events/responses; stderr carries logs. See the
[CLI protocol](app/cli/README.md) and [test controller](tests/interop/README.md).
Node is required only by the optional E2E runner, never by native applications.

Restart old server/client processes after updating binaries. Existing processes
do not pick up a rebuilt executable or a new signaling protocol.

## P2P / SFU Policy

The default is **automatic per-room selection**, not global forced SFU:

| Condition | Behavior |
|---|---|
| One participant | Wait for another participant |
| Two participants | Direct mesh/P2P |
| Third joins | Prepare SFU while existing media continues, then commit |
| Count returns to two | Wait 15 seconds, then prepare P2P |
| Transition fails | Abort pending graph, preserve the working graph, report reason |
| P2P connectivity fails | Request server-authorized SFU fallback; avoid retry loops |

Muted participants still count. A fallback caused by failed P2P connectivity
can keep the room on SFU even with two members.

```bash
# Four or fewer use P2P; five or more prefer SFU
./build/app/server/mrtc_server 8080 --mode auto --p2p-max 4 --web-root app/web/dist

# Explicit fixed modes; --mesh remains an alias for --mode mesh
./build/app/server/mrtc_server 8080 --mode sfu --web-root app/web/dist
./build/app/server/mrtc_server 8080 --mode mesh --web-root app/web/dist
```

Other options:

| Option | Default / meaning |
|---|---|
| `--p2p-max` | 2; accepted range 2..8 |
| `--downgrade-delay-ms` | 15000; lower values are useful in isolated tests |
| `--port` | 8080; positional port is also accepted |
| `--listen-ip` / `--bind` | `127.0.0.1` |
| `--announced-ip` | Reachable address advertised to remote clients |
| `--media-port` | 0 = ephemeral shared publisher port |
| `--web-root` | Optional directory of built static assets |

Subscription sessions use separate ephemeral UDP listeners. This keeps
independent DTLS associations distinguishable when native clients share their
local UDP socket. The implementation favors straightforward ownership over
single-bidirectional-connection optimization.

Protocol v2 session IDs and epochs isolate old/pending descriptions and
candidates. A topology switch creates new transport/SCTP sessions; it is not an
ICE restart of the same endpoint. Capture, participant identities and gallery
tiles survive. Legacy protocol-v1 clients are fixed-mode only.

## Networking

For remote devices, bind and advertise the actual reachable server address:

```bash
./build/app/server/mrtc_server 8080 --listen-ip 0.0.0.0 \
  --announced-ip 192.168.1.20 --web-root app/web/dist
```

Account for UDP media ports in the test firewall. For camera/microphone capture
on non-localhost origins, provide **HTTPS/WSS**, for example through a correctly
configured reverse proxy; plain LAN HTTP is not a secure capture context.
The educational server does not supply production authentication or certificate
management.

STUN discovers addresses; TURN is an optional network relay, not the SFU.
Native conference STUN accepts hostname/IP, `stun:` URL, or NULL/empty to disable.
The SDL default is `stun:stun.l.google.com:19302`; `--stun ""` disables external
discovery for a local test. The low-level client supports one authenticated
TURN URL with relay-only routing: `turn:relay.example:3478?transport=udp`,
`turn:relay.example:3478?transport=tcp`, or
`turns:relay.example:5349?transport=tcp`. TLS verifies the relay identity and
trust chain; `rtc_config_t.turn_tls_ca_file` optionally supplies a CA file
instead of OpenSSL's default trust store. Multiple-server/address failover
remains unsupported.

Normal browser privacy is supported without disabling mDNS: valid `.local`
host candidates are retained and explicitly deferred to authenticated
peer-reflexive checks, not sent to unicast DNS. TCP alternatives are classified
as ineligible for UDP listeners. Acceptance is not proof of connectivity;
actual ICE/DTLS/media must succeed. There is no multicast-DNS resolver.

## Tests

```bash
ctest --test-dir build --output-on-failure --timeout 120
npm --prefix app/web run typecheck
npm --prefix app/web test
```

Optional real UI/native interoperability:

```bash
npm --prefix tests/interop ci
npm --prefix tests/interop run install:browser
MRTC_BUILD_DIR=build MRTC_WEB_ROOT=app/web/dist npm --prefix tests/interop run test:all
```

The harness drives the shipped UI served by `mrtc_server`, not an independent
signaling implementation. It checks per-source decoded video/audio, SCTP chat,
fixed modes, automatic 2→3→2 handoff, capture/tile continuity, single selected
audio, timeout/abort behavior, stale messages and 120 native reconnects.
It uses real Join clicks and default browser privacy/autoplay policies.

Test paths above are root-relative; absolute paths also work.
`MRTC_CHROME_PATH` selects an installed Chromium executable.
`PLAYWRIGHT_BROWSERS_PATH` overrides the ignored local browser cache.
Fixtures use isolated loopback ports and stop only processes they start.

Install coturn for genuine relay tests (`brew install coturn`, or the
distribution package). Unix CTests `test_network_turn` / `test_peer_turn`
exercise relay connectivity when available; packet-format unit tests alone
are not relay verification.

CI includes Linux/macOS/Windows native builds, Linux ASan/leak checks, the web
build/contract tests, actual browser/native migration tests and a
client-disabled SFU build. Remote CI results are separate from local validation.
macOS ASan runs do not provide LeakSanitizer coverage.

## Build Profiles

`MRTC_ENABLE_CLIENT_API=OFF` excludes native peer/media/UI libraries but retains
the independent SFU server. `MRTC_ENABLE_ROUTING_API=OFF` excludes SFU; start
that server build with `--mode mesh` explicitly. Shared SCTP is built when
native client or routing support is enabled.

Other existing options are `MRTC_BUILD_TESTS`, `MRTC_ENABLE_ASAN`,
`MRTC_ENABLE_TWCC`, and `MRTC_ENABLE_RATE_CONTROL`.
Set `MRTC_BUILD_DESKTOP=OFF` to build native CLI/server tests without SDL.

## License

[MIT](LICENSE). Dependencies retain their respective licenses; the project
license does not relicense bundled or linked third-party software.
Not intended for production.
