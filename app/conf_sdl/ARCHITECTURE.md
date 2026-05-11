# Video Conference Application (conf_sdl)

Full SDL3-based video conferencing application supporting multi-peer audio/video calls.

## Architecture

```
┌───────────────────────┐
│   SDL3 Native UI      │  ui_sdl3.h/c — camera, audio, tile rendering
├───────────────────────┤
│   Conference Manager  │  conference.h/c — orchestrates peers + media
├───────────────────────┤
│   Media Pipeline      │  libmrtc_media — codecs, jitter, rate control
├───────────────────────┤
│   Peer Connections    │  libmrtc — ICE, DTLS, SRTP, RTP
├───────────────────────┤
│   Signaling Client    │  libmrtc_signaling — WebSocket meeting mgmt
└───────────────────────┘
```

## Components

### UI Layer (`ui_sdl3.h/c`)

SDL3-based capture and rendering:
- **Camera:** SDL3 camera API, captures I420 (YUV 4:2:0) frames
- **Audio:** SDL3 audio API for microphone input and speaker output
- **Tile grid:** displays up to 16 video tiles (one per peer)
- **Local preview:** own camera feed in bottom-right corner
- **Audio levels:** visual bars for mic input

### Conference Manager (`conference.h/c`)

Orchestrates signaling, peer connections, and media pipelines.

**Per-peer state:**
```c
struct conf_peer {
    char peer_id[64];
    rtc_peer_connection_t *pc;
    rtc_rtp_sender_t *video_sender;
    rtc_data_channel_t *dc;
    media_pipeline_t *recv_pipeline;
};
```

**Signaling flow:**
1. Connect to signaling server → `on_joined(my_id, peer_list)`
2. Create peer connections for existing peers
3. `on_peer_joined(peer_id)` → I am older, create offer
4. `on_offer(peer_id, sdp)` → I am newer, create answer
5. ICE candidates exchanged (currently all in SDP, no trickle)
6. On PC connected → start sending video/audio

**Media flow:**
```
Local:  camera → VP8 encoder → packetizer → send to all peers
Remote: RTP → recv_pipeline → depacketize → decode → render tile
```

### Main Loop (`main.c`)

```
init → open camera/mic (or use test pattern/tone) → start conference

while running:
    handle input (M=mute, V=camera off, Q=quit)
    capture or generate video frame → encode + send
    capture or generate audio → encode + send
    render all peer tiles + local preview
    sleep 10ms

cleanup
```

### Test Pattern / Tone Fallback

When no camera or mic is available, generates:
- Bouncing box test pattern (I420 frames)
- 440Hz sine wave test tone (PCM samples)

## Controls

| Key | Action |
|-----|--------|
| M | Toggle microphone / test tone |
| V | Toggle camera / test pattern |
| Q | Quit |

## Dependencies

- **SDL3** — camera capture, audio I/O, window rendering
- **libmrtc** — peer connections
- **libmrtc_media** — codecs, media pipeline
- **libmrtc_signaling** — signaling client
