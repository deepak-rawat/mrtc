# Production profile and release gates

This is the implementation and qualification contract for the first production
profile. It is not a declaration that mrtc is production-ready. Existing
educational mesh/AUTO behavior remains available and keeps its defaults.

## Supported target

- Linux server behind a trusted HTTPS/WSS reverse proxy.
- An opt-in fixed-SFU profile; at most eight participants per room, subject to
  measured instance capacity.
- One VP8 camera and one Opus microphone per participant, with standard SCTP
  chat. Audio-only, receive-only and data-only participants remain supported.
- BUNDLE, RTP/RTCP multiplexing, trickle ICE, consent, fingerprint-verified DTLS,
  SRTP/SRTCP and active ICE restart.
- Short-lived, room-scoped credentials issued by a trusted application backend.
- Chrome, Firefox and actual Safari, plus Linux/macOS/Windows native clients,
  only after the release matrix below has been qualified.

This is a constrained application interoperability profile, not full W3C API
or WebRTC standards certification. Unsupported changes must fail explicitly
without changing a working call. The SFU terminates media encryption; this
profile does not provide participant-to-participant E2EE.

## Work order

| Order | Work | Acceptance |
|---|---|---|
| 0 | Profile and baseline | Supported behaviors have tests; distinguish local results from external qualification |
| 1 | Negotiation | Browser/native offer/answer, payload mapping, directions, BUNDLE/MID and stale-generation rejection |
| 2 | RTCP | Compound SR/RR + CNAME, report timing, source retirement and correct rewritten-stream feedback |
| 3 | Loss/congestion | Both SFU legs recover; slow receivers do not cause unbounded queue delay |
| 4 | Connectivity | UDP relay plus TCP/TLS TURN, credential refresh, path failure and supported dual-stack behavior |
| 5 | Safety | Malformed inputs, allocation/queue failure, concurrent teardown, sanitizers and fuzz regressions |
| 6 | Qualification | Real decoded media/data across supported browsers/platforms, impairments, endurance and capacity |

Authentication, authorization, quotas, observability and deployment operations
are parallel release blockers, not substitutes for protocol correctness.

## Required qualification

### Developer safety commands

Configure a separate Debug build with `-DMRTC_ENABLE_ASAN=ON
-DMRTC_ENABLE_UBSAN=ON`, then build and run CTest normally. UBSan failures are
non-recovering. `-DMRTC_ENABLE_TSAN=ON` requires a separate build without ASan;
the worker and network-runtime fixtures are the initial focused race checks.
Do not treat these as complete concurrency coverage.

For coverage-guided parser fuzzing, use a Clang distribution with libFuzzer
(on macOS, Homebrew LLVM rather than Apple Command Line Tools):

```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DMRTC_BUILD_FUZZERS=ON \
  -DMRTC_ENABLE_ASAN=ON -DMRTC_ENABLE_UBSAN=ON
cmake --build build/fuzz --parallel 4 --target fuzz_packet_parsers
mkdir -p build/fuzz/corpus build/fuzz/findings
build/fuzz/rtc/fuzz_packet_parsers build/fuzz/corpus \
  -max_total_time=60 -max_len=8192 -artifact_prefix=build/fuzz/findings/
```

The harness instruments the library as well as the entry point, exercising
STUN, RTP/extensions, RTCP/feedback, SDP and ICE candidates. Retain reproducing
inputs and add focused regressions for findings. This is parser fuzzing, not
stateful transport fuzzing or an independent security audit.

### Release evidence

Release evidence must identify the commit, build profile, dependency versions,
machine, network conditions, test command and result. A configured CI job is not
evidence that it passed.

- Real browser-to-browser and browser-to-native SFU audio/video/data, including
  every expected source after join, mute/unmute, leave/rejoin and ICE restart.
- Actual Safari and supported native OS testing, not only Chromium or WebKit.
- Direct UDP and authenticated relay, including TCP/TLS when UDP is blocked.
- Loss, burst loss, jitter, reordering and bandwidth reduction on each SFU leg.
  Record freeze duration, audio gaps, latency and recovery, not packet counts.
- Eight-person reference calls with a documented safe instance capacity and
  headroom. Participant/room constants are not capacity measurements.
- A 24-hour call and at least 1,000 lifecycle cycles without sustained resource
  growth; use a 72-hour loaded soak before broad release.
- No unresolved reproducible crashes, sanitizer findings or release-blocking
  independent security-review findings.
- Demonstrated credential expiry/rotation, reverse-proxy restrictions, overload
  rejection, graceful drain and version rollback.
- Representative invite-only pilot meeting agreed call-establishment and
  media-quality SLOs before public availability.

Long soaks, hostile-network measurements, external platform runs and an
independent security review must not be checked off by a local smoke test.

## Deferred unless required by the supported profile

Additional codecs, screen sharing, multiple camera tracks, simulcast/SVC, FEC,
arbitrary established codec mutation, complete browser API/stats parity,
single-connection SFU optimization, multi-region placement and seamless
failover. Existing P2P/AUTO paths are not removed, but are outside the first
production promise.

## Evidence

The starting native baseline on macOS passed all 59 CTests, including local
coturn fixtures and desktop media, with a warning-free build. This does not
establish Linux/Windows/browser qualification or production readiness.

Implemented: native and SFU periodic reports include SDES with the same random
process-local CNAME advertised in SDP. Report parsing respects sub-packet
boundaries; loss fractions use report intervals, and late pre-rollover packets
do not create a false cycle. Focused RTCP/SDP/media/routing/peer/SFU tests pass
locally. Adaptive scheduling, reduced-size feedback negotiation and source
retirement remain required work; compound reports alone do not complete RTCP.

Negotiated MID routing rejects unknown MIDs and ambiguous payload-type-only
matches instead of binding a new source to the first stream. SR reception also
dispatches its reception-report blocks to the sender feedback path.

Local qualification so far:

- Warning-free Release build and all 58 headless Release CTests passed.
- All 58 headless ASan/UBSan CTests passed.
- Focused TSan worker/network-runtime tests passed (2 tests).
- A 30-second instrumented parser fuzz run executed 1,429,929 inputs without
  findings. This duration is a smoke test, not a fuzzing completion threshold.
- Complete Chromium/native interop passed, including mesh/SFU/AUTO, migration,
  failure continuity and 120 same-process reconnect generations.
- Web contracts, TypeScript build and 15 controller tests passed.
- Firefox was installed but could not launch on this host: it reported
  `Could not find profile folder` with both system and workspace-local temporary
  profiles. Firefox qualification remains open; privacy settings were not
  weakened to bypass the launch failure.

CI now defines Debug/Release native matrices, combined ASan/UBSan, focused TSan,
parser fuzzing and Chromium/Firefox jobs. Remote runs are not established by
these local results.
