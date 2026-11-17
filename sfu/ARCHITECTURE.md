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

- Producer/consumer forwarding, protected-wire retransmission history, sequence
  offsets and drop counters come from the routing library.
- PLI/FIR reaches the originating publisher; downstream NACK uses consumer
  sequence space rather than blindly forwarding sequence numbers.
- Negotiated publisher TWCC drives real transport estimates for the client
  encoder path.
- Subscriber-wide bitrate allocation and simulcast selection are not provided.
- Candidate validation/classification is shared with the RTC runtime. Valid
  mDNS hosts are deferred to authenticated peer-reflexive checks; TCP
  alternatives cannot pair with UDP listeners. Malformed candidates fail.

SCTP data is emitted as an event. The coordinator interprets application
addressing and routes room messages; it is not part of RTP forwarding.

## Validation

`test_sfu_session` exercises real encrypted media/data, live publisher changes,
restart continuity, codec rejection and actual TWCC feedback. Routing tests
cover loss/retransmission and wire-history wrap behavior. The real UI
interoperability suite exercises the engine through the unified server in
fixed SFU and automatic migration modes, including a configurable 4→5→4
participant boundary with decoded audio/video from every source.
