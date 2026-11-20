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
cmake --build build/fuzz --parallel 4 --target fuzz_packet_parsers fuzz_media_state
mkdir -p build/fuzz/corpus build/fuzz/media-corpus build/fuzz/findings
cp rtc/tests/fuzz_corpus/* build/fuzz/corpus/
build/fuzz/rtc/fuzz_packet_parsers build/fuzz/corpus \
  -max_total_time=60 -max_len=8192 -artifact_prefix=build/fuzz/findings/
build/fuzz/rtc/fuzz_media_state build/fuzz/media-corpus \
  -max_total_time=60 -max_len=8192 -artifact_prefix=build/fuzz/findings/
```

The harness instruments the library as well as the entry point, exercising
STUN, RTP/extensions, RTCP/feedback, SDP and ICE candidates. The media-state
driver sequences source binding/retirement, RTP/RTCP, recovery ticks, pacing,
write failures and allocation-budget failures, checking bounded resources and
cleanup. Pacer allocation tests cover failure at each object/frame/packet
allocation and recovery without resurrecting partial frames. Retain
reproducers and focused regressions. These targets do not establish full
ICE/DTLS state coverage or replace independent review.

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

Initial implementation: native and SFU periodic reports included SDES with the same random
process-local CNAME advertised in SDP. Report parsing respects sub-packet
boundaries; loss fractions use report intervals, and late pre-rollover packets
do not create a false cycle. Focused RTCP/SDP/media/routing/peer/SFU tests pass
locally. Dynamic source lifecycle/reconsideration was still required at this
stage; its subsequent implementation is recorded below. Compound reports
alone do not complete RTCP.

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
failure was not counted as a passing test. The newer explicit synthetic source
uses a silent AudioContext sink where supported, avoiding physical-speaker
clock dependence without relaxing browser policy; the actual Chromium
mesh/SFU/AUTO browser media suite passes after that change.

After removing the application authentication/deployment bundle, the native
build is warning-free and all 14 targeted signaling/app/protocol CTests pass.
The web build and four web contract tests pass, as do 15 controller tests.
Real native mesh, SFU and AUTO 2-to-3-to-2 / 4-to-5-to-4 E2E scenarios pass
with decoded media, SCTP, roster and lifecycle assertions. These checks do not
re-qualify the browser audio or external-platform gates above.

## Implementation and remaining release gates

The implementation targets the constrained unicast profile above, not every
optional WebRTC/RTP feature. Local code and test results do not close the
external release-evidence requirements:

1. Qualify supported-profile negotiation against the external browser matrix.
   The implementation validates bounded numeric/transport fields, feedback
   intersection, MID/extension directions and answer permissions, and preserves
   rejected SCTP sections without starting an association. Unsupported live
   mutations fail before changing a working connection.
2. Qualify SSRC recovery and synchronized playout under impairments.
   Authenticated local-source collision recovery now retires the old source,
   updates owners/routes/MID/stats and preserves crypto replay/nonce state.
   Publisher RTP/NTP correlation, per-publisher synchronization
   CNAME and monotonic RTT history now cross the SFU legs. Dynamic
   member/sender tracking, timer/reverse
   reconsideration, incoming/outgoing BYE and source inactivity expiry are
   implemented for the supported unicast profile. Large-group BYE backoff is
   outside this profile and remains unimplemented.
3. Qualify bounded frame-aware pacing and subscriber-wide video allocation
   under real impairments. They are enabled in the supported client/SFU path;
   audio/control stay independent, with explicit queue/resource accounting.
   Queues hold at most 256 KiB and frames at most 128 KiB. Streaming SFU
   forwarding and size-derived serialization preserve large keyframe recovery;
   the supported startup profile permits about a 10.49 Mbps keyframe peak.
   It does not promise delivery across a hard 500 kbps WAN bottleneck.
   One shared encoding still means a weak receiver can lower common source
   quality; this is not simulcast or independent subscriber encoding.
4. Qualify TURN under hostile networks. Multiple configured identities share
   four total endpoint attempts; failed allocations recover only on an explicit
   ICE restart, with new-ufrag relay candidates and retained DTLS/SRTP/SCTP.
   IPv4/IPv6 server and relay/peer combinations have local coturn coverage.
   There is one selected relay family, no NAT64 synthesis or transparent
   healthy-allocation migration.
5. Continue safety/capacity qualification. Stateful media/recovery/pacing fuzzing
   and deterministic queue-allocation failures supplement parser fuzzing,
   cancellation/teardown tests and concurrency checks. These are bounded local
   evidence, not exhaustive allocation/ICE/DTLS coverage or a security audit.

Actual Safari, Firefox, Linux/Windows qualification, long soaks and independent
protocol/security review remain open. No access-control scheme is selected by
this plan.

### Current integrated local qualification

The final protocol integration is `e70123d`; qualification automation follows
in `cf054e4`. On macOS arm64:

- The full Debug suite (99/99) and headless ASan/UBSan suite (97/97) passed
  before the final directional-negotiation merge. After that merge, the
  affected Debug tests passed 11/11, ASan/UBSan 10/10 and TSan 10/10.
- Optional TWCC/rate-control-disabled negotiation, pacing, peer, routing and
  SFU tests passed 7/7 with a warning-free build.
- The final complete Chromium/native suite passed: actual UI, per-source
  decoded VP8/Opus, SCTP, mesh/SFU/AUTO, restart, both topology transitions,
  failure/stale-epoch isolation and 120 same-process leave/join cycles.
- Final instrumented parser fuzzing executed 1,645,999 inputs and media-state
  fuzzing 1,643,285 inputs, each over 61 seconds, without findings.
- Benchmark/lifecycle-report contracts passed 23 tests; the optional compiled
  transport smoke is checked separately against each prepared Release build.
- A separate 1,000-cycle same-process run passed fresh bidirectional decode and
  exact SCTP delivery on every cycle. Sampling every ten cycles after a
  100-cycle warmup, maximum RSS increases were 1.047 MiB for the server,
  0 MiB for the observing client and 0.079 MiB for the rejoining client,
  within explicit 64/16/16 MiB allowances. Final sampled RSS was below warmup
  for all three processes. This is sampled growth evidence, not leak-freedom;
  peaks between samples and growth inside allowances can be missed.
- Firefox still fails during browser startup with `Could not find profile
  folder`, before loading the application. This is not a Firefox pass.

These are bounded local results, not the external release matrix, long loaded
soaks or a media-quality/latency certification.

### Comparative performance gate

The original pre-hardening baseline is `9a9319b`, before the first readiness
plan/code commits. Comparisons use matched Release builds and dependency hashes,
paired runs, actual decoded throughput, owned-process CPU/RSS and separately
labeled application SCTP/controller RTT and UDP/DTLS/SRTP transport latency.
Transport latency is not media capture-to-render latency.

Flag CPU/throughput regressions above 5% and RSS/latency regressions above 10%;
retain all measured deltas, noise and insufficient-evidence flags. The initial
three short baseline samples showed substantial RSS variability, so they do
not establish a no-regression result. The bounded suite uses longer warmup
and chronological AB/BA/AB pairs, recording VM pressure and unknown source
dimensions/bitrate rather than guessing them. The opt-in performance workflow
and local commands are documented in
[the interop benchmark guide](tests/interop/README.md).

### Bounded comparative result

The completed comparison used clean `9a9319b` and `0982ee1`, matched
warning-free Release builds, AppleClang 21 on macOS arm64, and the same hashed
OpenSSL 4.1.0, libvpx 1.17.0, Opus 1.6.1, usrsctp 0.9.5.0 and
libwebsockets 5.1.0 dependencies. The media workload was eight native
participants in one fixed-SFU room. Three chronological AB/BA/AB pairs each
used 60 seconds of warmup and 120 seconds of measurement. The separate
fingerprint-verified UDP/DTLS/SRTP transport workload used three pairs with
five-second warmup and 15-second measurement intervals.

All six media and six transport runs passed their functional assertions.
The performance comparison exited **1: regression**, also reporting
insufficient/noisy evidence. It is not a no-regression pass.

The table gives independently calculated medians across the three runs per
variant; totals need not equal sums of those medians. CPU is a percentage of
one logical core, not the whole machine.

| Metric | Baseline | Candidate | Relative delta |
|---|---:|---:|---:|
| Server CPU | 12.57% | 14.30% | **+13.79%** |
| Combined client CPU | 76.95% | 78.59% | +2.13% |
| Server + client CPU | 89.57% | 92.85% | +3.66% |
| Controller CPU | 1.20% | 1.18% | -1.72% |
| Server peak sampled RSS | 74.50 MiB | 83.02 MiB | **+11.43%** |
| Combined client peak sampled RSS | 200.47 MiB | 225.48 MiB | **+12.48%** |
| Total peak sampled RSS | 274.89 MiB | 303.50 MiB | **+10.41%** |
| Decoded video | 810.80 frames/s | 811.60 frames/s | +0.10% |
| Decoded audio | 2,425,133 samples/s | 2,421,589 samples/s | -0.15% |
| Application SCTP/controller RTT p50 | 43.43 ms | 43.06 ms | -0.87% |
| Application SCTP/controller RTT p95 | 56.89 ms | 56.05 ms | -1.49% |
| Application SCTP/controller RTT maximum | 68.30 ms | 64.96 ms | -4.89% |
| Transport CPU | 98.71% | 99.17% | +0.46% |
| Transport peak sampled RSS | 21.47 MiB | 21.56 MiB | +0.44% |
| Transport delivered packets | 93,152.74 packets/s | 92,606.17 packets/s | -0.59% |
| Transport delivered payload | 93.15 MB/s | 92.61 MB/s | -0.59% |
| Transport one-way p50 | 0.036 ms | 0.036 ms | 0% |
| Transport one-way p95 | 0.050 ms | 0.051 ms | +2.00% |
| Transport one-way maximum | 0.265 ms | 7.730 ms | **+2,816.98%** |

Important qualifications:

- Server CPU exceeded the 5% regression budget in all three pairs
  (+19.72%, +13.34%, +14.98%), with nonoverlapping observed ranges.
  The median increase is about 1.73 percentage points of one core.
  This is a repeatable observed cost; its code-level cause has not been
  established. Profiling and remediation remain open, not waived because
  aggregate CPU stayed below its median threshold. One total-CPU pair
  also exceeded 5% (+6.12%).
- RSS exceeded the 10% budget. Combined client peak RSS exceeded it in all
  three pairs; server/total peaks exceeded it in two pairs. Substantial
  residency variability remains: baseline server peaks ranged from
  38.86 to 79.95 MiB. These flags cannot be dismissed, but sampled RSS does
  not identify live allocations, leaks or a proven cause.
- Transport maximum latency was inconsistent: baseline run maxima were
  0.265/9.829/0.228 ms; candidate maxima were 7.730/0.247/7.822 ms.
  Two pairs exceeded budget and one improved. Retain the failed maximum
  gate; stable p50/p95 do not establish an unchanged worst case.
- Actual source dimensions and encoded bitrate were not exposed by runtime
  stats. Matched configured defaults and decoded throughput do not establish
  equal visual quality. Capture-to-render media latency was not measured.
- The measurement window excluded this task's other loaded work, but
  host-wide isolation on this shared machine was not established.

The retained evidence includes the full 37-metric delta CSV (including every
client), per-pair/noise flags, JSONL samples, comparison, manifest, executable
and dependency hashes, host/load/VM-pressure snapshots and the earlier noisy
baseline reports. Reproduction must preserve those qualifications rather than
turning functional success or a noisy result into a performance pass.

Application authentication remains deferred. No performance result can waive
protocol correctness, transport verification or the external release gates.

## Receive-source lifecycle

Native receivers validate compound framing and SR/RR/BYE bodies before
dispatch. A known-source BYE unbinds that source and resets receive statistics
and loss recovery without removing the track, participant, audio from other
sources or SCTP. Source-change callbacks run on the media
worker. The native conference controller uses them to discard the retired
source's decoder and cached receive handle.

Recently retired SSRCs are suppressed for five deterministic RTCP report
intervals (normally 25 seconds). The history holds 32 sources; exhaustion logs
an error and closes the transport instead of silently admitting stale media.
Explicit SDP binding can authorize a retired SSRC again. Replacements retire
the old binding before learning the new one; bound RTP also checks the
negotiated payload type.

Known RTP/SR/RR activity refreshes a source lease. Five deterministic report
intervals of inactivity clear a native binding, allowing a fresh same-SSRC
packet to resume it. SRTP replay/nonce state is never reset. Inbound statistics
are source-lifetime values; `in_source_generation` changes on each nonzero
binding, including same-SSRC resumption, so polling consumers can detect resets.
This does not reset cumulative application frame counters.

SRTP source tables retain 64 lifetime SSRCs per table/key without LRU eviction,
preserving old replay windows and sender rollover state across source churn.
Receive allocation happens after authentication; exhaustion closes the
transport. Outgoing RTP exhaustion returns `RTC_ERR_NOMEM`. Reclaiming this
capacity requires a new keyed transport, not ICE restart. Internal fatal
transport closure is surfaced as a failed native peer connection.

An SFU publisher BYE permanently ends that producer. Consumers clear recovery
history, stop forwarding/reporting and send BYE with their rewritten SSRC.
Late media cannot revive the producer, while other media and SCTP continue.
Reoffers may deactivate it or introduce a new SSRC; active same-SSRC restart is
rejected before mutation and requires a new source or transport. Inactivity
alone is resumable and does not destroy the producer/consumer graph. Each RTP
leg has its own activity; these leases do not promise a global end-to-end
expiry deadline. Per-transport RTCP membership is maintained separately below.

Local validation of this source-lifecycle implementation:

- Warning-free native build and 13/13 focused Debug CTests.
- 10/10 focused ASan/UBSan tests and 7/7 focused TSan tests.
- 8/8 focused tests with TWCC and rate control disabled.
- Full real Chromium/native interop suite, including mesh/SFU/AUTO decoded
  media, SCTP, migration/abort isolation and 120 leave/join generations.
- Packet-parser fuzzing: 1,504,500 inputs in 31 seconds without findings.

An earlier routing run timed out with elapsed wall time exceeding its configured
timeout. Three unchanged isolated repeats and the final runs passed; the cause
was not established. Later timed checks used process-scoped idle-sleep
prevention, not relaxed timeouts or media assertions. These results do not
replace hostile-network, long-soak or external-platform qualification.

## Live RTCP membership and sender clocks

Each logical transport now tracks its own live SSRC membership, not the room
roster or a fixed four-source estimate. Authenticated RTP SSRCs/CSRCs, RTCP
report/feedback senders and SDES chunks establish remote membership; successful
outgoing RTP/RTCP establishes local activity. Local report contexts register
their own participant before scheduling. BYE removes listed members, sender
roles expire after two calculated sender intervals, and inactive members expire
after five deterministic receiver intervals.

The shared model is bounded to 128 entries per transport; incoming capacity
exhaustion closes the transport, while local operations return a logged
resource error. This is separate from the 64-SSRC lifetime crypto tables.
Malformed SDES item/chunk bounds, alignment and feedback framing are rejected
before membership mutation. Reported-source references in RR blocks do not
create phantom members. Local and remote IDs are tracked separately;
authenticated local collision recovery retires the old identity before
selecting and registering a fresh SSRC.

Native and routing reports use live sender/receiver counts, a shared 1/16 EWMA
of protected RTCP sizes and a 5% media-target budget. Size accounting includes
conservative IPv6/UDP overhead. Timer reconsideration delays reports as a group
grows; reverse reconsideration advances the previous/next report epochs when
it shrinks. Routing timers check changes at most 100 ms apart while healthy,
rather than sleeping through a shortened deadline. This bounded polling adds
timer work and is not a media pacer. Transport statistics expose member/sender
counts. Idle senders eventually emit RR instead of indefinitely claiming SR.
Native sender/receiver objects sharing a local SSRC use one report schedule:
the SR carries the receiver's RR block instead of independently emitting a
second regular report and multiplying that source's report bandwidth.

Sender reports extrapolate the RTP clock to the report's monotonic instant
and pair it with the current NTP wall-clock timestamp. Reordered packets and
equal-timestamp fragments do not move the clock anchor backwards; RTP wrapping
and exact NTP fractions are covered by deterministic wire-format tests. Native
senders and SFU consumers supply the negotiated clock rate. This uses the
latest transmitted media anchor until a publisher SR is available. SFU
consumers then preserve and advance the publisher's RTP/NTP pair, including
NTP-era/RTP rollover, rather than substituting the SFU's wall epoch. A stable
per-publisher CNAME is shared by audio/video and kept in SDP, SDES and BYE.
Outgoing SR history calculates RTT on the local monotonic clock. Synchronized
playout and lip-sync under impairments still require qualification.

Local validation of these changes:

- Warning-free native build and 12/12 focused Debug CTests.
- 9/9 focused ASan/UBSan, 7/7 TSan and 9/9 optional-controls-disabled tests.
- Full real Chromium/native suite, including decoded media, SCTP, topology
  migration/abort isolation and 120 leave/join generations.
- 2,447,616 parser/membership fuzz inputs over 61 seconds, without findings.
- Eight peers for 60 seconds with 12 media/RSS samples and clean shutdown:
  post-warmup growth was at most 0.53125 MiB for the server and 0.4375 MiB
  for any client. This short sampled run is not long-soak/capacity certification.

## TURN identities, address failover and restart recovery

Configured TURN identities share four total endpoint attempts. DNS runs
on the configuring caller; the runtime retains unique results and puts an
alternate address family second when available. Core callers may instead
provide `rtc_turn_server_t.addresses` / `address_count`, with matching service
ports, to use their own resolver without worker-side DNS.

Retries are sequential and worker-owned, not parallel socket races. With
alternatives, each address gets a five-second allocation budget, checked every
100 ms; immediate failure advances sooner. A single address retains the
existing transaction deadlines. DNS and process scheduling are outside those
allocation budgets. The transport copies addresses, host, credentials and CA
path before returning, so retries do not borrow caller storage.

Each attempt uses its own explicitly configured protocol, credentials, server
identity and CA trust. TLS verifies that identity, including on alternate IPs;
there is no implicit plaintext or certificate-verification fallback. Each attempt starts
fresh TURN challenge/channel state. Failed I/O is unregistered before
replacement, and retry timers are cancelled on close. A failed runtime remains
in place on exhaustion so relay-only traffic cannot fall through to the direct
listener. Core transport stats expose attempts and terminal `turn_failed`.

The first successful allocation pins the address. A healthy allocation does not
move during ICE restart. A failed allocation may be replaced during an explicit
local or remote ICE restart: fresh-ufrag relay candidates are published while
DTLS, SRTP replay/nonce state and SCTP remain intact. Pool configuration is
owned until close to support this recovery.

The relay family is independent of the server connection family. IPv4 and IPv6
relay/peer paths, including IPv4-server/IPv6-relay and the converse, have local
coverage. All alternatives request the same selected relay family; simultaneous
dual-family allocations, NAT64 synthesis and transparent migration of a healthy
allocation are not promised. Real network-change/NAT and WAN qualification
remain release gates.

Local qualification:

- Warning-free native build; 21 distinct targeted runtime/native-peer CTests
  passed across focused Debug, ASan/UBSan and TSan runs.
- Real coturn UDP/TCP/verified TLS, fallback after refused/unresponsive endpoints,
  all four address slots, DNS hostnames and caller-buffer lifetime.
- Accepted-but-silent TCP/TLS endpoints verify the five-second budget and
  cancellation while the shared worker remains alive. Original normal
  allocation deadlines were not extended.
- Exhaustion cannot accept even a correctly authenticated direct nomination;
  untrusted/wrong-name certificates remain rejected after fallback.
- Media/data survive short allocation/permission/channel refresh; successful
  allocations stay pinned beyond the retry budget. Native peer tests cover
  hostname URLs, SCTP, refresh and ICE restart.
- The four real native mesh/SFU/AUTO conference E2E scenarios pass. This does
  not establish browser-forced-relay, hostile-WAN or external-platform coverage.
- Expanded identity/credential/trust, IPv6, local/remote restart and retained
  media/SCTP cases are included in the integrated native/sanitizer validation.

## Latest transport implementation

The project code is now MIT-licensed; dependencies retain their own licenses.
The worker supports read/write readiness on epoll/kqueue/select. TURN supports
UDP/TCP/TLS client-to-server legs without blocking the media worker; TLS has
strict trust-chain and server-name/IP verification. Library users configure
`rtc_transport_set_turn_server_ex()` for one identity,
`rtc_transport_set_turn_servers()` for a pool, or configured peer TURN URLs.
Stream sends are bounded to 64 KiB and 500 ms after connection; connection/TLS
setup is bounded to five seconds. DNS uses the calling thread; bounded address
alternatives and failed-allocation restart recovery are described above.

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
This intentionally shares source quality across receivers. Subscriber-wide
allocation and bounded video pacing now supplement it; simulcast and independent
per-subscriber encodings remain outside this profile.

Focused regressions verify loss-driven decrease/recovery, minimum-consumer
aggregation, stale/paused exclusion, feedback quantization/bounds and a real
encrypted downstream-to-publisher limit. The four native E2E scenarios and
targeted ASan/UBSan tests pass with this path enabled.

Reports now use shared randomized, bandwidth-aware interval calculation and
independent worker timers, including while media is idle/paused. Runtime
membership now uses the live per-transport model described above. Publisher SR
timing is echoed in receiver
reports, and retiring native/SFU sending sources attempt compound BYE before
transport shutdown. Focused tests verify interval bounds, quiet-path encrypted
receiver reports and their SR references, plus callback-safe teardown.

The optional native soak runner now checks fresh per-source decode/SCTP and
sampled RSS growth on owned processes, writing bounded-workload JSONL evidence.
A 30-second/three-peer smoke run passed locally. This is not a 24/72-hour soak,
capacity certification or a latency/quality measurement. See
[the runner contract](tests/interop/README.md#native-media-and-rss-soak).

## Latest local regression evidence

The following newer checks supersede the earlier transient Debug timeouts and
stalled synthetic browser-audio run. They were run locally on macOS/arm64.
The full native suites were rerun after the listener fix described below;
the browser and long reconnect runs used native code through `d39c7b1` and
browser source `b46eee9`. These are not remote CI/platform certification.

| Check | Result |
|---|---|
| Full Debug CTest suite, including desktop | 65/65 passed |
| Full headless Release CTest suite | 63/63 passed |
| Full headless ASan/UBSan CTest suite | 63/63 passed |
| Expanded TSan worker/network/peer/routing/relay checks | 9/9 passed |
| TWCC/AIMD-disabled focused build/tests | Clean build, 4/4 passed |
| Web contracts and TypeScript/Vite build | 4 tests and build passed |
| Controller/soak-runner failure contracts | 17/17 passed |
| Full Chromium/native `test:all` | Passed, including migration/failure isolation and 120 generations |
| Extended native reconnect run | 1,000 generations passed on `d39c7b1`; see provenance below |
| Updated parser fuzz run | 1,525,420 inputs in 60 seconds, no findings |
| Eight-person SFU/RSS run | 60 seconds, fresh per-source decode/SCTP, sampled growth gates passed |
| Two simultaneous rooms, three native peers each | 30 seconds, decoded media/SCTP/RSS and clean shutdown passed |

The eight-person run sampled server peak RSS at 66.75 MiB, with no measured
post-warmup server RSS growth and at most 0.204 MiB growth per client. Warmup
was 20 seconds; the report contains 12 samples. This is a short reference run,
not evidence of no leaks, maximum capacity, delay bounds or a long-duration SLO.

Reproduce the native suite with `ctest --test-dir build --output-on-failure
--timeout 120`, browser/native scenarios with `MRTC_BUILD_DIR=build npm --prefix
tests/interop run test:all`, and the sampled reference workload with
`MRTC_BUILD_DIR=build MRTC_SOAK_SECONDS=60 MRTC_SOAK_PEERS=8 npm --prefix
tests/interop run test:soak`.

A broader TSan TCP/TLS check subsequently detected an existing listener STUN
timer-handle race between its constructor and an immediate worker callback.
Timer creation/publication and cancellation are now serialized on the worker;
STUN hostname resolution is kept off that loop. The shared client worker's I/O
budget also accommodates eight relay allocations plus its listener. A focused
eight-peer construction regression covers that previous budget failure.

After these fixes the complete Debug suite passes again (65 tests), as do both
headless Release and ASan/UBSan suites (63 tests each) and nine focused
TSan worker/peer/routing/relay cases. The four TCP/TLS relay cases additionally
passed three consecutive TSan repetitions without suppressions. The
1,000-generation native reconnect run completed successfully against the
earlier `d39c7b1` native implementation; the later listener change has separate
focused regression coverage rather than being silently counted in that run.
