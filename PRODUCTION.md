# Media and transport production-readiness plan

This plan focuses on the media and transport protocols needed for reliable
production use. It is not a declaration that mrtc is production-ready or a
production deployment mode. Existing mesh/AUTO/SFU behavior keeps its defaults.

## Supported target

- Linux server behind a trusted HTTPS/WSS reverse proxy.
- Fixed SFU as the initial reference configuration (`--mode sfu`); at most
  eight participants per room, subject to measured instance capacity.
- One VP8 camera and one Opus microphone per participant, with standard SCTP
  chat. Audio-only, receive-only and data-only participants remain supported.
- BUNDLE, RTP/RTCP multiplexing, trickle ICE, consent, fingerprint-verified DTLS,
  SRTP/SRTCP and active ICE restart.
- Chrome, Firefox and actual Safari, plus Linux/macOS/Windows native clients,
  only after the release matrix below has been qualified.

This is a constrained application interoperability profile, not full W3C API
or WebRTC standards certification. Unsupported changes must fail explicitly
without changing a working call. The SFU terminates media encryption; this
profile does not provide participant-to-participant E2EE.

## Deferred application and deployment work

Application authentication and authorization are deferred until their design
is decided. There is no JWT/token login implementation, authenticated deployment
profile or `--production` option. Deployment-profile metrics, admission/rate
controls and graceful drain from that bundle have also been removed.

DTLS fingerprint verification, SRTP authentication, ICE/STUN integrity and TURN
credentials remain required transport-security behavior; they are not the
application authentication being deferred. A publicly exposed service still
needs a separate access-control and deployment design before release.

## Work order

| Order | Work | Acceptance |
|---|---|---|
| 0 | Profile and baseline | Supported behaviors have tests; distinguish local results from external qualification |
| 1 | Negotiation | Browser/native offer/answer, payload mapping, directions, BUNDLE/MID and stale-generation rejection |
| 2 | RTCP | Compound SR/RR + CNAME, report timing, source retirement and correct rewritten-stream feedback |
| 3 | Loss/congestion | Both SFU legs recover; slow receivers do not cause unbounded queue delay |
| 4 | Connectivity | UDP relay plus TCP/TLS TURN, allocation/permission/channel refresh, path failure and supported dual-stack behavior |
| 5 | Safety | Malformed inputs, allocation/queue failure, concurrent teardown, sanitizers and fuzz regressions |
| 6 | Qualification | Real decoded media/data across supported browsers/platforms, impairments, endurance and capacity |

Protocol qualification does not replace future application access controls,
quotas, observability or deployment operations. Those are outside this phase.

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
- Demonstrated TURN allocation/permission/channel refresh and explicit bounded
  failure behavior when media/transport resources are exhausted.
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
locally. Full dynamic source lifecycle/reconsideration remains required work;
compound reports alone do not complete RTCP.

Negotiated MID routing rejects unknown MIDs and ambiguous payload-type-only
matches instead of binding a new source to the first stream. SR reception also
dispatches its reception-report blocks to the sender feedback path.

Historical qualification before removing the application/deployment bundle:

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

Later revalidation was not wholly green: the full Debug suite reported two
timeouts (SCTP cleanup and desktop exit); both passed when rerun in isolation.
A later Chromium rerun failed decoded-audio assertions. Diagnostics showed
zero outbound audio packets and a synthetic AudioContext in `running` state
whose clock remained near 0.005 seconds after a two-second wait. The media
assertions and normal autoplay/privacy policy were retained; this source-clock
failure remains a qualification blocker, not a passing test or an established
protocol diagnosis.

After removing the application authentication/deployment bundle, the native
build is warning-free and all 14 targeted signaling/app/protocol CTests pass.
The web build and four web contract tests pass, as do 15 controller tests.
Real native mesh, SFU and AUTO 2-to-3-to-2 / 4-to-5-to-4 E2E scenarios pass
with decoded media, SCTP, roster and lifecycle assertions. These checks do not
re-qualify the browser audio or external-platform gates above.

## Remaining protocol work

The retained changes are a tested foundation, not completion of work items 0–6:

1. Complete supported-profile negotiation validation, including feedback
   capability intersection, rejected sections and browser-specific SDP shapes.
2. Complete incoming BYE/source expiry, SSRC collision recovery and dynamic
   membership reconsideration. Small-connection scheduling, outgoing BYE,
   compound feedback and capability intersection are implemented.
3. Qualify conservative shared-source bitrate adaptation under impairments,
   then complete pacing and subscriber-wide allocation across media legs.
4. Qualify TURN under hostile networks and complete server/address failover and
   dual-stack selection. Nonblocking read/write readiness and TCP/TLS TURN are
   now implemented, with verified certificates, bounded framing/partial writes,
   refresh and coturn runtime/client tests.
5. Add broader allocation-failure/stateful fuzz/concurrency coverage, hostile
   network fixtures and automated capacity/resource-growth measurements.

Actual Safari, Firefox, Linux/Windows qualification, long soaks and independent
protocol/security review remain open. No access-control scheme is selected by
this plan.

## Latest transport implementation

The project code is now MIT-licensed; dependencies retain their own licenses.
The worker supports read/write readiness on epoll/kqueue/select. TURN supports
UDP/TCP/TLS client-to-server legs without blocking the media worker; TLS has
strict trust-chain and server-name/IP verification. Library users configure
`rtc_transport_set_turn_server_ex()` or a single peer `turn:`/`turns:` URL.
Stream sends are bounded to 64 KiB and 500 ms after connection; connection/TLS
setup is bounded to five seconds. DNS uses the calling thread and selects one
address. This is not server/address failover.

Local UDP/TCP/TLS runtime and native peer tests pass, including real SRTP,
DTLS data, SCTP, allocation refresh and ICE restart. Trusted-CA/wrong-identity
and untrusted-CA tests fail closed. All ten targeted TURN/peer tests also pass
under ASan/UBSan. A short-lifetime TLS refresh failure exposed Nagle/delayed-ACK
latency; enabling TCP_NODELAY fixed it, and the focused TLS peer test passed
three consecutive repetitions without changing its lifetime/deadlines.

Feedback is now compound without relying on unnegotiated reduced-size RTCP.
SDP retains payload-specific/wildcard feedback and rejected RTP sections;
native/SFU answers negotiate supported capabilities rather than splicing raw
SDP. Native answers can adopt an initially offered dynamic payload type while
established mappings remain immutable. Receivers gate NACK/PLI/FIR on negotiated
support. Targeted protocol tests and their ASan/UBSan counterparts pass, as do
the four native mesh/SFU/AUTO decoded-media/SCTP E2E scenarios.

Negotiated single-source REMB now carries the minimum fresh active-consumer
video bound to the original publisher SSRC. Receiver-report loss/RTT uses the
existing AIMD controller; paused/closed consumers and ten-second-old bounds
are excluded. Native senders honor negotiated limits (including zero-rate
pause) and expire unrefreshed limits after ten seconds. Audio is unaffected.
This intentionally shares source quality across receivers; full subscriber
allocation, pacing and simulcast remain distinct work.

Focused regressions verify loss-driven decrease/recovery, minimum-consumer
aggregation, stale/paused exclusion, feedback quantization/bounds and a real
encrypted downstream-to-publisher limit. The four native E2E scenarios and
targeted ASan/UBSan tests pass with this path enabled.

Reports now use shared randomized, bandwidth-aware interval calculation and
independent worker timers, including while media is idle/paused. Runtime
membership uses the declared four-SSRC one-camera/one-microphone model; this is
not dynamic group reconsideration. Publisher SR timing is echoed in receiver
reports, and retiring native/SFU sending sources attempt compound BYE before
transport shutdown. Focused tests verify interval bounds, quiet-path encrypted
receiver reports and their SR references, plus callback-safe teardown.
