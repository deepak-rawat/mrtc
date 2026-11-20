# Real UI interoperability

Build `app/web/` first. Build `app/server/mrtc_server` and `app/cli/mrtc_cli` in a
native directory such as `build/reorg-web`. No signaling implementation lives
in the test fixture: Playwright drives the shipped UI served by the real
`app/server/mrtc_server` executable.

```sh
cd tests/interop
npm ci
npm run test:controller
npm run test:native
npm run test:browser
npm test
npm run test:migration
npm run test:churn
# Standard suite:
npm run test:all
# Optional longer run, not part of test:all:
npm run test:soak
```

`MRTC_BUILD_DIR` is relative to the repository root (absolute paths also work).
`MRTC_WEB_ROOT` optionally overrides the built app's root-relative/absolute path.
`MRTC_CHROME_PATH` optionally selects an existing Chromium executable.
Use `npm run install:browser` if the browser is missing. It installs headless
Chromium under this harness's ignored `node_modules/.cache`, not system-wide.
`PLAYWRIGHT_BROWSERS_PATH` overrides that location. Playwright remains pinned
to 1.65.1.

`MRTC_BROWSER=chromium|firefox|webkit` selects the engine (default Chromium).
Install the selected runtime with `npx playwright install firefox`, for example.
Only Chromium accepts `MRTC_CHROME_PATH`. Chromium and Firefox have separate CI
jobs; configured jobs are not qualification results. WebKit runs are useful
additional checks but do not certify actual Safari. All engines retain their
normal privacy/autoplay behavior; no fake-media permission bypass is enabled.

Fixtures reserve loopback ephemeral ports and terminate only their own child
processes. No existing application or server is reused. Tests use explicit
synthetic capture through the same Join control, not fake device fallbacks.
Chromium runs headless with default networking/privacy and autoplay policies,
and system audio muted. The real Join click unlocks media playback; decoded
audio counters still verify media reception. Native interoperability requires
that the synthetic browser actually emitted `.local` host candidates, so the
test cannot silently stop exercising mDNS privacy.

`test:browser` verifies three actual browser participants in each forced/auto
mode. `npm test` covers forced SFU/mesh, decoded browser/native media both directions,
SCTP messages, SFU direction renegotiation, live ICE restart, tile removal and
rejoin, and native CLI chat. `test:migration` covers native/browser auto 2→3→2,
per-source decode after each handoff, stable gallery nodes, capture lifetime,
single selected audio stream, coordinator timeout/abort with active-path
continuity, and replayed stale-epoch SDP. The browser-only suite additionally verifies a
non-default threshold with real media at 4→5→4 participants. Override that
boundary with `MRTC_P2P_MAX` (2..7 so an overflow participant fits the room);
the native/browser migration case deliberately remains 2→3→2.
`MRTC_DOWNGRADE_DELAY_MS` overrides its hold-down (default 250).
`test:churn` preserves 120 real native reconnects; override
`MRTC_CHURN_CYCLES` only for a local smoke run.

## Native media and RSS soak

The optional `test:soak` runner uses the real SFU and controlled native clients,
not mock media. It requires fresh decoded media from every remote source and
SCTP chat on each sample, then measures RSS of its owned server/client processes
on Linux or macOS. Reports are JSONL beneath `build/`, including configuration,
commit/dirty state, sampled RSS, growth and a terminal success/failure result.
An existing output file is not overwritten.

```sh
MRTC_BUILD_DIR=build MRTC_SOAK_SECONDS=86400 \
  MRTC_SOAK_PEERS=8 npm --prefix tests/interop run test:soak
```

Defaults are 300 seconds, eight peers, one room and five-second sampling.
`MRTC_SOAK_SECONDS` accepts 10..259200; `MRTC_SOAK_PEERS` 2..8;
`MRTC_SOAK_ROOMS` 1..32; `MRTC_SOAK_SAMPLE_MS` 1000..60000. Duration must allow
at least three samples. Larger workloads should run on an isolated test host;
the accepted options do not establish supported capacity.

Warmup is the smaller of 60 seconds and one third of the run. Post-warmup peak
sampled RSS growth must not exceed 64 MiB for the server or 16 MiB per client.
Override these explicit thresholds with `MRTC_SOAK_SERVER_GROWTH_MIB` and
`MRTC_SOAK_CLIENT_GROWTH_MIB` (1..4096). `MRTC_SOAK_REPORT` overrides the
root-relative output path. These sampled growth gates do not prove absence of
leaks or establish latency/quality SLOs. Review the report and run the actual
24/72-hour qualification separately. CI's 30-second/three-peer run is only a
smoke check.

## Bounded before/after performance benchmark

`benchmark` uses the real fixed SFU, one room and eight native clients by default,
with the CLI's controlled synthetic VP8/Opus defaults and targeted SCTP messages.
It needs no browser or desktop build. Run only during an exclusive measurement
window: stop concurrent compiles, tests and other heavy workloads, use the same
host/power configuration and installed dependencies, and alternate baseline and
candidate runs to expose load/thermal drift. Build both in Release beforehand;
do not rebuild or edit either source worktree during a measurement. The original
pre-hardening baseline is `9a9319b`, not the readiness-plan commit `1f9ab26`.

From the repository root, with the two existing Release build directories:

```sh
npm --prefix tests/interop run test:benchmark
BASELINE=/absolute/path/to/performance-baseline-build
CANDIDATE=/absolute/path/to/current-release-build
for repeat in 1 2 3; do
  MRTC_BUILD_DIR="$BASELINE" MRTC_BENCH_REPORT="build/before-$repeat.jsonl" \
    npm --prefix tests/interop run benchmark
  MRTC_BUILD_DIR="$CANDIDATE" MRTC_BENCH_REPORT="build/after-$repeat.jsonl" \
    npm --prefix tests/interop run benchmark
done
node tests/interop/benchmark-compare.mjs \
  --baseline build/before-{1,2,3}.jsonl --candidate build/after-{1,2,3}.jsonl \
  > build/performance-comparison.json
```

Outputs are exclusively created, never overwritten; choose new names for repeats.
`MRTC_BUILD_DIR` and `MRTC_BENCH_REPORT` accept root-relative or absolute paths.
Defaults: 15 seconds of warmup **after** roster/media/SCTP readiness, followed by
60 seconds of steady state, sampling every 2 seconds. Overrides:
`MRTC_BENCH_SECONDS=60..600`, `MRTC_BENCH_WARMUP_SECONDS=5..120`,
`MRTC_BENCH_PEERS=2..8`, `MRTC_BENCH_SAMPLE_MS=1000..10000`.
Startup/readiness and shutdown are outside the measured interval. Changing the
workload between comparison groups is rejected.

JSONL records the actual CMake cache source worktree's commit, dirty status and
tracked-diff hash (not the harness checkout's HEAD), build/cache/compiler and
dependency configuration, linked-library paths, executable SHA-256 hashes, host,
load, workload, raw samples and one terminal functional result. Source identity
is observed at run time, not an embedded proof of which commit produced a binary:
rebuild after source changes and retain the build logs. Build/source changes
during a run fail it. Check the emitted compiler/dependency/build profiles match
before interpreting a comparison; dependency path differences alone are not a
performance conclusion.

CPU is the owned server/client `ps` cumulative CPU-time delta divided by elapsed
wall time (100% is one logical core), plus separately reported Node controller CPU.
`ps` precision is platform-dependent (Linux may quantize to whole seconds).
Controller CPU excludes its short-lived `ps` subprocesses. RSS is sampled peak
resident bytes for each process, the server, simultaneous client sum and total.
Throughput sums **decoded remote-source** video frames and audio samples per
second, not encoded/network throughput. Every interval must have fresh audio and
video from every source in the unchanged active epoch. SCTP probes send peer 0 →
each remote → peer 0 sequentially, with an explicit controller-mediated reply and
exact-message auditing through shutdown. Probe timing never calls `exchange()`'s
250 ms duplicate-check delay. RTT includes Node/stdio/controller scheduling:
**this is application SCTP round-trip latency, not media one-way or
capture-to-render latency**. Functional failures produce a failed result and
nonzero exit, never partial successful measurements.

`benchmark:compare -- --baseline ... --candidate ...` also accepts JSON reports,
arrays of terminal results, or JSONL and prints every metric's median delta,
min/max, range span percentage, median absolute deviation percentage and range
overlap. It rejects failed/incomplete results, duplicate run IDs, mixed
binary/source identities within a group, and incompatible hosts/workloads.
Use at least three runs per side. Median increases
**over 5% CPU**, decreases **over 5% decoded throughput**, and increases
**over 10% sampled peak RSS or application RTT** are flagged, including per-process
CPU/RSS. A flagged median is not erased by noise: overlapping ranges, range span
above the metric threshold, or MAD above half that threshold request more repeats,
as do fewer than three runs.
Exit codes: 0 = no flagged regression with sufficient repeat evidence,
1 = flagged regression (or invalid input; inspect JSON/stderr),
2 = insufficient/noisy evidence without a flagged regression. These conservative
checks are not statistical significance tests. Report all deltas, including
improvements and noisy/insufficient results; do not cherry-pick successful runs.
Bounded results establish neither supported capacity nor leak-freedom, long-haul
stability, or media latency certification.

### RSS ambiguity and a better bounded repeat protocol

The initial three 15-second-warmup/60-second baseline runs on the same M1 host
showed total sampled RSS peaks of 286.55, 468.58 and 569.95 MiB, but final sampled
totals were **213.44, 285.69 and 250.59 MiB**. The second and third runs shed
resident pages during unchanged active media, with multiple clients and the
server declining together. This is not evidence of monotonic allocation growth.
Decoded video/audio run-to-run spans were below 0.4%, with the same binaries,
eight peers, fixed SFU, synthetic inputs and nominal warmup. The baseline source
configures 320×240 at 15 fps with a 350 kbit/s initial camera target.

However, those reports did **not** capture actual negotiated/decoded dimensions,
encoded bitrate, allocation/VM footprint, compressed pages or system memory
pressure. Stable decoded frame/sample counts cannot exclude rate-control changes,
and concurrent residency drops cannot prove OS compression/reclamation rather
than application working-set changes. Neither peak nor end RSS alone supports a
memory regression verdict here. Keep every original result; do not discard the
first repeat or trim away inconvenient samples.

For the next exclusive window, predeclare **60 seconds warmup + 120 seconds
measurement**, three chronological pairs, alternating order AB/BA/AB, and a fixed
15-second idle gap between processes. Re-run **both** builds with these settings;
the original shorter baseline is not workload-compatible. For example:

```sh
for slot in before-1 after-1 after-2 before-2 before-3 after-3; do
  case "$slot" in before-*) selected="$BASELINE";; after-*) selected="$CANDIDATE";; esac
  MRTC_BUILD_DIR="$selected" MRTC_BENCH_WARMUP_SECONDS=60 MRTC_BENCH_SECONDS=120 \
    MRTC_BENCH_REPORT="build/paired-$slot.jsonl" node tests/interop/benchmark.mjs
  sleep 15
done
```

Capture OS memory-pressure/VM evidence (`vm_stat` on macOS, `/proc/meminfo` on
Linux) before and after each run; preserve host power/load settings. Interpret
each chronological pair as well as group medians and all variability. If the
longer fixed warmup still leaves large RSS transients or pressure changes,
explicitly report RSS as insufficient evidence and request further bounded
pairs, rather than silently extending warmup until a favorable result appears.
The comparator does not automatically pair observations or claim causality.

### Standalone DTLS/SRTP UDP transport benchmark

`benchmark-transport.c` uses only public APIs shared by `9a9319b` and current
code. Each process owns two loopback-only UDP listeners and two workers, exchanges
real ICE credentials and certificate fingerprints, completes DTLS, verifies SRTP
readiness, and sends timestamped 1,000-byte RTP payloads through actual SRTP
protect/socket/unprotect/receive callbacks. Every packet's payload, index and
exact-once plaintext delivery are checked. A mismatched certificate pin must
fail; there is no unauthenticated transport fallback.

The wrapper compiles only this standalone file against the selected build's
`libmrtc.a`/`libmrtc_common.a` and OpenSSL, using that cache's source headers,
compiler and Release flags. It does not configure/rebuild the project, edit the
baseline source, or install dependencies. It records harness/header/static/shared
library/executable SHA-256 hashes, actual source/build identity, compiler version,
full compiler arguments, linked libraries, host and workload. Artifacts are never
overwritten; changed source, library, header or executable inputs require a new
build/output name. Only unsanitized Release libraries are accepted.

From the repository root, build and test setup **before** an exclusive window:

```sh
MRTC_BUILD_DIR="$BASELINE" MRTC_TRANSPORT_BINARY=build/transport-before \
  node tests/interop/benchmark-transport.mjs build
MRTC_BUILD_DIR="$CANDIDATE" MRTC_TRANSPORT_BINARY=build/transport-after \
  node tests/interop/benchmark-transport.mjs build
MRTC_BUILD_DIR="$BASELINE" MRTC_TRANSPORT_BINARY=build/transport-before \
  node tests/interop/benchmark-transport.mjs smoke
MRTC_TRANSPORT_TEST_BINARY="$PWD/build/transport-before" \
  npm --prefix tests/interop run test:benchmark
```

`smoke` sends only one 32-packet warmup window and one 32-packet verification
window. It emits `transport-smoke-result`, which the comparison parser rejects
as performance evidence. The optional native Node test also verifies that a
deliberately incorrect remote certificate fingerprint is rejected.

During a coordinated exclusive window, use fresh report names for three bounded,
balanced-order repeats per build, for example:

```sh
MRTC_BUILD_DIR="$BASELINE" MRTC_TRANSPORT_BINARY=build/transport-before \
  MRTC_TRANSPORT_REPORT=build/transport-before-1.jsonl \
  node tests/interop/benchmark-transport.mjs run
MRTC_BUILD_DIR="$CANDIDATE" MRTC_TRANSPORT_BINARY=build/transport-after \
  MRTC_TRANSPORT_REPORT=build/transport-after-1.jsonl \
  node tests/interop/benchmark-transport.mjs run
node tests/interop/benchmark-compare.mjs \
  --baseline build/transport-before-{1,2,3}.jsonl \
  --candidate build/transport-after-{1,2,3}.jsonl
```

Transport defaults: five-second warmup, 15-second measurement, two-million packet
cap **per phase**. Overrides: `MRTC_TRANSPORT_WARMUP_SECONDS=1..60`,
`MRTC_TRANSPORT_SECONDS=1..60`, `MRTC_TRANSPORT_PACKET_CAP=1024..8000000`.
The fixed 32-packet window drains before the next, using 100 µs driver polling.
Reported throughput is achieved **delivered packet/payload byte rate including
driver waits**, not a maximum-capacity certification. Missing/corrupt/duplicate
plaintext, handshake failure, timeout, or hitting the packet cap before the
requested duration fails the run; no partial throughput result is accepted.
Timestamping uses one process's monotonic clock immediately before RTP build/send
and at the verified receive callback. The p50/p95/max one-way values include
**transport crypto + UDP socket queues + receive-worker scheduling**, not
capture, codecs, jitter buffering or rendering. They are distinct from the native
benchmark's controller-mediated SCTP RTT and must not be combined with it.
Process CPU includes both workers and the bounded driver, excluding handshake and
warmup; RSS is OS **lifetime peak**, including setup/warmup/teardown, not a sampled
steady-state or live-allocation metric. Transport and native reports cannot be
mixed in one comparison. The same >5% CPU/throughput and >10% RSS/latency
regression flags, repeat/noise warnings and exit codes apply.

### Complete bounded suite and manual CI

`benchmark-suite.mjs` prepares/verifies **both** variants before measuring either.
It requires distinct clean, committed source worktrees (baseline defaults to
`9a9319b`) and never edits the baseline source. Evidence, builds, harnesses and
logs must live outside that baseline worktree. It configures both with the same
Release `-O3 -DNDEBUG` profile, desktop/tests disabled, client/routing/TWCC/rate
control enabled and no sanitizers. Actual compiler ID/version, flags, feature
toggles, dependency versions **and resolved linked-library hashes**, and
source-configured media defaults must match. Preparation fails on compiler
warnings, unknown/mismatched build identity or a changed source/build.

After committing/freezing the final candidate, from the repository root:

```sh
export MRTC_PERF_DIR=build/performance-final
export MRTC_PERF_BASELINE_SOURCE=/absolute/path/to/performance-baseline-src
export MRTC_PERF_CANDIDATE_SOURCE="$PWD"
# Optional newline-separated paths: retain historical noisy reports verbatim.
export MRTC_PERF_PRIOR_REPORTS="$(printf '%s\n' \
  /absolute/path/to/baseline-1.jsonl /absolute/path/to/baseline-2.jsonl \
  /absolute/path/to/baseline-3.jsonl)"
node tests/interop/benchmark-suite.mjs prepare

# Only after coordinating an exclusive measurement window:
node tests/interop/benchmark-suite.mjs run
```

`prepare` configures/builds both libraries/apps and standalone transport harnesses,
and runs only their tiny setup smoke checks. Alternatively, `verify` skips native
project builds, accepting explicitly supplied `MRTC_PERF_BASELINE_BUILD` and
`MRTC_PERF_CANDIDATE_BUILD` only when their actual profiles match all required
settings; it still builds/verifies both transport harnesses. Those two variables
also optionally choose build directories for `prepare`. `all` combines preparation
and measurement and must therefore run exclusively. `compare` reconstructs a
missing terminal artifact from already saved reports/environment captures without
building or measuring; it does not overwrite an existing terminal artifact.
Every preparation requires a **new** `MRTC_PERF_DIR`. Source edits, rebuilds and
dependency changes after preparation require preparing a new suite.
Benchmark/controller source hashes are frozen alongside build inputs; both
variants also run the setup test that rejects an incorrect certificate pin.

The default plan is three chronological AB/BA/AB pairs of **eight-peer,
60-second warmup/120-second steady media**, followed by three AB/BA/AB transport
pairs, with fixed 15-second idle gaps. All settings are fixed in `manifest.json`
at preparation, including:

| Environment option | Default | Bounds |
| --- | ---: | --- |
| `MRTC_PERF_REPEATS` | 3 pairs per workload | 3..5 |
| `MRTC_PERF_PEERS` | 8 | 2..8 |
| `MRTC_PERF_WARMUP_SECONDS` | 60 | 5..120 |
| `MRTC_PERF_SECONDS` | 120 | 60..300 |
| `MRTC_PERF_SAMPLE_MS` | 2000 | 1000..10000 |
| `MRTC_PERF_IDLE_SECONDS` | 15 | 0..60 |
| `MRTC_PERF_TRANSPORT_WARMUP_SECONDS` | 5 | 1..60 |
| `MRTC_PERF_TRANSPORT_SECONDS` | 15 | 1..60 |
| `MRTC_PERF_TRANSPORT_PACKET_CAP` | 4000000 per phase | 1024..8000000 |
| `MRTC_PERF_BUILD_JOBS` | 4 | 1..8 |
| `MRTC_PERF_TIMEOUT_SECONDS` | 3600 | 300..7200 |

Commands have individual deadlines plus the suite deadline and own isolated
process groups, so timeout/failure cleanup also terminates their native children.
Failed/missing/incompatible reports are explicit failures, not zero measurements.
Before/after each run the suite records host/load plus macOS `vm_stat` or Linux
`/proc/meminfo` and memory PSI when available. Missing VM evidence is marked
`unknown`; boundary captures do not reconstruct every transient during a run.
Native reports retain any runtime source dimensions/encoded bitrate exposed in
stats before/after steady state; absent fields remain **unknown**, never inferred
from decoded counters or a configured bitrate target.

The one terminal `comparison.json` includes every raw terminal run/provenance,
environment capture, retained historical reference, media/transport group metric
delta and chronological **per-pair delta**, median paired delta, threshold flags,
range/MAD noise warnings, and insufficient-evidence/functional flags. Group or
median-paired regressions remain flagged even when noisy; individual pair flags
are also retained. Three successful runs cannot resolve unexplained RSS
transients by themselves. Earlier 15/60 noisy baseline reports are archived
unchanged when supplied, never pooled with the longer workload or reinterpreted
as a pass/cause diagnosis. Exit codes remain 0/1/2 as above.

The separate optional `.github/workflows/performance.yml` is **manual only**,
fetches full same-repository history, resolves `baseline_ref` safely through
quoted environment variables, and compares it with the checked-out candidate.
It installs one shared set of dependencies, uses identical Release options,
and always uploads the evidence directory after regression/inconclusive exits.
It has only `contents: read` permission and does not push or dispatch other runs.
Shared-runner CPU, VM pressure and scheduling are explicitly uncontrolled:
`MRTC_PERF_SHARED_RUNNER=1` (also automatic in GitHub Actions) forces an
insufficient-evidence/advisory result, never certification. A regression is still
exit 1; otherwise shared-runner evidence is exit 2. Run the coordinated local
suite for controlled follow-up. Neither workflow status nor either latency
metric establishes capture-to-render media latency, capacity or leak-freedom.

## Headless native controller

`test:native` and `test:churn` use only Node built-ins: they need no browser,
SDL, npm installation, display or capture devices. They launch the shipped
`mrtc_cli --control stdio` against the real server, using synthetic sources
through its shared application session. Windows executable paths receive
the `.exe` suffix automatically. Native-only fixtures omit `--web-root` and
accept an HTTP readiness response regardless of status, so no `app/web/dist`
is needed and `MRTC_WEB_ROOT` is ignored for these suites.

`test:native` exercises forced mesh/SFU and automatic 2→3→2 and custom 4→5→4.
Every active topology epoch must produce fresh decoded video-frame and
audio-sample **deltas for every remote source**, not merely packet counters.
It checks exact-once chat, roster removal, generation-incrementing same-process
rejoin, independent audio/video mute after drain, receive-only participants,
and two participants sending no media while exchanging SCTP messages.
`none` disables sending that source; it does not disable remote subscriptions.
Churn likewise leaves and rejoins the same CLI process 120 times, requiring
fresh bidirectional decode and bidirectional SCTP on every cycle. Mesh
receive-only coverage observes and verifies both real peer-ID offerer orders.

`native-client.mjs` speaks version-1 JSONL. Requests contain `version: 1`,
unique `id`, and `command`; commands are `join` (`server`, `room`, `name`,
`video`/`audio`: `synthetic` or `none`, optional `stun` and `data_only`), `leave`,
`send_message` (`text`, optional `peer_id`), `mute` (`source`, `muted`),
`stats`, and `quit`. Results contain `event: "result"`, `id`, `ok` and
`stats` for a snapshot. Async `joined`, `peer_joined`, `peer_left`, `message`,
`topology`, and `error` events carry `generation` and `epoch`.
Joined/message events identify `peer_id`; messages include `text`.
Snapshots expose `peer_id`, `generation`, `epoch`, `mode`, `status`, and
`peers` with `peer_id`, `name`, `video_frames`, `audio_samples`, and `epoch`.
Counters reset when a new epoch becomes active. Browser chat receives an
explicit test-controller reply; the application contains no probe auto-echo.

Commands have 10-second deadlines, event/media/topology waits at most
45 seconds, and graceful quit at most 5 seconds after its response.
Unexpected EOF, process exit, malformed JSON/protocol events and async errors
immediately reject pending commands and event waits. Frames are capped at
1 MiB, outstanding commands at 128, event history at 2,048 entries, and logs at
200 KB per stream/name.
Only the deliberate migration-abort scenario permits the exact browser
`topology preparation timed out` abort report and requires the matching native
topology event; native async errors and unrelated browser errors still fail.
Churn also has a 360-second total
deadline, overridable with `MRTC_CHURN_TIMEOUT_MS`.

Failures persist bounded command/event output, stderr, the exception and final
native/browser snapshots beneath the repository's `build/interop-failures`.
`MRTC_TEST_ARTIFACTS` overrides that parent directory. Snapshots can report an
unavailable/exited session rather than concealing a process failure.
`test:controller` uses minimal fake child processes only to test JSONL
framing, correlation, deadlines, exits, bounded history and error handling;
it never substitutes fake signaling/media for the real E2E suites.
