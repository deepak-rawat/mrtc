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
│          Peer Connection API         │  rtc/ (codec-agnostic transport)
├──────────┬───────────┬───────────────┤
│   SDP    │  RTP/RTCP │  SRTP + DC    │
├──────────┴───────────┴───────────────┤
│        DTLS (OpenSSL 3.0+)           │
├──────────────────────────────────────┤
│        ICE Agent + STUN/TURN         │
├──────────────────────────────────────┤
│     Transport (epoll/kqueue/select)  │
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

All public API functions use the `rtc_` prefix.

## Project Structure

```
├── CMakeLists.txt          # Top-level build (project: mrtc)
├── common/                 # Platform abstractions (libmrtc_common)
│   ├── include/rtc/        #   rtc_common.h — sockets, threads, errors, logging
│   └── src/
├── rtc/                    # Core transport library (libmrtc)
│   ├── include/rtc/        #   rtc_peer.h, rtc_track.h, rtc_sdp.h, rtc_stun.h
│   ├── src/                #   rtc_rtp.h (private), ICE, DTLS, SRTP internals
│   └── tests/              #   12 test executables
├── media/                  # Media library (libmrtc_media)
│   ├── include/media/      #   media_pipeline.h, video_codec.h, audio_codec.h
│   ├── src/                #   vp8_packetizer.h (private), jitter_buffer, rate_control
│   └── tests/              #   6 test executables
├── signaling/              # Signaling (libmrtc_signaling + server)
│   ├── include/signaling/  #   signaling_client.h
│   ├── src/                #   signaling_msg.h (private)
│   ├── server/             #   signaling_server executable
│   └── tests/              #   2 test executables
├── conference/             # Conference library (libmrtc_conference)
│   ├── include/conference/ #   conference.h — multi-peer orchestration
│   └── src/
├── turn/                   # TURN server executable
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
| RTC | [rtc/ARCHITECTURE.md](rtc/ARCHITECTURE.md) | Core WebRTC protocol stack |
| Media | [media/ARCHITECTURE.md](media/ARCHITECTURE.md) | Codecs, packetizer, media pipeline |
| Signaling | [signaling/ARCHITECTURE.md](signaling/ARCHITECTURE.md) | WebSocket client + server |
| Conference | [conference/ARCHITECTURE.md](conference/ARCHITECTURE.md) | Multi-peer orchestration |
| TURN | [turn/ARCHITECTURE.md](turn/ARCHITECTURE.md) | TURN relay server |
| Video App | [app/conf_sdl/ARCHITECTURE.md](app/conf_sdl/ARCHITECTURE.md) | SDL3 conference application |

## How a WebRTC Connection Works

1. **Signaling** — Exchange SDP offer/answer between peers via WebSocket
2. **ICE Gathering** — Each peer discovers its network addresses (host, server-reflexive)
3. **ICE Connectivity Checks** — Peers probe each other's candidates with STUN
4. **DTLS Handshake** — Encrypted channel established over the working ICE pair
5. **SRTP Key Export** — DTLS provides keying material for SRTP
6. **Media Flow** — RTP packets encrypted with SRTP flow over the ICE transport

## Library Dependencies

```
app/conf_sdl ──→ mrtc_conference ──→ mrtc ──→ mrtc_common
                       │                         │
                       ├──→ mrtc_media ──→ mrtc ──┘
                       │
                       └──→ mrtc_signaling ──→ mrtc_common

app/chat ──→ mrtc ──→ mrtc_common
       └──→ mrtc_signaling
```

External dependencies:
- `mrtc_common` → OpenSSL (crypto)
- `mrtc` → OpenSSL (SSL + crypto)
- `mrtc_media` → libvpx, libopus
- `mrtc_signaling` → libwebsockets, cJSON
- `app/conf_sdl` → SDL3
