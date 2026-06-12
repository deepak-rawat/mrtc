# mrtc — Roadmap

Priority order is based on security risk, interop impact, and user-visible
product gaps, with RFC_COMPLIANCE.md as the source of truth.

## P0: Security and Correctness (do first)

### 1) SRTP replay protection (RFC 3711 §3.3.2)

- Why: marked as critical in RFC compliance; currently allows packet replay.
- Deliverables:
    - Add replay window tracking per SRTP context.
    - Reject duplicate/too-old packet indices before decrypt/auth.
    - Unit tests for in-order, out-of-order, and replayed packets.
- Primary files:
    - rtc/src/rtc_srtp.c
    - rtc/src/rtc_srtp.h

### 2) Fix known protocol bugs from RFC compliance report

- Why: correctness and interop hazards already identified.
- Deliverables:
    - STUN IPv6 mapped-address length validation fix.
    - TURN auth key truncation fix (binary key length-safe HMAC input).
    - Keep nonce rotation behavior explicit (client/server policy).
- Primary files:
    - rtc/src/rtc_stun.c
    - rtc/src/rtc_turn.c
    - signaling/server/* (nonce policy)

## P1: Mid-call negotiation and ICE lifecycle

### 3) Mid-call renegotiation (RFC 3264 / RFC 8829 gaps)

- Why: major product gap (audio-only call cannot be upgraded to audio+video
    without recreating peer connection).
- Current limitation: set_local_desc/set_remote_desc reject updates after
    connect_started.
- Deliverables:
    - Allow repeated offer/answer cycles after connected state.
    - Add negotiation-in-progress state and glare handling policy.
    - Apply transceiver deltas safely (add/remove/dir changes) post-connect.
    - Attach newly added senders to live transport during renegotiation.
    - Add rollback or defined error policy for failed renegotiation.
- Primary files:
    - client/src/rtc_peer.c
    - client/include/rtc/rtc_peer.h
    - rtc/src/rtc_sdp.c

### 4) Post-connect ICE restart (RFC 8445 §12, RFC 8829)

- Why: required for network changes, NAT rebinding recovery, and long-lived
    calls.
- Current limitation: restart_ice is only allowed pre-connect.
- Deliverables:
    - Support ICE restart on active connection (new ufrag/pwd in re-offer).
    - Re-run ICE checks without full peer recreation.
    - Preserve media/data continuity across successful restart.
    - Add restart failure handling and fallback behavior.
- Primary files:
    - client/src/rtc_peer.c
    - rtc/src/rtc_ice.c
    - rtc/src/rtc_sdp.c

## P2: ICE and TURN interop completeness

### 5) SFU server completion and hardening

- Why: SFU is not complete yet and is required for reliable multi-party scale.
- Deliverables:
    - Complete server application flow on top of router/producer/consumer APIs.
    - Robust publisher/subscriber lifecycle (join, leave, reconnect, teardown).
    - Per-subscriber forwarding policy (keyframe request path, backpressure handling).
    - Multi-party stability targets (long-run calls, churn, packet loss).
    - Operational controls (basic metrics, health logs, failure surfacing).
- Primary files:
    - conference/src/*
    - routing/src/*
    - app/conf_sdl/*
    - signaling/server/*

### 6) TURN relay candidates wired into ICE (RFC 5766/8656 + RFC 8445)

- Why: relay reachability is incomplete until TURN candidates participate in
    normal checklist/nomination.
- Deliverables:
    - Feed TURN allocated relayed candidates into ICE gather output.
    - Permission/channel/allocation refresh timers.
    - Validate against coturn in end-to-end tests.
- Primary files:
    - rtc/src/rtc_turn.c
    - rtc/src/rtc_ice.c

### 7) Full local trickle ICE and end-of-candidates (RFC 8863/RFC 8839)

- Why: reduces setup latency and improves browser interop.
- Deliverables:
    - Emit local candidates incrementally.
    - Emit explicit end-of-candidates signaling.
    - Update checklist as new remote candidates arrive.
- Primary files:
    - rtc/src/rtc_ice.c
    - client/src/rtc_peer.c
    - rtc/src/rtc_sdp.c

### 8) ICE roles, nomination, and checklist behavior (RFC 8445)

- Why: current behavior is simplified and always controlling.
- Deliverables:
    - Implement controlled role and role-conflict resolution.
    - Implement regular nomination behavior.
    - Improve pair ordering and triggered checks.
- Primary files:
    - rtc/src/rtc_ice.c
    - client/src/rtc_peer.c

## P3: SDP and RTCP interop improvements

### 9) SDP feedback and options completeness

- Why: improves interop consistency with peers that rely on explicit SDP
    capability lines.
- Deliverables:
    - Add a=rtcp-fb generation/parsing coverage where missing.
    - Add a=end-of-candidates support in SDP/event model.
    - Track RFC 8866 migration notes in docs.
- Primary files:
    - rtc/src/rtc_sdp.c
    - client/src/rtc_peer.c

### 10) RTCP compound packet and interval compliance (RFC 3550)

- Why: current fixed periodic behavior is simplified.
- Deliverables:
    - Compound RTCP support (SR/RR + SDES where required).
    - Adaptive RTCP interval/timer behavior.
    - Compliance tests for mixed media sessions.
- Primary files:
    - rtc/src/rtc_rtcp.c
    - client/src/rtc_peer.c

## P4: Media resilience and feature expansion

### 11) UlpFEC (RFC 5109)

- Why: packet-loss robustness for video on poor links.
- Deliverables:
    - FEC encode/decode path.
    - SDP negotiation lines for ulpfec.
    - Interop validation in loopback and lossy-network tests.
- Primary files:
    - media/src/*
    - rtc/src/rtc_sdp.c

### 12) VP8 payload extension completeness (RFC 7741)

- Why: broader interop with peers using picture-id extensions and temporal
    layers.
- Deliverables:
    - 2-byte PictureID support.
    - Parse/use optional descriptor extension bits where needed.
- Primary files:
    - media/src/vp8_packetizer.c
    - media/src/vp8_packetizer.h

## Cross-cutting validation plan

For each priority band above, add/extend tests in parallel:

- Renegotiation: audio-only to audio+video upgrade, and reverse.
- ICE restart: post-connect network switch and recovery.
- SFU: multi-peer join/leave churn, publisher handoff, and relay stability.
- TURN relay: coturn-backed end-to-end call establishment.
- Security: SRTP replay rejection and STUN/TURN bug regression tests.

Suggested test targets:

- client/tests/*
- rtc/tests/*
- conference/tests/*
- routing/tests/*
- signaling/tests/*
