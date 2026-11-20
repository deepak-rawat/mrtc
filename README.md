# mrtc - Minimal WebRTC in C

Educational RTC stack: native desktop/CLI clients, browser UI and conferencing
server. VP8 video, Opus audio and SCTP chat; rooms use P2P for two participants
and switch to an SFU as more join. Not production-ready; no application login.
All commands below run from the repository root.

## Dependencies and build

C11 compiler, CMake >= 3.14, OpenSSL >= 3, libwebsockets, cJSON,
usrsctp >= 0.9.5, libvpx and Opus. SDL3 enables the optional desktop UI.
Node >= 22.18 is needed only for web assets and JavaScript tests.

### macOS
```sh
xcode-select --install
brew install cmake ninja pkgconf openssl@4 libwebsockets cjson libvpx opus sdl3 libusrsctp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@4)" -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

### Linux
```sh
sudo apt install build-essential cmake ninja-build pkg-config libssl-dev \
  libwebsockets-dev libcjson-dev libvpx-dev libopus-dev libusrsctp-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```
Install SDL3 development files separately for the desktop client.

### Windows (MSYS2 UCRT64 shell)
```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,openssl,libwebsockets,cjson,libvpx,opus,sdl3,usrsctp}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

### Build after configuring
```sh
cmake --build build --parallel 4
```
Only `mrtc_server`, `mrtc_cli` and optional `mrtc_desktop` app targets are built;
legacy `signaling_server`, `chat` and `conf_sdl` targets have been removed.
Configure a separate build directory for each profile:

| CMake option | Effect |
|---|---|
| `-DCMAKE_BUILD_TYPE=Release` | Optimized build |
| `-DMRTC_BUILD_DESKTOP=OFF` | Headless CLI/server; no SDL |
| `-DMRTC_ENABLE_CLIENT_API=OFF -DMRTC_BUILD_TESTS=OFF` | Server without native client/codecs |
| `-DMRTC_ENABLE_ROUTING_API=OFF` | No SFU; run with `--mode mesh` |

## Run
```sh
npm --prefix app/web ci
npm --prefix app/web run build
./build/app/server/mrtc_server 8080 --web-root app/web/dist
```
Open **http://localhost:8080**, enter a name/room and click **Join**.
Leave Server URL empty. Choose synthetic media for device-free testing, or
grant camera/microphone permission. **Leave** stops capture.

Join the same room from other terminals:
```sh
# Headless synthetic video/audio; no devices needed
./build/app/cli/mrtc_cli --room demo --name Alice --stun ""
# Desktop on Linux/Windows
./build/app/desktop/mrtc_desktop --meeting demo --name Bob
# Desktop on macOS
./build/app/desktop/mrtc_desktop.app/Contents/MacOS/mrtc_desktop --meeting demo --name Bob
```
Desktop defaults are synthetic; use `--camera`/`--mic` for real devices.
The CLI accepts chat, `status`, `mute video`, `mute audio`, `unmute ...`, `quit`.
`--video none --audio none` receives only; `--data-only` disables media both ways.
Use `--control stdio` for [JSONL automation](docs/PROTOCOL.md#cli-control).
Restart running processes after rebuilding.

| Server option | Default / purpose |
|---|---|
| `--mode auto\|mesh\|sfu` | `auto`; fixed modes disable topology switching |
| `--p2p-max`, `--downgrade-delay-ms` | 2 participants (range 2..8); 15000 ms hold-down |
| `--bind` / `--listen-ip`, `--announced-ip` | Bind `127.0.0.1`; advertise a reachable address |
| `--port`, `--media-port` | 8080 signaling; 0 = ephemeral publisher UDP port |
| `--web-root` | Built assets; SFU subscriptions also use ephemeral UDP ports |

For LAN use, append `--bind 0.0.0.0 --announced-ip 192.168.1.20` and allow
signaling/media through the firewall. Browser device capture outside localhost
requires HTTPS/WSS through a reverse proxy. STUN discovers addresses; TURN
relays traffic; neither is the SFU. Native `--stun ""` disables external STUN.
Use CLI/desktop `--help` for remaining client options.

## Test
```sh
ctest --test-dir build --output-on-failure --timeout 120
npm --prefix app/web test
npm --prefix tests/e2e run test:native
```
For full browser/native E2E, after building the web app:
```sh
npm --prefix tests/e2e ci
npm --prefix tests/e2e run install:browser
npm --prefix tests/e2e test
```
Defaults use `build/` and `app/web/dist/`. See [testing](docs/TESTING.md)
for individual suites, alternate paths, sanitizers, fuzzing and soak runs.
Both GitHub workflows are **manual-only**: choose **manual validation** or
**manual bounded performance comparison**, then **Run workflow**.

## Developer guides

- [Architecture](docs/ARCHITECTURE.md): source map, ownership and a call walkthrough.
- [Testing](docs/TESTING.md) and [performance](docs/PERFORMANCE.md): commands and results.
- [Protocol](docs/PROTOCOL.md): signaling and CLI contracts.
- [Standards](docs/STANDARDS.md): implemented support, gaps and evidence.
- [Roadmap](docs/ROADMAP.md): priorities and release blockers.

[MIT license](LICENSE); dependencies retain their own licenses.
