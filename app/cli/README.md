# Native CLI

`mrtc_cli` is a headless client of the same native session used by the desktop
application. It sends paced synthetic video (320×240, 15 fps, `camera`) and
audio (48 kHz mono, 20 ms frames, `mic`) by default. It requires no capture or
playback hardware. Decoded remote media is counted, not played.

```sh
build/app/cli/mrtc_cli --room demo --name Alice
build/app/cli/mrtc_cli --room demo --no-interactive --duration 15 --events jsonl
build/app/cli/mrtc_cli --control stdio
```

Options:

- `--server URL`: default `ws://127.0.0.1:8080`.
- `--room ROOM` (`--meeting` is an alias), `--name NAME`: room is required
  outside control mode; default name is `native`.
- `--stun HOST_OR_URL`: default `stun:stun.l.google.com:19302`; `--stun ""`
  disables STUN for local/offline use.
- `--video synthetic|none`, `--audio synthetic|none`: independently select
  outgoing sources. `none` does **not** disable receiving that kind of media.
- `--data-only`: no outgoing **or incoming** audio/video. Also selects both
  outgoing sources as `none`.
- `--duration SECONDS`: exit after 1–86400 seconds, including control mode.
- `--no-interactive`: do not read stdin outside control mode.
- `--events jsonl`: emit asynchronous events as JSONL instead of human status.
- `--control stdio`: read the version 1 protocol below; all stdout is JSONL.
- `--help`: print usage to stderr.

Interactive input supports `status`, `mute video`, `unmute video`, `mute audio`,
`unmute audio`, and `quit`. `/status` and `/quit` are aliases; any other nonempty
line is broadcast chat. EOF and termination signals shut down gracefully.
The compatibility `chat` executable uses this frontend with data-only defaults.

## Stdio control protocol, version 1

Each command is one JSON object per line, with required `version:1`, `id`, and
`command`. IDs are nonempty strings of at most 64 UTF-8 bytes or integers in
JavaScript's safe integer range. They are echoed unchanged in results.
Field names and command names are case-sensitive. Unknown fields, duplicate
fields, invalid types, unsupported versions, malformed JSON, and overlong
values produce an error result. JSON null is not an omitted optional field.
Embedded NUL characters are rejected.

Every output has `version`, `event`, `generation`, and `epoch`. `type` is an
alias of `event`. Only the main thread writes stdout; diagnostics go to stderr.
No prompt, progress message, media frame, or library log is written to stdout.

### Commands

```json
{"version":1,"id":"j1","command":"join","server":"ws://127.0.0.1:8080","room":"demo","name":"Alice","video":"synthetic","audio":"synthetic","stun":""}
{"version":1,"id":2,"command":"send_message","text":"Hello"}
{"version":1,"id":3,"command":"send_message","text":"Private hello","peer_id":"a1b2c3"}
{"version":1,"id":4,"command":"mute","source":"video","muted":true}
{"version":1,"id":5,"command":"stats"}
{"version":1,"id":6,"command":"leave"}
{"version":1,"id":7,"command":"quit"}
```

`join` requires `server`, `room`, `name`, `video`, and `audio`. `video` and
`audio` must be `synthetic` or `none`. Optional `stun` defaults to the shared
STUN URL; an empty string disables STUN. Optional `data_only:true` disables
receiving and requires both `video` and `audio` to be `none`. Both sources
`none` without `data_only` is a receive-only media participant with chat.

`server` must use `ws://` or `wss://` and be shorter than 512 bytes. `room` and
`name` must be nonempty and shorter than 64 bytes. `stun` is at most 255 bytes.
Messages must contain 1–4096 bytes; optional `peer_id` must contain 1–15 bytes.
`mute.source` must be `video` or `audio` and `muted` must be a JSON boolean.
Muting a source that was not selected is an error.

### Results

Exactly one correlated result is emitted for each complete input line:

```json
{"version":1,"event":"result","type":"result","id":"j1","ok":true,"generation":1,"epoch":0,"data":{"accepted":true}}
{"version":1,"event":"result","type":"result","id":4,"ok":false,"generation":1,"epoch":1,"error":"command failed","code":-5}
```

Malformed input without a usable ID has `id:null`. Errors have `ok:false`, a
human-readable `error`, and a negative native `code`. A send attempted before
the data channel is open, or to a missing peer, returns code `-5` and
`error:"peer not ready or not open"`; the caller can retry after readiness.
An accepted send is not a remote delivery acknowledgement.
Broadcast aggregates failures as code `-1` with
`"broadcast incomplete: peer not ready or send failed"`. A failed broadcast
may already have reached some peers; use targeted sends if retrying individual
recipients without duplicate delivery is important.

Join success means **accepted**, not joined. The result is written before
draining any callbacks caused by that command. Wait for the separate `joined`
event, then active topology and media/chat readiness as appropriate. Joining
an already active call is rejected; use `leave` before another `join`.
`leave` is idempotent. `quit` emits its result, stops the session, and exits.
EOF processes a final unterminated line, if present, then stops the session.

### Asynchronous events

```json
{"version":1,"event":"joined","type":"joined","generation":1,"epoch":1,"peer_id":"a1b2c3"}
{"version":1,"event":"peer_joined","type":"peer_joined","generation":1,"epoch":1,"peer_id":"d4e5f6"}
{"version":1,"event":"peer_left","type":"peer_left","generation":1,"epoch":1,"peer_id":"d4e5f6"}
{"version":1,"event":"topology","type":"topology","generation":1,"epoch":2,"mode":"sfu","status":"switching","reason":""}
{"version":1,"event":"message","type":"message","generation":1,"epoch":1,"peer_id":"d4e5f6","text":"Hello","truncated":false}
{"version":1,"event":"error","type":"error","generation":1,"epoch":1,"text":"Connection failed","error":"Connection failed"}
```

Topology events forward `mode` (`mesh` or `sfu`), `status`
(`switching`, `active`, or `error`), target/event `epoch`, and `reason`.
The session snapshot keeps its **committed** epoch and mode while switching.
An abort leaves the committed epoch unchanged and reports topology `error`;
it does not imply the old media graph has stopped working. A transport or
conference error is also forwarded as an `error` event when the underlying
API emits that callback. Received messages exceeding 4096 bytes, containing
NUL, or containing invalid UTF-8 are represented by a valid text prefix with
`truncated:true`.

### Stats and identity

`stats` returns a snapshot as both `result.stats` and `result.data`. Shape:

```json
{
  "generation":1,"epoch":1,"active":true,"joined":true,
  "video_muted":false,"audio_muted":false,
  "local_id":"a1b2c3","peer_id":"a1b2c3","name":"Alice",
  "topology":"mesh","mode":"mesh","status":"active","error":"",
  "peers":[
    {"id":"d4e5f6","peer_id":"d4e5f6","name":"Bob","connected":true,
     "video_frames":30,"audio_frames":100,"audio_samples":96000,"epoch":1}
  ],
  "sources":[
    {"peer_id":"d4e5f6","label":"camera","video_frames":30,
     "audio_frames":0,"audio_samples":0,"generation":1,"epoch":1},
    {"peer_id":"d4e5f6","label":"mic","video_frames":0,
     "audio_frames":100,"audio_samples":96000,"generation":1,"epoch":1}
  ]
}
```

`local_id`/`peer_id`, `topology`/`mode`, and per-peer `id`/`peer_id` are aliases.
Names come from signaling, with ID fallback. `connected` reflects the native
connection callback, not a guarantee that SCTP is already open.
There are at most eight peer entries and 32 per-source entries.

Counters count decoded remote callbacks, not RTP packets or local sends.
`audio_samples` counts samples **per channel**; `audio_frames` counts decoded
audio blocks. Per-peer totals include all that peer's labels. Counters reset
on each newly committed epoch, never for a merely prepared/aborted graph.
Departing peers and their sources are removed from the snapshot.

Each validated start attempt increments `generation`, even if initialization
or connection subsequently fails. Invalid options and an already-active join
do not increment it. Every new start clears the previous call snapshot.
Stopping marks it inactive/not joined and retains final identity/counters
until the next start. No old call callbacks survive completion of `leave`.

### Bounds and ownership

Input lines are limited to 8192 bytes excluding LF. Oversized lines are
discarded through their newline and yield one error; following lines remain
usable. At most 512 input bytes are processed between main-loop ticks.
The event queue holds at most 128 entries, each at most 32768 serialized bytes.
Queue overflow emits an explicit error and shuts down instead of growing
without bound. Result lines are limited to 65536 serialized bytes. Stdout
backpressure may pause ticks; consumers must keep reading it.

The shared session owns RTC initialization, conference, signaling, and
synthetic generators. Its application thread drives lifecycle and `tick`;
callbacks update the mutex-protected snapshot and then forward to the UI
without holding that mutex. Shutdown quiesces conference/signaling callbacks
before releasing runtime resources or frontend callback state. Source
generation never uses a detached pacing thread.
