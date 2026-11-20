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

Authenticated collisions rotate a consumer's **local outbound** SSRC and its
sequence mapping, not the immutable publisher source. The transport sends BYE
for the old ID and purges queued/repair packets; the consumer clears ciphertext
history, restarts sender counters/SR history, increments `source_generation`,
and requests an upstream keyframe after releasing its media mutex. Publisher
clock correlation and CNAME survive. `get_rtp_parameters` and stats expose the
current local source. Producers use separately owned random RR reporter IDs;
colliding with one changes only `local_ssrc`, not incoming routes or counters.

Subscription forwarding emits the negotiated subscriber MID, configured with
`rtc_consumer_set_mid`, rather than the publisher MID or consumer object ID.
MID uses validated one-byte extension IDs and 1..16-byte values. An active
consumer cannot change it without pausing/draining media first.

The consumer caches accepted **protected wire packets** in 128 sequence
positions. NACK retransmission uses `rtc_transport_retransmit_rtp()` rather
than re-encrypting old packets across rollover counters. Retransmission attempts
are bounded; missing history can request an upstream keyframe through the
producer. PLI/FIR similarly target the originating producer, not an unrelated
consumer SSRC.

Reports run on independent owning-worker timers, including during idle or
paused media, rather than waiting for another RTP packet. Report intervals use
the shared RFC 3550 A.7 calculation with randomized initial/steady minima and a
5% media-target budget with live per-transport member/sender counts. Healthy
timers recheck within 100 ms so reverse reconsideration can advance reports
after departures. Shared packet-size averaging and sender/member expiry live
in the transport, not independent per-producer membership estimates.
Publisher SR timing
is retained for RR LSR/DLSR. Closing consumers cancels report callbacks and
attempts BYE before freeing their state; the SFU retires them before closing
their transport.

Validated publisher BYE permanently marks the producer ended, cancels reports
and ends its consumers. Consumers clear protected retransmission history, stop
media/feedback processing and emit BYE using their rewritten SSRC if they sent
RTP. Ended sources cannot be resumed or acquire new consumers. Source state is
exposed in producer/consumer statistics; SRTP replay/nonce state is preserved.

Five deterministic report intervals without known RTP/SR/RR activity instead
expire the producer's active-source lease and RTCP receive-report statistics,
not its graph, identity or cumulative forwarding counters. Fresh activity may
resume this source. The upstream and downstream
legs have independent activity; downstream reporting context can remain while
the upstream source is inactive.

There is no unbounded per-subscriber queue. Closed/not-ready transports and
failed sends produce checked failures and drop counters. Opt-in transport
pacing stages complete video frames with bounded dynamic storage; the generic
routing default remains immediate. Queued packets do not inflate public sent
counters or sender-report packet/octet counts. Dropped/retired queued frames
cannot be repaired from cached ciphertext.

Video consumers reuse the core AIMD controller for receiver-report loss/RTT
estimates and accept single-source REMB when offered. Producers aggregate the
minimum fresh active-consumer limit and send negotiated REMB for the original
publisher SSRC, never the downstream rewritten SSRC. Paused/closed consumers
and estimates older than ten seconds do not hold back the source.

`rtc_consumer_allocate_video` fairly splits a subscriber's aggregate video
budget across up to 64 legs, including separate transports/workers. The caller
supplies the room/subscriber/epoch-scoped set and owns the consumers throughout
the call. The default pool sums fresh leg estimates, capped at 2.5 Mbps per
leg, with 500 kbps per new leg or expired estimate. Thus adding normal sources
does not divide a fixed 500 kbps between them. An explicit aggregate cap,
including zero, is also supported. Paused/ended sources and sources idle ten
seconds are excluded; feedback is stale after ten seconds. Audio is untouched.
Zero allocation suppresses only that subscriber's video and excludes its
bound from shared publisher adaptation. Resumption requests a keyframe.

This is conservative shared-source adaptation: one weak receiver can lower
video quality for all subscribers of that source. The pool coordinates existing
per-leg estimates; their sum is not an independently measured shared-bottleneck
capacity estimate. There is no simulcast or independent per-subscriber source
quality. Allocations are atomically published before producer feedback, and
producer worker invocation occurs without any consumer media mutex held.
Bounds and allocation/feedback timestamps are exposed in consumer statistics.

Consumer SR timestamps use the publisher's RTP/NTP mapping once received,
advancing both clocks at the negotiated rate without substituting the SFU wall
clock. Consumers created after a publisher SR are seeded with that mapping.
Audio/video from one publisher share a random synchronization CNAME; different
publishers do not. SDP, regular reports and BYE compounds keep it consistent.
Before a publisher SR arrives, the newest send anchor is used provisionally.
RTT uses bounded local monotonic SR history, not the foreign NTP wall epoch.

## Tests

| Test | Coverage |
|------|----------|
| `test_routing_transport` | Routing transport fixture over ICE-lite, DTLS/SRTP, RTP/RTCP/data hooks |
| `test_routing_ice` | Full ICE transport connecting to ICE-lite transport |
| `test_routing_media` | Forwarding, real routed VP8 loss/NACK recovery, sequence wrap, bounded history and failed-send accounting |
| `test_routing_timers` | Runtime timer behavior through routing-created transports |
