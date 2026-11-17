# mrtc — Architecture

The project separates capture/UI, room policy and encoded-media forwarding.
Native and browser clients share a wire protocol, not an implementation of
WebRTC. The browser uses its own media engine.

## Layers

```text
Native SDL/chat apps                         Browser UI
        |                                       |
  conference controller                   browser RoomClient
        |                                       |
  media + native client                   built-in WebRTC
        |                                       |
        +------------- WS control --------------+
                            |
                 mrtc_server room coordinator
                  /                       \
         signaling protocol             SFU session engine
         HTTP/WS control loop                  |
                                        routing graph
                                        producer/consumer
                                              |
                       shared RTC runtime + shared SCTP
                       worker/listener/transport/protocols
                                              |
                                     common portability
```

In mesh mode, media goes directly between clients; the server coordinates
signaling. In SFU mode, each participant publishes once and the server forwards
encoded media to its subscribers. The SFU decrypts/re-encrypts per hop but does
not decode and composite everyone into a new video.

## Source Layout

```text
common/             sockets, threads, logging and containers
rtc/                runtime/protocols; independent mrtc_sctp backend
client/             native PeerConnection facade
media/              native VP8/Opus encoding, decoding and media pipeline
routing/            low-level producers, consumers and forwarding
sfu/                independent publisher/subscription session engine
signaling/          JSON room protocol and WebSocket client
conference/         native participants, capture and epoch-scoped sessions
app/
  server/           unified launcher, room policy and HTTP/WS coordinator
  conf_sdl/         native capture/playback/gallery
  chat/             text client using conference orchestration
web/                real browser UI and TypeScript room controller
tests/
  interop/          Playwright driving the actual shipped UI + native clients
  test_harness.h    native standalone test macros
```

The old executable name is retained at
`build/signaling/signaling_server`; it uses the same entrypoint as
`build/app/server/mrtc_server`. A CMake target alias alone would not preserve
that executable path, so the compatibility target is built explicitly.

## Library Dependencies

```text
mrtc_common
    ^
  mrtc                 core transport/protocols, no codec or SCTP dependency
    ^
    +-- mrtc_sctp      usrsctp endpoint/channel implementation
    |       ^
    |       +-- mrtc_client
    |       +-- mrtc_sfu
    |
    +-- mrtc_routing --^

mrtc_media       -> mrtc_client + VP8/Opus
mrtc_signaling   -> mrtc_common + libwebsockets/cJSON
mrtc_conference  -> mrtc_client + mrtc_media + mrtc_signaling
mrtc_server     -> mrtc_signaling + mrtc_sfu (when routing enabled)
```

The native client and server-side routing remain siblings. The SFU depends on
routing and shared SCTP, **not** on the native client facade or media codecs.
Its public API exchanges stable IDs, SDP/candidate data and events; it does not
accept WebSocket handles or meeting-manager objects.

The routing-disabled server supports explicit mesh mode. The client-disabled
server can still run SFU/auto. These are validated build profiles, not just
separate directories.

## Thread and Lifetime Ownership

| Owner | Responsibilities |
|---|---|
| Server control/main loop | WebSocket/HTTP, participants, room policy and topology transactions |
| SFU media worker | ICE/DTLS/SRTP/SCTP sessions, producers/consumers, timers and packet forwarding |
| Native application thread | Capture, encoder pushes and SDL rendering |
| Native signaling thread | Room/session control events; peer operations marshal to RTC worker |
| Native RTC worker | Socket dispatch, receive/decode callbacks and protocol timers |
| Browser room controller | UI/capture/session selection; built-in WebRTC owns network/media processing |

This is one server process with separate control and media ownership, not two
independently supervised processes. Existing worker marshalling is reused.
SFU events are copied to the control queue before their callback storage expires.
Native sends pin session lifetime and do not hold locks across synchronous
worker invocation. Shutdown drains callbacks before freeing reached objects.

Runtime hot sends use atomic readiness and synchronized SRTP state. No producer
or consumer owns a thread; the worker serializes its media graph.

## Room Topology

The server's default policy is AUTO with a configurable P2P maximum of two.
Each room has independent active/desired mode, epoch and transition phase.
Muted participants count; transport count does not determine policy.

```text
MESH active -> prepare SFU -> commit/ack -> SFU active
SFU active  -> hold-down -> prepare mesh -> commit/ack -> MESH active
                     failure -> abort pending, preserve active
```

The coordinator announces a concrete mesh/SFU mode, never `auto` as an active
media graph. P2P failure can latch SFU preference to avoid repeated failed
downgrades. Membership changes and transaction deadlines are reconciled on the
control loop.

## Participants Are Not Connections

A participant ID stays stable during a joined call. A transport session has a
separate role, epoch and session ID:

- Mesh: one canonical pair session; smaller ASCII ID offers initially.
- SFU: one publishing session and subscription sessions per other publisher.
- During preparation, active and pending/retiring generations may coexist.

SDP/candidates are scoped by epoch/session and ICE username fragments. Retiring a
graph is not a participant leave. Native capture/encoders and browser capture
tracks belong to the call, not to an old connection being closed.

Pending video/audio decodes without being selected twice for playback. Commit
changes the selected graph while preserving participant tiles. Application
message IDs and bounded deduplication protect overlapping receive paths; a new
remote endpoint requires a new SCTP association.

## Component Details

- [Common](common/ARCHITECTURE.md)
- [RTC and shared SCTP](rtc/ARCHITECTURE.md)
- [Native client facade](client/ARCHITECTURE.md)
- [Media](media/ARCHITECTURE.md)
- [Routing](routing/ARCHITECTURE.md)
- [SFU engine](sfu/ARCHITECTURE.md)
- [Signaling](signaling/ARCHITECTURE.md)
- [Unified server](app/server/ARCHITECTURE.md)
- [Native conference](conference/ARCHITECTURE.md)
- [SDL app](app/conf_sdl/ARCHITECTURE.md)
- [Browser app](web/README.md)
- [Wire protocol](signaling/PROTOCOL.md)

See [README.md](README.md) for running and build profiles. Independent process
isolation, production authorization, sophisticated subscriber allocation,
simulcast and full RFC coverage remain separate work.
