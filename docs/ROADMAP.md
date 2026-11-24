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

## Feature TODOs

Start with the H.264 sequence; the remaining items are proposed follow-ups, not
prerequisites for unrelated features. Keep VP8/Opus, AUTO and SCTP working;
new codec paths stay opt-in until their own interoperability checks pass.

- [ ] **Next: H.264 RTP payload support (`media/`).** Use the existing
  [payload interface](../media/src/video_payload.h): send single NAL/FU-A, receive
  those plus STAP-A in mode 1. Define input framing; test access-unit round trips,
  MTU bounds, malformed input, loss and reordering. Never emit incomplete access
  units or advertise H.264 before codec support; no encoder dependency yet.
- [ ] **H.264 encoder/decoder backend.** Check licensing/platform availability;
  add an optional library through the [codec interface](../media/include/media/video_codec.h).
  Done when I420 round trips, bitrate changes, requested keyframes and SPS/PPS
  delivery pass component tests; builds without the backend retain VP8.
- [ ] **H.264 negotiation and real calls.** Select one browser-compatible profile,
  validate profile/packetization compatibility, and expose codec selection.
  Done when browser/native send and receive H.264 in mesh and SFU, including
  restart/handoff; incompatible peers fall back to VP8 or reject explicitly.
  Keep codec dependencies out of the SFU.
- [ ] **Compare H.264 with VP8.** Use identical input, resolution, FPS and target
  bitrate; record CPU, actual bitrate, spatial detail, temporal smoothness and
  capture-to-render latency. Done when trade-offs are reproducible, not RTT proxies.
- [ ] **Fix one measured media-quality problem.** Run a real mesh/SFU call under
  burst loss, seeded jitter and a one-second outage; measure audio interruption,
  concealment, freezes and recovery. Set bounds first; fix and regression-test
  the worst demonstrated failure.
- [ ] **Opus in-band loss recovery (`media/`, `client/`).** Negotiate `useinbandfec`,
  configure loss estimates, and recover missing audio from the following packet
  before its playout deadline. Done when tests distinguish FEC from PLC and
  peers without FEC retain existing timing without unconditional extra delay.
- [ ] **RTX video retransmission (`rtc/`, `client/`, `routing/`).** Negotiate RTX
  PT/`apt`/SSRC mappings and original sequence numbers across SFU legs. Done when
  browser/native loss tests recover without duplicates, with bounded history
  and independent RTX SRTP sequence/replay state; retain non-RTX NACK behavior.
- [ ] **Synchronized audio/video playout (`rtc/`, `media/`, desktop).** Connect
  sender-report RTP/NTP mappings to presentation timing, handling timestamp wrap
  and source replacement. Done when flash/tone tests meet declared lip-sync and
  latency bounds under jitter/handoff without unbounded queues.
- [ ] **Per-source call-quality diagnostics (shared app, desktop, web).** Expose
  bitrate, RTT, loss, jitter, fresh/recovered/concealed audio and freezes through
  typed snapshots and a compact view. Done when impairments identify the correct
  source and stale/unsupported values are explicit; reuse existing counters.
- [ ] **Automatic call recovery (shared app, conference, `client/`).** Add bounded
  signaling retries/backoff and ICE restart on media-path failure. Done when
  outages recover fresh media/chat without duplicate participants, leave cancels
  retries and exhaustion shows an error; never reset live SRTP replay/nonce state.
- [ ] **Adaptive temporal/spatial video quality (`media/`, shared app).** Use
  existing bandwidth estimates to adjust FPS (temporal smoothness) and resolution
  (spatial detail), with motion/detail presets, bounds and hysteresis. Done when
  bandwidth falls/rebounds change quality without flapping or starving audio.
- [ ] **Bandwidth probing and recovery (`rtc/`).** Extend existing GCC/TWCC with
  bounded probe bursts for startup and capacity recovery, accounting for probe
  traffic in pacing and feedback. Done when capacity increases raise usable
  bitrate without persistent queues, excessive loss or audio interruption.
- [ ] **Native echo cancellation/noise suppression (`media/`, desktop).** Select
  a platform-compatible backend; process microphone input using speaker playback
  as the echo reference, with device-delay handling and an explicit bypass.
  Done when speakerphone/double-talk tests suppress echo without damaging speech.
- [ ] **Per-subscriber video quality (`client/`, SFU).** Add negotiated simulcast
  spatial encodings and supported codec temporal layers; select by receiver
  bandwidth and requested display size. Done when mixed-bandwidth peers receive
  suitable layers without degrading everyone, unbounded queues or decode errors.
- [ ] **Layout generation (desktop, web).** Add gallery, active-speaker, pinned
  participant and presentation-focused modes; generate rectangles from viewport,
  source aspect ratios and participant count. Done when resize/join/leave/share
  tests preserve tile identity, keep controls visible and avoid speaker flapping.
- [ ] **Measure capacity before changing room limits.** Compare 2/4/8-peer SFU
  rooms and multiple rooms at equivalent load. Raise limits only after sustained
  decoded media, bounded resource growth and explicit headroom are demonstrated.

## Ongoing tracks, not feature-development prerequisites

These continue alongside the TODOs; they do not need to be globally "finished"
before starting the next feature. A regression caused by a change blocks that
change's merge. An unqualified platform/path remains a release/support limitation.

| Track | Concrete recurring work |
|---|---|
| Reliability | Run affected restart/migration/media tests per change; turn observed failures into focused regressions; schedule churn, soak and sanitizer runs for lifecycle changes |
| Browser/device qualification | Compare pinned and current Chromium audio; run Firefox and actual Safari separately; record real microphone/camera results and exact unsupported combinations |
| Performance | Re-measure historical flags against a frozen current revision using the paired procedure; profile only reproduced hotspots and retain inconclusive results |
| Design clarity | Keep ownership and codec/transport boundaries explicit while adding features; remove duplication exposed by the change, not through a separate framework rewrite |

Production release still requires qualification and deployment safeguards.

## Conditional future work

These are deferred scope, not features to remove or commitments to implement.
Revisit only when a concrete use case, interoperability gap or measurement
justifies the complexity; preserve the working baseline.

| Capability | Revisit when |
|---|---|
| RFC 8888 congestion feedback | A target peer needs its wire format or comparison shows value beyond current TWCC/REMB and RR-based control |
| VP9/AV1 and further codec options | Quality/bandwidth gains justify backend, CPU, platform and negotiation costs; complete the H.264 TODOs first |
| Additional spatial SVC modes | Measured simulcast cost justifies codec-specific inter-layer dependencies and switching complexity |
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
