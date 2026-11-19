# mrtc — Roadmap

The immediate goal is a small, testable educational RTC stack, not a complete
browser implementation or a production conferencing service. Prefer existing
runtime primitives and bounded state over a parallel networking framework.

See [RFC_COMPLIANCE.md](RFC_COMPLIANCE.md) for actual protocol capabilities and
limits. Do not treat old percentage estimates or already-fixed bugs as new work.

## Reliability and Interoperability Baseline

The P0–P2 work is organized into independently testable areas:

| Area | Acceptance criteria |
|---|---|
| Participant lifecycle | Reuse conference/media slots; safely stop callbacks before freeing state; remove departed tiles; pass 120 full-stack reconnect generations |
| Video integrity | Shared per-frame RTP timestamps; configured frame rates; correct VP8 metadata; reject incomplete frames; bounded reorder/NACK/PLI recovery |
| Trickle ICE | Forward local candidates and end-of-candidates, apply delayed remote candidates, and surface signaling errors |
| Transport identity | Pin remote SDP fingerprints, reject mismatches, enforce DTLS 1.2 minimum, and preserve datagram boundaries |
| ICE lifecycle | Full/lite roles, role conflicts, nomination, consent expiration and active restart with media/data continuity |
| TURN | Runtime relay-only connectivity with authenticated UDP allocation, permissions/channels, refresh and cleanup against coturn |
| Live negotiation | Track/direction updates, explicit glare rejection/rollback and safe rejection of unsupported codec changes |
| Standard data channels | usrsctp SCTP/DCEP, partial reliability, fragmentation, backpressure and stream reset, verified with a browser |
| Congestion and forwarding | Apply transport estimates to encoders; bounded send/retransmission state; explicit failure/drop reporting |
| Independent SFU/server | Separate shared SCTP and SFU libraries; one launcher with control/media threads, stable-ID boundary and client-disabled server build |
| Room topology | Per-room AUTO/fixed policy, configurable P2P threshold, hold-down, epoch-scoped prepare/commit/abort, stable capture/tiles and failure fallback |
| Browser client | Real TypeScript UI using browser WebRTC in P2P/SFU/auto, including native interoperability without privacy/autoplay bypasses |
| Native apps | Shared session layer, headless media CLI with JSONL control, SDL desktop join/gallery/chat interface |
| Validation | Native/TypeScript contracts, actual shipped UI, both migration directions, abort/stale isolation, full-stack churn, CI and sanitizer coverage |

The native suite and focused ASan suites exercise these areas. The browser
harness checks real decoded audio/video and standard data-channel messages,
not merely ICE connection state or incoming packet counts. Remote CI results
remain distinct from tests run locally.

The original SFU-under-signaling layout and standalone browser test fixture
have been replaced. See [the layout](ARCHITECTURE.md),
[server coordinator](app/server/ARCHITECTURE.md),
[SFU engine](sfu/ARCHITECTURE.md) and [browser app](app/web/README.md).

## Next Priorities After This Baseline

### 1. Keep the validation matrix green

- Run the native suite on Linux, macOS and Windows.
- Run AddressSanitizer/leak detection on Linux.
- Keep browser media/data and 120-generation churn tests in CI.
- Exercise the shipped UI, default STUN shapes, normal browser privacy, and
  actual decoded sources after migration rather than a second test client.
- Reproduce failures using bounded, isolated loopback fixtures.
- Use the real headless CLI's stdio protocol for native E2E control, rather than
  maintaining a separate test participant with special reply behavior.
- Validate desktop rendering separately from headless media transport tests.
- Extend a focused regression before changing a protocol invariant.

### 2. Improve RTCP interoperability

- Preserve compound SR/RR + SDES CNAME on native and routing report paths.
- Preserve randomized bandwidth-aware small-connection intervals; add dynamic
  membership reconsideration beyond that model.
- Qualify incoming/outgoing BYE, inactivity expiry and source replacement under
  broader network faults; complete SSRC collision recovery. Compound feedback
  is implemented; reduced-size RTCP is not advertised.

These are more useful next protocol improvements than reimplementing already
working NACK, PLI, RTP extensions or replay windows.

### 3. Expand hostile-network and deployment coverage

- TURN server/address failover and STUN long-term authentication v2.
- IPv6/dual-stack relay and candidate selection.
- Network changes and NAT behavior against independent implementations.
- Fault injection across both SFU media legs, keeping queues and caches bounded.

### 4. Improve SFU efficiency without changing the model

- Per-subscriber bitrate allocation and packet scheduling informed by congestion.
- Better operational counters and explicit resource limits.
- Evaluate a single bidirectional bundled connection per participant before
  adding simulcast or more complex transport sharing.
- Introduce independent SFU processes only if crash isolation or measured
  scaling needs justify IPC, supervision and reconciliation; the stable-ID
  library boundary already separates responsibilities.

The current separate subscription connections deliberately favor straightforward
ownership. Their UDP-port cost is documented; separate DTLS associations must
not be multiplexed over an indistinguishable five-tuple.

### 5. Add media features only after reliability measurements justify them

- Opus FEC/DTX negotiation and multi-party audio quality measurements.
- FEC/RTX and optional additional video codecs.
- Simulcast/temporal layers if a concrete bandwidth use case requires them.

### 6. Treat production readiness as a separate project

The media/transport hardening target and ordered acceptance criteria are
defined in [PRODUCTION.md](PRODUCTION.md). Existing defaults stay compatible;
fixed SFU is the initial reference configuration, not a deployment profile.
Application authentication/authorization and deployment controls are deferred.

Authentication/authorization, secure signaling deployment, quotas, abuse
prevention, key lifecycle, operational tooling and independent security review
are not implied by an educational SFU or passing local interoperability tests.

## Development Rules

- Keep client and routing as siblings over the common RTC runtime.
- Make unsupported behavior explicit instead of silently accepting it.
- Include focused tests and related documentation in logical commits.
- Use the available CLI/bundled clang-format on modified C/C++ files.
- Measure actual outputs: decoded media, delivered messages, successful recovery
  and resource reuse, rather than success-shaped proxy counters.
