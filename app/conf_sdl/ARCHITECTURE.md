# Video Conference Application (conf_sdl)

SDL3 video conferencing application using the default SFU server, or explicit
legacy mesh mode selected by the server.

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

Remote frame buffers, tile lookup, and audio levels are protected by a mutex
shared with the render thread. The `on_peer_left` conference callback marks
all of that peer's tiles for removal; the next render removes their index
entries, frees pending frames, and destroys textures on the SDL main thread.
Inactive slots are reused when participants join later.

### Conference Manager (`conference.h/c`)

Orchestrates signaling, peer connections, and media pipelines.

The application uses the public conference API rather than maintaining a second
peer/media graph. Conference owns the publishing/subscription connections and
shared pipeline; UI tiles are keyed by participant and source label.

**Signaling flow:**
1. Connect to signaling server → `on_joined(my_id, peer_list)`
2. The conference selects SFU or mesh behavior from the server's mode event
3. In SFU mode, publish to `"sfu"` and answer subscription offers for remote peers
4. Forward asynchronous ICE candidates and end-of-candidates in both directions
5. On connection readiness, send local frames and render decoded subscriptions

**Media flow:**
```
Local:  camera → VP8 encoder → packetizer → SFU publisher (or mesh fan-out)
Remote: subscription RTP → shared pipeline → depacketize → decode → render tile
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
