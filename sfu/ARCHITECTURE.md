# SFU Session Engine — libmrtc_sfu

This component owns publishing/subscription media sessions. Room membership,
WebSocket connections, topology policy, JSON envelopes and chat destinations
belong to the [server coordinator](../app/server/ARCHITECTURE.md), not this library.

## Dependency Boundary

```text
server coordinator
       |
   mrtc_sfu
    /     \
routing   shared SCTP
    \     /
  RTC runtime
```

No native `mrtc_client`, SDL or codec library is required. The server-only
client-disabled build verifies that boundary. The SFU forwards encoded payloads;
it does not run VP8/Opus encoders or mix pixels/audio.

## Public Interface

[rtc_sfu.h](include/rtc/rtc_sfu.h) uses room, participant, session and epoch IDs.
Control calls are serialized by the caller and marshalled to the media worker.
Events execute on that worker; callers copy event data before returning and
deliver signaling responses through their own control loop.

Initialize RTC and shared SCTP globally before creating an engine. Close and
destroy it before global cleanup. `rtc_sfu_leave(..., epoch)` can retire one
generation; epoch zero removes all generations for a participant.

The event/command boundary carries no borrowed `lws` or meeting-peer pointers.
This keeps session lifetime independent of a WebSocket object's storage and
leaves a future IPC deployment possible without implementing an RPC framework
now.

## Media Graph

Each incoming video/audio track has one producer. Each subscriber gets a
consumer for that track on its subscription transport.

Subscription offers negotiate the one-byte MID extension. Forwarded packets
carry that subscription m-section's MID, including after a local consumer SSRC
collision. Consumer rotation preserves the publisher, room participant and SCTP
association; fresh offers obtain the current SSRC from the consumer. Publisher
source SSRCs remain immutable, independently of rotatable local RR reporter IDs.

```text
Alice publishing transport -> Alice camera producer
                                    |
                          +---------+---------+
                          |                   |
                      Bob consumer       Carol consumer
                          |                   |
                    subscription         subscription
                     transport            transport
```

Publishing sessions share a listener. Subscription sessions have separate
listeners so different DTLS associations do not collide when a native client
shares its local socket. This trades more UDP ports for simple ownership.
The worker socket budget must cover these listeners, not just publishers.
`max_io_sources` defaults to two full eight-person room generations plus the
shared listener; the server sizes it for all 32 rooms and overlapping epochs.

Compatible publisher track/direction changes and ICE restart retain the
established transport/SCTP association. Existing codec/PT/MID order/SCTP port
constraints remain explicit. This is different from topology migration, which
creates new epoch-scoped sessions.

## Recovery and Congestion

- Publisher BYE permanently ends only that producer and propagates BYE in each
  consumer's rewritten SSRC space. Late media is ignored; audio from other
  sources, participant identity and SCTP remain. New subscriptions represent
  ended sources as inactive rather than failing the room join.
- Active same-SSRC reoffers after BYE are rejected before mutation; deactivation
  or a new SSRC follows the existing update path. A discontinuous restart needs
  a new source or transport, not reset SRTP replay state. Inactivity expiry alone
  is resumable and preserves the media graph.
- Producer/consumer forwarding, protected-wire retransmission history, sequence
  offsets and drop counters come from the routing library.
- PLI/FIR reaches the originating publisher; downstream NACK uses consumer
  sequence space rather than blindly forwarding sequence numbers.
- Negotiated publisher TWCC drives real transport estimates for the client
  encoder path.
- Every 200 ms, subscription sessions pool active video legs for the same
  room, subscriber and epoch, excluding publishing sessions. The routing
  allocator shares the aggregate fresh AIMD/REMB budget fairly. New normal
  legs each contribute the existing 500 kbps default rather than dividing a
  fixed per-connection budget. Stale/paused/ended legs do not consume shares.
  This coordinates leg estimates, not independent shared-path capacity
  measurement. Simulcast selection is not provided.
- Native publisher streams and SFU consumer streams use bounded, frame-aware
  transport pacing by default; audio and control bypass video staging.
  SFU streaming mode forwards the eligible prefix while the rest of a frame
  arrives, including frames assembled over more than 200 ms. Its 200 ms
  packet queue limit is independent of its bounded frame assembly lease.
- Negotiated single-source REMB and receiver-report AIMD estimates propagate
  the lowest fresh active-subscriber video bound upstream. This deliberately
  chooses common source quality rather than independent subscriber layers;
  audio remains independent. A real encrypted feedback test checks the
  downstream-to-original SSRC mapping and resulting publisher send target.
- Candidate validation/classification is shared with the RTC runtime. Valid
  mDNS hosts are deferred to authenticated peer-reflexive checks; TCP
  alternatives cannot pair with UDP listeners. Malformed candidates fail.

SCTP data is emitted as an event. The coordinator interprets application
addressing and routes room messages; it is not part of RTP forwarding.

## Validation

`test_sfu_session` exercises real encrypted media/data, live publisher changes,
restart continuity, codec rejection and actual TWCC feedback. Its real VP8
regression uses negotiated PT 117 and 640×480 high-entropy VP8 at 500 kbps,
checks both paced legs' actual writes, and exercises incomplete-frame expiry,
sampling-clock recovery and downstream mid-frame cancellation while audio and
SCTP remain available. Native encoding retries startup overshoot before RTP.
Deterministic pacer tests verify token spacing, burst limits, resource bounds,
128 KiB atomic / 120,000-byte relayed frame delivery, streamed one-second
assembly without whole-frame double pacing, partial-frame expiry and repair
cancellation without sleeps.
Routing tests
cover loss/retransmission and wire-history wrap behavior. The real UI
interoperability suite exercises the engine through the unified server in
fixed SFU and automatic migration modes, including a configurable 4→5→4
participant boundary with decoded audio/video from every source.
