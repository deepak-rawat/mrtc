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
