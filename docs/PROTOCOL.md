# Application protocols

Two independent versioned interfaces: room WebSocket messages use **v2**;
native CLI JSONL control uses **v1**. Standard WebRTC transports are described
in [standards](STANDARDS.md); these application contracts do not replace them.

## Room signaling

The server owns membership and topology. Clients must not switch based only
on their local roster count. [C message helpers](../signaling/include/signaling/signaling_msg.h)
and [web protocol tests](../app/web/tests/protocol.test.mjs) define matching encodings.

| Field | Contract |
|---|---|
| `version` | 2; absent means legacy v1, usable only with fixed-mode compatibility |
| `peer_id` | Stable joined-call participant; ASCII letters/digits/`_`/`-`, 1..15 bytes |
| `display_name`, `peer_names` | Presentation only; optional mapping of peer IDs to names |
| `epoch` | Positive uint32, assigned by coordinator for a topology generation |
| `session_id` | One connection's offer/answer/candidates; fewer than 64 bytes |
| `to`, `from` | Participant addressing; `"sfu"` addresses publishing |
| `peers` | Other participants, excluding recipient; rooms admit at most eight |

| Session role | Deterministic ID |
|---|---|
| Mesh pair | `m:<epoch>:<smaller-id>:<larger-id>` |
| SFU publisher | `p:<epoch>:<publisher-id>` |
| SFU subscription | `s:<epoch>:<subscriber-id>:<publisher-id>` |

Mesh IDs compare ASCII bytes, not locale order; the smaller peer initially
offers. SDP/candidates carry `epoch` and `session_id`. ICE restart retains the
session ID; ICE username fragments identify its new connectivity generation.

```json
{"type":"join","version":2,"meeting":"demo","display_name":"Alice","capabilities":{"mesh":true,"sfu":true,"topology_switch":true}}
```
`joined` returns `peer_id`, concrete `mode` (`mesh`/`sfu`), `epoch`, `peers` and
optional `peer_names`. `auto` is policy, never an announced active graph.
`peer_joined`/`peer_left` report membership; `offer`/`answer`/`candidate` carry
session addressing. Empty candidate means end-of-candidates. Malformed fields,
unsupported versions, bounds and invalid ownership fail explicitly.

### Topology transaction

Messages carry `version`, target `epoch`, and `mode`:

| Message | Meaning |
|---|---|
| `topology_prepare` | Build pending graph beside active; includes current peer roster |
| `topology_ready` | Client has required usable pending media/data sessions |
| `topology_commit` | Select pending graph for sending/playback |
| `topology_ack` | Selection complete; old sessions may drain/retire |
| `topology_abort` | Discard pending graph; `reason` explains failure |
| `topology_request` | Request server-authorized SFU fallback after actual P2P failure |

AUTO upgrades above `--p2p-max` and delays downgrades; muted participants count.
Forced modes do not initiate transitions. Membership changes/timeouts reconcile
or abort pending work without destroying the active call. Syntactically valid
stale generations are not applied to current sessions.
Readiness requires flowing pending media, not transport state alone; intentionally
muted/inactive tracks must not cause impossible waits. Capture/participant/tile
identity survives. Select audio once; remote endpoint changes require new SCTP
associations. At most active and pending/retiring graph generations coexist.

### Conference data envelope

SCTP/DCEP remains standard; the room's application payload is:

| Bytes | Content |
|---|---|
| 0..3 | ASCII `MRC2` |
| 4..19 | NUL-terminated ID, zero-padded to 16 bytes |
| 20..27 | Nonzero uint64 sender message ID, network byte order |
| 28..end | Application payload |

Mesh puts sender ID in the ID field. SFU publishers put destination ID or `*`;
the coordinator replaces it with sender ID before forwarding and retains the
message ID. Bounded sender/message-ID deduplication handles overlapping graphs,
not exactly-once delivery across arbitrary failures. Legacy `MRTC` has the same
ID field but no message ID; fixed-mode readability is not version negotiation.

Sends select the active topology, not pending/retiring channels. Native and
browser messages fit the 65535-byte channel limit including the envelope.
Browser chat rejects admission above 1 MiB of buffered output before fan-out;
native channels retain their bounded queue. Send success is not a delivery ack.

## CLI control

Start `./build/app/cli/mrtc_cli --control stdio`. stdin/stdout are one JSON object
per line; stderr carries logs. Keep reading stdout: backpressure can pause ticks.
[control.c](../app/cli/control.c) validates fields; [main.c](../app/cli/main.c)
owns I/O and events.

Every command requires `version:1`, `id`, and `command`. IDs are echoed unchanged:
nonempty UTF-8 string <=64 bytes or JavaScript-safe integer. Fields/names are
case-sensitive. Unknown/duplicate fields, wrong types/versions, embedded NUL
and malformed JSON fail; null is not an omitted optional field.

| Command | Additional fields |
|---|---|
| `join` | Required `server`, `room`, `name`, `video`, `audio`; optional `stun`, `data_only` |
| `send_message` | `text`; optional `peer_id` for targeted delivery, omitted broadcasts |
| `mute` | `source` = `video`/`audio`; boolean `muted`; selected source required |
| `stats` | None; return current snapshot |
| `leave` | None; idempotent |
| `quit` | None; respond, stop session, exit |

`server` uses `ws://`/`wss://`, <512 bytes. Room/name: 1..63 bytes.
Video/audio: `synthetic` or `none`; neither source means receive-only with chat.
`data_only:true` requires both `none` and disables receiving media too.
`stun`: <=255 bytes; omitted uses default STUN, empty disables discovery.
Text: 1..4096 bytes; target peer ID: 1..15 bytes.

```json
{"version":1,"id":"j1","command":"join","server":"ws://127.0.0.1:8080","room":"demo","name":"Alice","video":"synthetic","audio":"synthetic","stun":""}
{"version":1,"id":2,"command":"stats"}
```

### Results, events and readiness

Every output carries `version`, `event` (`type` alias), `generation`, `epoch`.
Exactly one correlated `result` per complete input line; unusable ID becomes null.
Success has `ok:true`; failure has `ok:false`, human `error`, negative native `code`.

```json
{"version":1,"event":"result","type":"result","id":"j1","ok":true,"generation":1,"epoch":0,"data":{"accepted":true}}
```

Join success means **accepted**, not joined; its result precedes callback events.
Wait for `joined`, active topology, then fresh decoded media/SCTP readiness.
An active call rejects another join; leave first. Accepted send is not a remote
delivery ack. Unready/missing peer returns `-5`; incomplete broadcast returns
`-1` and may have reached some recipients. Retry targeted messages carefully.
EOF processes a final unterminated line, then stops the session.

| Event | Important fields |
|---|---|
| `joined`, `peer_joined`, `peer_left` | `peer_id` |
| `topology` | `mode`, `status` (`switching`/`active`/`error`), target/event `epoch`, `reason` |
| `message` | Sender `peer_id`, `text`, `truncated` |
| `error` | `text` and `error` |

Invalid UTF-8/NUL/oversized received text is represented by a valid prefix with
`truncated:true`. A topology abort leaves committed snapshot mode/epoch intact.

### Snapshot and bounds

`stats` returns the snapshot as both `result.stats` and `result.data`:

| Fields | Meaning |
|---|---|
| `generation`, `epoch` | Local start attempt versus committed server topology generation |
| `active`, `joined`, `status`, `error` | Lifecycle; acceptance/transport state is not delivery |
| `local_id`/`peer_id`, `name`, `topology`/`mode` | Aliases and current identity |
| `video_muted`, `audio_muted` | Explicit local source state |
| `peers[]` | Up to eight: `id`/`peer_id`, `name`, `connected`, media counters, `epoch` |
| `sources[]` | Up to 32: `peer_id`, `label`, media counters, `generation`, `epoch` |

Media counters are `video_frames`, `audio_frames`, `audio_samples` (per channel),
counting decoded callbacks, not RTP/local sends. They reset on committed epochs,
not prepare/abort; departure removes peer/source entries. Validated starts advance
`generation` and clear snapshots; invalid/already-active starts do not.
Stop retains final identity/counters but marks inactive/not joined.

Input limit: 8192 bytes excluding LF; oversized lines discard through newline
and yield one error. Results: <=65536 serialized bytes; event queue: 128 entries,
<=32768 bytes each. Overflow reports error and stops rather than growing forever.
At most 512 input bytes are processed between main-loop ticks.
