# Unified Server — mrtc_server

One executable starts the HTTP/WebSocket control loop and the independent SFU
media worker. This is the threaded deployment; there is no child-process
supervisor or internal network RPC service.

## Responsibilities

- `signaling_server.c`: launcher/options, libwebsockets HTTP mounts and
  WebSocket transport, copied SFU-event queue, application chat routing.
- `meeting.c/h`: admitted participants, room policy, epochs, transitions,
  session ownership and roster notifications.
- `mrtc_sfu`: media sessions through a stable-ID command/event interface.

The compatibility executable is built from the same entrypoint at
`build/signaling/signaling_server`; the canonical path is
`build/app/server/mrtc_server`.

## Start

From the repository root, after building C targets and `app/web/dist`:

```sh
./build/app/server/mrtc_server 8080 --web-root app/web/dist
```

Open `http://localhost:8080`. HTTP assets and `/ws` share the control endpoint;
native root-path WebSocket clients remain supported. Node is not a server
runtime dependency. `--web-root` uses libwebsockets file mounts rather than a
custom filesystem handler.

Mode options:

```sh
./build/app/server/mrtc_server 8080 --mode mesh
./build/app/server/mrtc_server 8080 --mode sfu
./build/app/server/mrtc_server 8080 --mode auto --p2p-max 4
```

AUTO is default; the default maximum P2P membership is two (range 2..8).
`--mesh` aliases forced mesh. Downgrade hold-down defaults to 15000 ms and is
configurable with `--downgrade-delay-ms`. Transition preparation/acknowledgement
deadlines are 10 seconds. Invalid ports, policy values and web roots fail
explicitly.

Default binding is loopback. `--listen-ip`/`--bind`, `--announced-ip`, and
`--media-port` retain their existing purposes. Remote browser capture needs
HTTPS/WSS; certificate management and authentication are not implemented here.

## Per-Room State

Each room owns membership, active/desired/previous mode, active/pending/previous
epoch, transaction phase and deadlines. AUTO is a policy; clients are told
the concrete active mesh/SFU topology.

```text
STABLE -> PREPARING -> COMMITTING -> STABLE
                  \-> abort -> STABLE on old graph
```

On count above the threshold, prepare SFU. On count below it, wait the hold-down
before preparing mesh. Muting does not remove a participant. A genuine P2P
failure can request SFU fallback and latch that preference for the room to
avoid oscillation. Repeated failed preparations are not retried in a tight
loop.

The coordinator validates epoch/session ownership, gathers READY/ACK from
remaining participants, reconciles membership changes and retires old
generations. Existing media continues during preparation. It never lets each
client independently choose a topology.

Version 2 joins advertise capabilities. Legacy version 1 is fixed-mode only;
incompatible joins fail visibly instead of putting half a room on another
topology. Rejoining on a reused WebSocket gets a fresh call identity so delayed
messages cannot match another room's epoch-one sessions.

## Thread Boundary

The control loop owns WebSocket and meeting objects. The media worker receives
stable IDs/SDP/candidate commands and emits events. Its callback copies events
into a bounded control queue; writable WebSocket callbacks remain on the
control thread. A queued event with a departed owner cannot dereference a freed
WebSocket peer.

The SFU does not parse the conference's `MRC2` destination envelope.
The coordinator validates/replaces sender identity and forwards application
messages while preserving message IDs for client deduplication.

## Tests

- `test_topology`: independent rooms, configurable threshold, hold-down,
  capabilities, timeouts, membership races and stale addressing.
- `test_server_runtime`: real HTTP mounts and WebSocket room behavior.
- `test_signaling_server_args` / `test_server_compat_args`: checked options on
  both canonical and compatibility executables.
- Real UI/native interoperability: decoded media/data, both migration
  directions, abort continuity and churn through the actual launcher.
