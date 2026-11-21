# Roadmap

Build a minimal, reliable and performant WebRTC stack for excellent audio/video
conferencing, with understandable code and room to experiment with new media
technologies and protocols.

## Current baseline

VP8/Opus/SCTP, native/browser mesh/SFU/AUTO calls, restart, topology migration
and authenticated UDP/TCP/TLS TURN are implemented. Local Chromium/native E2E
coverage includes 120 reconnects; broader platform qualification and historical
performance regression flags remain open. See [standards](STANDARDS.md) for
support boundaries, not certification.

## Ordered conferencing priorities

| Priority | Work | Exit criterion |
|---|---|---|
| 1 | Reliable baseline | Cross-platform/browser calls, reconnect/migration regressions and repeatable CPU/memory/latency measurements |
| 2 | Clear, extensible design | Explicit ownership, simple codec/transport boundaries and less duplication; targeted refactoring, not a framework rewrite |
| 3 | Better media quality | Measured loss/jitter recovery, congestion response, audio continuity, A/V sync and latency |
| 4 | H.264 support | Backend/licensing decision, negotiation, RTP packetization, browser/native interoperability and quality/CPU comparisons |
| 5 | Larger conferences | Resolve measured bottlenecks/regressions, test larger rooms and raise limits only with capacity/headroom evidence |
| 6 | Further codecs and advanced media | Select codecs, simulcast/layering and bandwidth improvements from demonstrated conferencing needs |

Measure performance from priority 1 onward; maintain design clarity throughout.

## Conditional future work

These are deferred scope, not features to remove or commitments to implement.
Revisit only when a concrete use case, interoperability gap or measurement
justifies the complexity; preserve the working baseline.

| Capability | Revisit when |
|---|---|
| RFC 8888 congestion feedback | A target peer needs its wire format or comparison shows value beyond current TWCC/REMB and RR-based control |
| VP9/AV1 and further codec options | Quality/bandwidth gains justify backend, CPU, platform and negotiation costs; H.264 remains priority 4 |
| Simulcast/SVC | Different subscriber bandwidths or larger rooms need independently selectable encodings/layers |
| Additional TURN behavior | Deployment evidence requires TCP media relays, NAT64, simultaneous dual-family allocations or healthy-allocation migration; retain existing UDP/TCP/TLS server legs and failover |
| Broader PeerConnection negotiation | An application needs fuller Unified Plan behavior or established codec mutation beyond the supported conferencing model |
| Large-group RTP/RTCP details | The actual RTP-session topology needs large-group BYE backoff or other scale-specific behavior; retain reports, membership, consent and source retirement |

Optional for our product scope does not mean optional for a conforming WebRTC
endpoint. Keep omitted requirements explicit in [standards](STANDARDS.md).

## Parallel experimentation

- Start early: explore codecs, congestion algorithms, media processing and new
  transports/protocols alongside the conferencing priorities.
- Give each experiment a question, bounded prototype, correctness checks and
  comparison against the baseline.
- Keep immature/incompatible paths opt-in; preserve working conferencing.
- Promote results after interoperability, reliability and performance checks;
  retire unsuccessful prototypes instead of accumulating permanent complexity.

## Engineering principles and safeguards

Prefer clear code, bounded resources, focused tests and measured improvements
over feature completeness or abstraction for its own sake. A useful conferencing
stack and an accessible experimentation platform should reinforce each other.
Production use still requires platform/network/endurance qualification,
access controls, secure deployment and independent review. SFU encryption is
hop-by-hop, not participant E2EE; configured limits are not measured capacity.

Commands: [testing](TESTING.md) and [performance](PERFORMANCE.md).
