# mrtc — Architecture

## Layer Diagram

```
┌──────────────────────────────────────┐
│          Applications                │  app/ (chat, conf_sdl)
├──────────────────────────────────────┤
│          Conference                  │  conference/ (peer lifecycle, fan-out)
├──────────────────────────────────────┤
│          Media Pipeline              │  media/ (codecs, jitter, packetizer)
├──────────────────────────────────────┤
│   Client Peer API  │  Routing API    │  client/, routing/
├──────────────────────────────────────┤
│ Runtime Transport Core               │  worker, listener, logical transport
├──────────┬───────────┬───────────────┤
│   SDP    │  RTP/RTCP │  SRTP + DC    │  protocol primitives
├──────────┴───────────┴───────────────┤
│        DTLS (OpenSSL 3.0+)           │
├──────────────────────────────────────┤
│   ICE/STUN checks + TURN Client      │
├──────────────────────────────────────┤
│     Packet I/O (epoll/kqueue/select) │
├──────────────────────────────────────┤
│     Platform Abstraction             │  common/ (sockets, threads, logging)
└──────────────────────────────────────┘
         ↕ WebSocket signaling ↕         signaling/ (client + server)
```

## Design Philosophy

Follows the **Pion model**: the `rtc` core is codec-agnostic and operates at the
RTP payload level. Senders take encoded payloads. Receivers deliver raw RTP
payloads. The `media` library is a separate layer providing codecs, packetizers,
and pipeline orchestration — it depends on `rtc` and uses its typed
`rtc_rtp_sender_t*` interface directly (no callback indirection).

The runtime transport core is always built into `libmrtc`. The optional
`libmrtc_client` layers the WebRTC-style peer-connection facade, tracks, data
channels, and stats on top. The optional `libmrtc_routing` layers server-side
router / producer / consumer primitives on top. Client and routing are siblings:
neither depends on the other.

All public API functions use the `rtc_` prefix.

## Project Structure

```
├── CMakeLists.txt          # Top-level build (project: mrtc)
├── common/                 # Platform abstractions (libmrtc_common)
│   ├── include/rtc/        #   rtc_common.h — sockets, threads, errors, logging
│   └── src/
├── rtc/                    # Runtime transport core (libmrtc) — always built
│   ├── include/rtc/        #   rtc_worker.h/listener.h/transport.h,
│   │                       #   RTP/RTCP/SDP helpers, rtc.h
│   ├── src/                #   private helpers (rtc_stun.h, rtc_turn.h), ICE, DTLS,
│   │                       #   SRTP, RTP, RTCP, SDP, rate control, NACK, TWCC, BWE
│   └── tests/              #   runtime + protocol test executables
├── routing/                # Server-side routing primitives (libmrtc_routing)
│   ├── include/rtc/        #   rtc_router.h, rtc_producer.h, rtc_consumer.h
│   ├── src/                #   router, producer, consumer implementation
│   └── tests/              #   routing test executables
├── client/                 # WebRTC-style peer-connection facade (libmrtc_client)
│   ├── include/rtc/        #   rtc_peer.h, rtc_track.h, rtc_data_channel.h,
│   │                       #   rtc_stats.h, rtc_client.h (umbrella)
│   ├── src/                #   peer, track, data channel, shared client runtime
│   └── tests/              #   test_peer, test_data_channel, test_rtp_sender_loopback
├── media/                  # Media library (libmrtc_media)
│   ├── include/media/      #   media_pipeline.h, video_codec.h, audio_codec.h, video_stats.h
│   ├── src/                #   vp8_packetizer.h (private), jitter_buffer, video_debug/dump
│   └── tests/              #   8 test executables
├── signaling/              # Signaling (libmrtc_signaling + server)
│   ├── include/signaling/  #   signaling_client.h
│   ├── src/                #   signaling_msg.h (private)
│   ├── server/             #   signaling_server executable
│   └── tests/              #   2 test executables
├── conference/             # Conference library (libmrtc_conference)
│   ├── include/conference/ #   conference.h — multi-peer orchestration
│   └── src/
├── app/                    # Applications
│   ├── chat/               #   chat — text messaging over data channels
│   └── conf_sdl/           #   conf_sdl — SDL3 video conference app
└── tests/
    └── test_harness.h      # Shared test macros
```

## Component Documentation

Each component has its own `ARCHITECTURE.md` with detailed design, API, and usage:

| Component | File | Description |
|---|---|---|
| Common | [common/ARCHITECTURE.md](common/ARCHITECTURE.md) | Platform abstractions, types, logging |
| RTC | [rtc/ARCHITECTURE.md](rtc/ARCHITECTURE.md) | Core RTC runtime + protocol primitives |
| Routing | [routing/ARCHITECTURE.md](routing/ARCHITECTURE.md) | Server-side router / producer / consumer primitives |
| Client | [client/ARCHITECTURE.md](client/ARCHITECTURE.md) | WebRTC-style peer connection facade |
| Media | [media/ARCHITECTURE.md](media/ARCHITECTURE.md) | Codecs, packetizer, media pipeline |
| Signaling | [signaling/ARCHITECTURE.md](signaling/ARCHITECTURE.md) | WebSocket client + server |
| Conference | [conference/ARCHITECTURE.md](conference/ARCHITECTURE.md) | Multi-peer orchestration |
| Video App | [app/conf_sdl/ARCHITECTURE.md](app/conf_sdl/ARCHITECTURE.md) | SDL3 conference application |

## How a WebRTC Connection Works

1. **Signaling** — Exchange SDP offer/answer between peers via WebSocket
2. **Runtime Candidate Setup** — The shared listener exposes local UDP candidates and ICE credentials
3. **ICE Connectivity Checks** — Logical transports probe each other's candidates with STUN
4. **DTLS Handshake** — Encrypted channel established over the working ICE pair
5. **SRTP Key Export** — DTLS provides keying material for SRTP
6. **Media Flow** — RTP packets encrypted with SRTP flow over the selected logical transport

## Library Dependencies

```
app/conf_sdl ──→ mrtc_conference ──→ mrtc_client ──→ mrtc ──→ mrtc_common
                       │                                       │
                       ├──→ mrtc_media ──→ mrtc_client ──────┘
                       │
                       └──→ mrtc_signaling ──→ mrtc_common

mrtc_routing ──→ mrtc ──→ mrtc_common

app/chat ──→ mrtc_client ──→ mrtc ──→ mrtc_common
       └──→ mrtc_signaling
```

External dependencies:
- `mrtc_common` → OpenSSL (crypto)
- `mrtc` → OpenSSL (SSL + crypto)
- `mrtc_client` → (transitively via `mrtc`)
- `mrtc_routing` → (transitively via `mrtc`)
- `mrtc_media` → libvpx, libopus
- `mrtc_signaling` → libwebsockets, cJSON
- `app/conf_sdl` → SDL3
