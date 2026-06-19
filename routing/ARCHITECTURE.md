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
Application / future SFU server
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

- Core transport owns network, ICE, DTLS, SRTP, and the SSRC -> consumer
  RTP demux. The router binds each producer's SSRC into its transport via
  `rtc_transport_bind_rtp()`; there is no router-side SSRC map, mutex, or
  global transport registry anymore — the worker loop serializes bind /
  unbind / dispatch.
- Routing owns producer registration (bind/unbind into the transport).
- Producers own consumer lists.
- Consumers own outbound RTP parameters and forwarding counters.

## Tests

| Test | Coverage |
|------|----------|
| `test_routing_transport` | Routing transport fixture over ICE-lite, DTLS/SRTP, RTP/RTCP/data hooks |
| `test_routing_ice` | Full ICE transport connecting to ICE-lite transport |
| `test_routing_media` | Producer/consumer media graph forwarding |
| `test_routing_timers` | Runtime timer behavior through routing-created transports |
