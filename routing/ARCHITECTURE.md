# Routing Primitives (libmrtc_routing)

`libmrtc_routing` is the server-side media routing layer built on top of
`libmrtc`. It owns the RTP media graph concepts that do not belong in the core
transport runtime: routers, producers, consumers, SSRC demux, and forwarding
statistics.

The core `libmrtc` transport receives encrypted packets, performs ICE / DTLS /
SRTP, parses RTP / RTCP, and exposes callbacks. Routing decides what incoming RTP
packets mean and which consumers should receive them.

## Layer Role

```
Application / signaling_server SFU
        |
        v
mrtc_routing        router, producer, consumer, fan-out primitives
        |
        v
mrtc                worker, listener, transport, RTP/RTCP/SDP helpers
        |
        v
mrtc_common         sockets, threads, logging, platform types
```

`mrtc_routing` is a sibling of `mrtc_client`. Client peer connections do not use
routers, producers, or consumers; they create core transports directly.

## Public Headers

- `rtc_router.h` — media router lifecycle and transport creation helper
- `rtc_producer.h` — inbound media source on a router transport
- `rtc_consumer.h` — outbound media sink consuming a producer

## Packet Flow

```
UDP -> mrtc transport -> SRTP unprotect -> RTP parse
    -> transport RTP demux (SSRC -> producer)
    -> producer stats + consumer fan-out
    -> consumer re-packetize/re-SSRC
    -> mrtc transport SRTP protect + send
```

## Ownership

- Core transport owns network, ICE, DTLS, SRTP, and the SSRC -> stream
  RTP demux. The router binds each producer's SSRC into its transport via
  `rtc_transport_bind_rtp()`; there is no router-side SSRC map, mutex, or
  global transport registry anymore — the worker loop serializes bind /
  unbind / dispatch.
- Routing owns producer registration (bind/unbind into the transport).
- Producers own consumer lists.
- Consumers own outbound RTP parameters and forwarding counters.

## Forwarding and Recovery

Consumers use a stable sequence offset and their own SSRC. Source sequence
gaps, reordering and wraparound remain visible downstream; compacting every
received packet into a contiguous sequence would hide an incomplete VP8 frame.
Discontinuous source restarts require a new producer/consumer.

The consumer caches successful **protected wire packets** in 128 sequence
positions. NACK retransmission uses `rtc_transport_retransmit_rtp()` rather
than re-encrypting old packets across rollover counters. Retransmission attempts
are bounded; missing history can request an upstream keyframe through the
producer. PLI/FIR similarly target the originating producer, not an unrelated
consumer SSRC.

Reports run on independent owning-worker timers, including during idle or
paused media, rather than waiting for another RTP packet. Report intervals use
the shared RFC 3550 A.7 calculation with randomized initial/steady minima and a
5% media-target budget in the four-SSRC connection model. Publisher SR timing
is retained for RR LSR/DLSR. Closing consumers cancels report callbacks and
attempts BYE before freeing their state; the SFU retires them before closing
their transport.

There is no unbounded per-subscriber queue. Closed/not-ready transports and
failed sends produce checked failures and drop counters. This is a simple
forwarder, not a simulcast selector or a full subscriber bitrate allocator.

Video consumers reuse the core AIMD controller for receiver-report loss/RTT
estimates and accept single-source REMB when offered. Producers aggregate the
minimum fresh active-consumer limit and send negotiated REMB for the original
publisher SSRC, never the downstream rewritten SSRC. Paused/closed consumers
and estimates older than ten seconds do not hold back the source.

This is conservative shared-source adaptation: one weak receiver can lower
video quality for all subscribers of that source. It leaves audio untouched,
does not add a media queue and is not per-subscriber layer selection, pacing or
allocation across all of a subscriber's connections. Bounds are exposed in
producer/consumer statistics for qualification.

## Tests

| Test | Coverage |
|------|----------|
| `test_routing_transport` | Routing transport fixture over ICE-lite, DTLS/SRTP, RTP/RTCP/data hooks |
| `test_routing_ice` | Full ICE transport connecting to ICE-lite transport |
| `test_routing_media` | Forwarding, real routed VP8 loss/NACK recovery, sequence wrap, bounded history and failed-send accounting |
| `test_routing_timers` | Runtime timer behavior through routing-created transports |
