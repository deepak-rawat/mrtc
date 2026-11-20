# Testing

Run commands from the repository root after the [build](../README.md).
Component-local C suites check libraries (including real transport integration);
application-local suites check policy/UI/control. Root [E2E](../tests/e2e/)
runners orchestrate real applications with JS/Playwright, not replacement
signaling. [Performance tools](../tests/performance/) are separate.

## Choose a suite

| Check | Command |
|---|---|
| All configured native suites | `ctest --test-dir build --output-on-failure --timeout 120` |
| List configured suites | `ctest --test-dir build -N` |
| One native suite | `ctest --test-dir build -R '^test_packet_io$' --output-on-failure` |
| Web contracts / typecheck+build | `npm --prefix app/web test` / `npm --prefix app/web run build` |
| Runner/report contracts | `npm --prefix tests/e2e run test:controller` |
| Native media/chat; no browser | `npm --prefix tests/e2e run test:native` |
| Real browser UI | `npm --prefix tests/e2e run test:browser` |
| Browser/native media/chat | `npm --prefix tests/e2e run test:browser-native` |
| Handoff, timeout/abort, stale sessions | `npm --prefix tests/e2e run test:migration` |
| 120 same-process reconnects | `npm --prefix tests/e2e run test:churn` |
| Full E2E; excludes soak/performance | `npm --prefix tests/e2e test` |
| Performance-tool contracts only | `npm --prefix tests/performance test` |

CTest does not run npm suites. Build options/platform dependencies change its
list. Install coturn for genuine relay tests (`brew install coturn` or your
Linux package manager); some fixtures skip without prerequisites.

## E2E setup and failures

Build `mrtc_server` and `mrtc_cli`; browser scenarios also need web assets:
```sh
npm --prefix app/web ci
npm --prefix app/web run build
npm --prefix tests/e2e ci
npm --prefix tests/e2e run install:browser
npm --prefix tests/e2e test
```
Defaults: native `build/`, web `app/web/dist/`, Chromium. Override with
`MRTC_BUILD_DIR` / `MRTC_WEB_ROOT` (root-relative or absolute).
For Firefox: `(cd tests/e2e && npx playwright install firefox)`, then set
`MRTC_BROWSER=firefox` for a browser suite. `MRTC_CHROME_PATH` selects an existing
Chromium executable; `PLAYWRIGHT_BROWSERS_PATH` overrides the browser cache.

Runners reserve loopback ports and stop only children they start. They require
fresh per-source decoded media and delivered SCTP messages, not just ICE state.
Browser tests preserve normal privacy/autoplay and click the real Join control.
Failures write logs/snapshots to `build/e2e-failures/`; override with
`MRTC_TEST_ARTIFACTS`. `*.test.mjs` files validate runner/report behavior.

## Sanitizers

Use separate build directories and your platform's CMake prefix settings.
```sh
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMRTC_BUILD_DESKTOP=OFF -DMRTC_ENABLE_ASAN=ON -DMRTC_ENABLE_UBSAN=ON
cmake --build build/asan --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --test-dir build/asan --output-on-failure --timeout 120
```
LeakSanitizer requires Linux; omit `detect_leaks=1` on macOS. For races,
configure another build with `-DMRTC_ENABLE_TSAN=ON`, without ASan, then run
affected worker/network/peer/routing tests. Do not mix TSan and ASan.

## Fuzzers

Use Clang with libFuzzer (Homebrew LLVM rather than Apple Clang on macOS).
```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DMRTC_BUILD_FUZZERS=ON \
  -DMRTC_ENABLE_ASAN=ON -DMRTC_ENABLE_UBSAN=ON \
  -DMRTC_BUILD_TESTS=OFF -DMRTC_ENABLE_CLIENT_API=OFF -DMRTC_ENABLE_ROUTING_API=OFF
cmake --build build/fuzz --parallel 4 --target fuzz_packet_parsers fuzz_media_state
mkdir -p build/fuzz/corpus build/fuzz/media-corpus build/fuzz/findings
cp rtc/tests/fuzz_corpus/* build/fuzz/corpus/
build/fuzz/rtc/fuzz_packet_parsers build/fuzz/corpus \
  -max_total_time=60 -max_len=8192 -artifact_prefix=build/fuzz/findings/
build/fuzz/rtc/fuzz_media_state build/fuzz/media-corpus \
  -max_total_time=60 -max_len=8192 -artifact_prefix=build/fuzz/findings/
```
Parser fuzzing covers wire inputs; media-state fuzzing sequences source,
pacing, allocation and write failures. Retain crash inputs as regressions.

## Optional soak and resource checks

Linux/macOS only; fresh decode/chat plus sampled owned-process RSS:
```sh
MRTC_SOAK_SECONDS=300 MRTC_SOAK_PEERS=8 npm --prefix tests/e2e run test:soak
```
Defaults: one room, five-second samples, warmup of min(60 seconds, duration/3).
`MRTC_SOAK_SECONDS` accepts 10..259200; peers 2..8; `MRTC_SOAK_ROOMS` 1..32;
`MRTC_SOAK_SAMPLE_MS` 1000..60000 with at least three samples.
Post-warmup growth allowances default to 64 MiB server / 16 MiB per client;
override with `MRTC_SOAK_SERVER_GROWTH_MIB` / `MRTC_SOAK_CLIENT_GROWTH_MIB`
(1..4096). `MRTC_SOAK_REPORT` overrides the new `build/soak-<time>.jsonl` path.

Longer same-process lifecycle evidence:
```sh
MRTC_CHURN_CYCLES=1000 MRTC_CHURN_TIMEOUT_MS=3600000 \
  MRTC_CHURN_REPORT=build/churn-1000.jsonl \
  MRTC_CHURN_SAMPLE_CYCLES=10 MRTC_CHURN_WARMUP_CYCLES=100 \
  npm --prefix tests/e2e run test:churn
```
Use a new report path and an isolated resource-test window. Samples compare
the same post-leave state. Optional `MRTC_CHURN_SERVER_GROWTH_MIB` and
`MRTC_CHURN_CLIENT_GROWTH_MIB` set allowances: omitted disables a gate;
0 permits no growth. Sampled RSS is not proof of leak-freedom or capacity.

## Manual CI

**manual validation** runs three native Debug jobs, Linux ASan/UBSan, and
Chromium E2E plus web/tool contracts. **manual bounded performance comparison**
is described in [performance](PERFORMANCE.md). Neither runs on pushes/PRs.
Release, Firefox, TSan, fuzzing and soak are opt-in. Actual Safari/device/network
qualification remains separate; see the [roadmap](ROADMAP.md).
