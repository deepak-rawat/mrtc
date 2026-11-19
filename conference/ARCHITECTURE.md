# Native Conference Controller — libmrtc_conference

The public API stays above native PeerConnection, media and WebSocket helpers.
Applications push raw frames and receive decoded media/message/status callbacks.
The server chooses each room's topology; the controller does not independently
count members and switch modes.

## Participants, Sources and Sessions

These are separate identities:

- **Participant:** stable remote ID/name for the joined call and gallery.
- **Local source:** capture/encoder state, surviving a transport change.
- **Session:** one epoch/role-scoped PeerConnection, senders, receivers and SCTP.
- **Graph:** the selected mesh/SFU sessions or one pending/retiring generation.

Session IDs follow [the shared wire contract](../signaling/PROTOCOL.md).
An SFU publishing session is not an extra remote participant. A subscription
identified by a remote participant still connects to the server, not directly
to that participant.

The native sender pipeline shares encoders and fans encoded data out to active
and preparing sessions. Receive pipelines belong to sessions so old/new decoder
state cannot mix. This intentionally favors straightforward ownership over
packing every decoder into a single global receive array.

Receive-source callbacks retire the old SSRC's decoder/jitter state and cached
receive handle on the owning worker after BYE, inactivity expiry or replacement.
The next source lazily creates fresh receive state. Participant identity,
capture and cumulative application frame counters survive; source retirement
does not tear down the call or its SCTP channel.

## Public Lifecycle

```c
conference_config_t config = {
    .display_name = "Alice",
    .stun_server = "stun:stun.l.google.com:19302",
    .callbacks = {
        .on_remote_video = render_video,
        .on_remote_audio = play_audio,
        .on_message = receive_message,
        .on_topology = topology_changed,
        .on_error = report_error,
    },
    .user_data = app,
};
conference_t *conference = conference_create(&config);
conference_add_video_source(conference, "camera", 640, 480, 30, 500);
conference_add_audio_source(conference, "mic", 48000, 1, 32000);

signaling_config_t callbacks;
conference_get_signaling_callbacks(conference, &callbacks,
                                    "ws://127.0.0.1:8080/ws", "demo");
signaling_client_t *signaling = signaling_create(&callbacks);
conference_join(conference, signaling);
signaling_connect(signaling);
/* Capture loop: conference_push_video / conference_push_audio. */

conference_leave(conference);
signaling_destroy(signaling);
conference_destroy(conference);
```

Check return values in real applications. Bind signaling before connecting:
the server may return a joined event before the connect call returns.
Callbacks execute on internal threads; SDL operations must be handed to the
main thread.

`conference_get_signaling_callbacks()` advertises protocol v2, both concrete
topologies and live switching. Legacy protocol-v1 events can be used in fixed
modes, not to silently enter an automatic room.

## Fixed Modes

Mesh creates one pair session per remote participant; the smaller ASCII ID
makes the initial version-2 offer. Each sender has its own RTP/SRTP state.

SFU creates one publishing session plus subscriptions for remote publishers.
Native sources are encoded once for publishing. With no local sources the
publishing session is SCTP-only, which supports text chat/audience clients.
Remote media subscriptions remain possible.

ICE candidates/end-of-candidates, data channels and bandwidth-estimate/keyframe
callbacks are installed for the corresponding session. Bare STUN hostname/IP,
explicit STUN URL and disabled-STUN settings remain supported.

## Live Topology Changes

```text
active graph remains selected
       |
topology_prepare -> build pending graph -> topology_ready
       |                                      |
       +<----------- abort on failure --------+
                                              |
                               topology_commit / topology_ack
                                              |
                                  select new; retire old
```

Active and pending/retiring graphs have independent session keys. A delayed
answer/candidate cannot be applied to a replacement connection for the same
participant.

Pending video/audio decodes without being selected twice for application
render/playback. Readiness includes connected transport/data and usable pending
media for recently flowing sources (freshness window 1500 ms). Muted, inactive
and absent sources do not create an impossible media wait. Source diagnostics
also verify actual decoding after handoff.

Commit preserves participant IDs, local sources and encoders. Closing an old
graph does not signal a participant leave or destroy capture. Abort destroys
only pending sessions, reports a reason, and keeps the selected graph working.
Genuine P2P failure requests SFU fallback from the authoritative coordinator.

## Concurrency and Data

Session lifetime is pinned around sends. Teardown marks a session inactive,
removes it from routing and drains send/callback references before freeing it.
Locks are not held across synchronous worker invocation. Signaling disconnect
is drained before conference destruction.

Version-2 application data uses the `MRC2` envelope and a room-wide sender
message counter. Bounded per-participant deduplication handles overlapping
receive paths; new sends select one graph. SCTP associations cannot be moved
to a different endpoint merely by changing a topology flag.

## Status and Diagnostics

- `on_topology(mode, status, epoch, reason)` reports concrete mesh/SFU mode and
  active/switching/error status.
- `conference_get_peer_count()` counts participants, not internal connections.
- `conference_get_peer_name()` copies the signaling display name (or ID fallback)
  under the participant lock. Names survive transport-graph handoffs.
- `conference_get_source_stats()` snapshots active-rendered video/audio counts
  per participant, excluding pending decode.
- Mode selection alone does not prove that media has decoded; tests check both.

The native facade currently negotiates VP8 and Opus. Lower-level transceivers
and the media library are more general; source labels alone do not add another
negotiated conference track.

## Validation

`test_conference` covers slot reuse, default STUN shapes, negotiated media
parameters, send/leave/callback races, staged commit/abort/stale events,
readiness and data deduplication. SDL tests cover thread-safe status and tile
handoff. Real UI/native tests cover fixed modes, live 2→3→2 migration, mute/data
cases, media/data continuity and 120 native call generations.
