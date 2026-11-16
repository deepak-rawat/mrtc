# Conference Library (libmrtc_conference)

High-level conferencing API over client peer connections, the media pipeline,
and WebSocket signaling. Applications push raw frames and receive decoded media
and participant callbacks; protocol algorithms stay in the RTC layer.

The default server selects SFU mode. The same API also works with a server
started with `--mesh`; `on_mode` selects the topology before the joined callback.

## Default SFU Sessions

- One publishing peer connection identified internally as `"sfu"` carries local
  media and the conference's standard SCTP/DCEP channel.
- Remote publisher IDs identify subscription peer connections. They deliver
  decoded frames under the original participant ID, not under `"sfu"`.
- Local ICE candidates and end-of-candidates are forwarded over signaling;
  incoming candidates are applied to the corresponding session.
- The publisher's bitrate-estimate callback updates the pipeline's peer budget.
  Recovery callbacks request a keyframe on the relevant receiver.
- No local media sources are required for text chat through the conference API.

The shared encoder sends once to the SFU, which performs encoded-packet fan-out.
In mesh mode the pipeline instead fans the encoded payloads out to each peer.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Application                                                 │
│    push_video/push_audio → conference → on_remote_video/audio│
├─────────────────────────────────────────────────────────────┤
│  Conference (this library)                                   │
│    ├── sources[]          — local media ("camera", "mic")    │
│    ├── peers[]            — remote participants              │
│    └── pipeline           — shared media_pipeline_t          │
├──────────┬──────────────┬───────────────┬───────────────────┤
│  media   │  rtc          │  signaling    │                   │
│  encode  │  ICE+DTLS     │  WebSocket    │                   │
│  decode  │  SRTP+RTP     │  offer/answer │                   │
│  fan-out │  data channel │  join/leave   │                   │
└──────────┴──────────────┴───────────────┴───────────────────┘
```

## Data Structures

```
conference_t
 ├── cfg (conference_config_t)          — codecs, resolution, STUN, callbacks
 ├── pipeline (media_pipeline_t*)       — shared encode/decode engine
 ├── sources[4] (conf_source_t)         — local media sources
 │    ├── label ("camera", "mic")
 │    ├── is_video, muted, active
 │    └── stream (media_send_stream_t*) — pipeline encoder handle
 ├── peers[8] (conf_peer_t)            — remote participants
 │    ├── peer_id
 │    ├── pc (rtc_peer_connection_t*)   — WebRTC connection
 │    ├── video_sender, audio_sender    — RTP senders (pipeline fans out to)
 │    ├── recv_video, recv_audio        — pipeline decoder handles (lazy)
 │    └── dc (rtc_data_channel_t*)      — chat data channel
 └── signaling (signaling_client_t*)    — WebSocket connection
```

## Send Path (every frame push)

```
conference_push_video("camera", frame)
  │
  ├─ find_source("camera") → check !muted
  │
  └─ media_pipeline_push_video(pipeline, stream_handle, frame)
       │
       ├─ VP8 encode (once)
       ├─ VP8 packetize (MTU-sized payloads)
       │
       └─ for each registered send peer:
            rtc_rtp_sender_send(alice.video_sender, payload)
               → RTP header → SRTP → UDP → alice
            rtc_rtp_sender_send(bob.video_sender, payload)
               → RTP header → SRTP → UDP → bob
```

## Recv Path (every incoming RTP packet)

```
UDP from alice → SRTP unprotect → RTP parse
  → on_recv_video_rtp(payload, seq, ts, ssrc, marker)
       │
       ├─ first packet? → pipeline.add_recv_stream("alice", ssrc)
       │                  → creates VP8 decoder, caches handle
       │
       └─ pipeline.recv_rtp(handle, payload, seq, ts, marker)
            │
            ├─ VP8 depacketize (reassemble fragments)
            ├─ VP8 decode → video_frame_t
            │
            └─ renderer callback → conference → app
                  on_remote_video("alice", "camera", decoded_frame)
```

## Peer Lifecycle

### Joining a default SFU meeting

1. Create the conference and local sources.
2. Obtain callbacks, create signaling, and call `conference_join()` to bind it.
3. Call `signaling_connect()`; the server reports `mode="sfu"` and a local ID.
4. Offer a publishing connection to `"sfu"` and apply its answer.
5. Answer server-provided subscription offers identified by remote publisher ID.
6. Reuse inactive peer/media slots when participants leave and reconnect.

`conference_get_peer_count()` counts remote participants, excluding the internal
publishing session. Text-chat applications use the same lifecycle without
adding camera/microphone sources.

### Joining a mesh meeting (`--mesh`)

```
App                     Conference              Signaling Server
 │                         │                         │
 ├─ conference_create() ──→│ create pipeline          │
 ├─ add_video_source() ───→│ pipeline.add_send_stream │
 ├─ add_audio_source() ───→│ pipeline.add_send_stream │
 ├─ get_signaling_cbs() ──→│ wire callbacks           │
 ├─ signaling_connect() ──→│────────────────────────→│
 ├─ conference_join() ────→│──── join(meeting) ─────→│
 │                         │                         │
 │                         │←── on_joined(my_id,     │
 │                         │     [alice, bob])        │
 │                         │                         │
 │                         │ for each existing peer:  │
 │                         │  alloc_peer("alice")     │
 │                         │  create_pc + add tracks  │
 │                         │  create_offer            │
 │                         │──── offer(alice, sdp) ──→│→ alice
 │                         │                         │
 │                         │←── answer(alice, sdp) ──│← alice
 │                         │  set_remote_desc         │
 │                         │  → ICE+DTLS+SRTP auto    │
 │                         │                         │
 │                         │ on_conn_state(CONNECTED):│
 │                         │  pipeline.add_send_peer( │
 │                         │    "alice", v_sender,    │
 │                         │    a_sender)              │
```

### New mesh peer arrives after us

```
Signaling → sig_on_offer("charlie", sdp)
  → alloc_peer("charlie")
  → create_pc: add video+audio tracks, wire callbacks
  → set_remote_desc(offer) → create_answer → set_local_desc
  → signaling_send_answer
  → ICE+DTLS complete → on_conn_state(CONNECTED)
    → pipeline.add_send_peer("charlie", ...)
  → on_remote_track(video) → wire on_recv_video_rtp
  → on_remote_track(audio) → wire on_recv_audio_rtp
```

### Peer leaves

```
Signaling → sig_on_peer_left("charlie")
  → destroy_peer:
    pipeline.remove_send_peer("charlie")  — stop sending
    rtc_peer_connection_close/destroy     — stop network
    pipeline.remove_peer("charlie")       — remove decoders after callbacks drain
  → app.on_peer_left("charlie")            — remove the participant's gallery tiles
```

Peer maps, slot reuse and in-flight send references are synchronized. Sends
pin their peer while using its media/data handles; teardown marks the peer
inactive and drains those references before closing the connection. Locks are
not held across synchronous worker invocation, which could deadlock against a
callback on that worker. Signaling disconnect is drained before peer storage
is destroyed. Removed receive handles must not be used after slot reuse.

## Usage Example

```c
// 1. Create conference with callbacks
conference_config_t cfg = {
    .video_codec = "VP8",
    .audio_codec = "opus",
    .video_width = 640, .video_height = 480,
    .video_fps = 30, .video_bitrate_kbps = 500,
    .stun_server = "stun:stun.l.google.com:19302",
    .callbacks = {
        .on_remote_video = my_render_video,   // decoded frames
        .on_remote_audio = my_play_audio,     // decoded samples
        .on_peer_joined  = my_on_join,
        .on_peer_left    = my_on_leave,
        .on_message      = my_on_chat,
    },
    .user_data = app,
};
conference_t *conf = conference_create(&cfg);

// 2. Add local media sources (creates encoders in pipeline)
conference_add_video_source(conf, "camera", 640, 480, 30, 500);
conference_add_audio_source(conf, "mic", 48000, 1, 32000);

// 3. Connect signaling and join meeting
signaling_config_t sig_cfg;
conference_get_signaling_callbacks(conf, &sig_cfg, "ws://server:9000", "room1");
signaling_client_t *sig = signaling_create(&sig_cfg);
conference_join(conf, sig);
signaling_connect(sig);

// 4. Main loop — push local frames
while (running) {
    video_frame_t vf = capture_camera();
    conference_push_video(conf, "camera", &vf);

    audio_frame_t af = capture_mic();
    conference_push_audio(conf, "mic", &af);

    // Mute/unmute with keyboard
    if (key_pressed('M'))
        conference_mute_source(conf, "mic", !muted);

    // Send chat message
    if (has_chat_input())
        conference_broadcast_message(conf, msg, msg_len);

    // Remote frames arrive via callbacks automatically
}

// 5. Leave and cleanup
conference_leave(conf);
conference_destroy(conf);
```

## What Conference Does NOT Do

| Concern | Who handles it |
|---|---|
| Encoding / decoding | `media_pipeline` |
| VP8 packetization | `media_pipeline` → `vp8_packetizer` |
| RTP / SRTP | `rtc_rtp_sender` / `rtc_peer_connection` |
| ICE / DTLS | `rtc_peer_connection` (automatic) |
| Frame capture / render | Application |
| Signaling transport | `signaling_client` (WebSocket) |

## API Summary

| Function | Purpose |
|---|---|
| `conference_create(cfg)` | Create conference + pipeline |
| `conference_get_signaling_callbacks(...)` | Wire signaling to conference |
| `conference_join(conf, signaling)` | Store signaling reference |
| `conference_add_video_source(label, ...)` | Create encoder in pipeline |
| `conference_add_audio_source(label, ...)` | Create encoder in pipeline |
| `conference_push_video(label, frame)` | Encode → fan-out to all peers |
| `conference_push_audio(label, audio)` | Encode → fan-out to all peers |
| `conference_mute_source(label, mute)` | Stop encoding without removing |
| `conference_remove_source(label)` | Remove source entirely |
| `conference_send_message(peer_id, ...)` | Send data channel message |
| `conference_broadcast_message(...)` | Send to all peers |
| `conference_get_peer_count()` | Count active peers |
| `conference_leave()` | Disconnect all peers |
| `conference_destroy()` | Free all resources |

## Dependencies

- **libmrtc** — rtc core (peer connection, tracks, data channels)
- **libmrtc_media** — media pipeline (encode, decode, fan-out)
- **libmrtc_signaling** — signaling client (WebSocket)

## Limits

The native facade currently negotiates VP8 video and Opus audio for each
publisher. The lower-level media pipeline and client transceiver APIs are more
general; labels alone do not add a new negotiated conference track.

| Resource | Max |
|---|---|
| Peers | 8 (`CONF_MAX_PEERS`) |
| Sources | 4 (`CONF_MAX_SOURCES`) |
| Send peers in pipeline | 8 (`MP_MAX_SEND_PEERS`) |
| Recv streams in pipeline | 16 (`MP_MAX_RECV_STREAMS`) |

## Validation

- `test_conference` exercises reusable peer slots and disconnect notifications.
- The native churn harness verifies 120 real meeting generations with media and
  SCTP messages, not just allocation/free calls.
- The local browser harness checks actual decoded media, standard data channels,
  live publisher direction/restart behavior, retained subscriptions and gallery
  cleanup.
