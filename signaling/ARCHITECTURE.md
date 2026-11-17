# Signaling Library (libmrtc_signaling) + Server

WebSocket signaling and SDP/ICE exchange. The reusable `mrtc_signaling` library
contains only message helpers and the WebSocket client. The unified server lives
in `app/server/`; its SFU media engine is the independent `mrtc_sfu` library in
`sfu/`, linked to routing, the RTC runtime, and shared `mrtc_sctp`, not the client
facade. See [PROTOCOL.md](PROTOCOL.md) for the versioned wire contract.

The server defaults to automatic per-room topology selection and loopback
binding. Start `build/app/server/mrtc_server --web-root web/dist` after building
frontend assets; libwebsockets HTTP mounts serve them without a Node runtime.
`build/signaling/signaling_server` is built from the same entrypoint for
compatibility. Both accept the existing positional port and `--mesh` alias.
Other options are `--port`, `--listen-ip` (`--bind`), `--announced-ip`,
`--media-port`, `--mode mesh|sfu|auto`, `--p2p-max` (default 2), and
`--downgrade-delay-ms` (default 15000).

## Architecture

```
┌─────────────┐     WebSocket      ┌──────────────────┐
│  Client A   │ ←───────────────→  │  Signaling       │
│  (lws +     │                    │  Server           │
│   cJSON)    │     JSON msgs      │  (lws single-    │
├─────────────┤ ←───────────────→  │   threaded)      │
│  Client B   │                    │                   │
└─────────────┘                    │  Meeting Manager  │
                                   │  (peer routing)   │
                                   └──────────────────┘
```

## Protocol (JSON over WebSocket)

The base fields below also describe legacy fixed-mode messages. Normal native
and web conference clients use version 2, adding epoch/session identity and
capabilities. See the [complete contract](PROTOCOL.md) rather than omitting
those fields when implementing an AUTO-capable client.

| Direction | Type | Fields |
|-----------|------|--------|
| C→S | `join` | meeting |
| C→S | `leave` | — |
| S→C | `joined` | peer_id, peers[], mode |
| S→C | `peer_joined` | peer_id |
| S→C | `peer_left` | peer_id |
| C→S | `offer` | to, sdp |
| C→S | `answer` | to, sdp |
| C→S | `candidate` | to, candidate |
| S→C | (relayed) | from (replaces to), sdp/candidate |
| S→C | `error` | message |
| S→C | `topology_prepare`, `topology_commit`, `topology_abort` | version, epoch, concrete mode, roster/reason |
| C→S | `topology_ready`, `topology_ack`, `topology_request` | version, epoch, concrete mode, reason where required |

Example: `{"type":"offer","to":"peer_abc123","sdp":"v=0\r\no=..."}`

In SFU mode, a publisher sends its offer to `"sfu"`. The server answers as
`"sfu"` and sends subscription offers identified by each remote publisher's
peer ID. Candidates and answers use those same session identifiers. An empty
candidate signals end-of-candidates; it is not parsed as an address.

## Usage Example

### Standalone mesh example (server started with `--mesh`)

```c
// 1. Define callbacks
void on_joined(const char *my_id, const char **peers, int n, void *user) {
    printf("I am %s, %d peers already here\n", my_id, n);
    for (int i = 0; i < n; i++)
        send_offer_to(peers[i]);  // create PC + offer for each
}

void on_offer(const char *from, const char *sdp, void *user) {
    // Received offer → create answer → send back
    signaling_client_t *sc = (signaling_client_t *)user;
    const char *answer_sdp = create_answer(from, sdp);
    signaling_send_answer(sc, from, answer_sdp);
}

void on_answer(const char *from, const char *sdp, void *user) {
    // Received answer → apply to peer connection
    set_remote_desc(from, sdp);
}

void on_peer_left(const char *peer_id, void *user) {
    destroy_peer_connection(peer_id);
}

// 2. Create and connect
signaling_config_t cfg = {
    .server_url  = "ws://localhost:9000",
    .meeting     = "room1",             // auto-joins on connect
    .on_joined       = on_joined,
    .on_peer_joined  = on_peer_joined,
    .on_peer_left    = on_peer_left,
    .on_offer        = on_offer,
    .on_answer       = on_answer,
    .on_candidate    = on_candidate,
    .on_error        = on_error,
    .user_data       = app,
};

signaling_client_t *sc = signaling_create(&cfg);
signaling_connect(sc);
// → background thread starts
// → WebSocket connects
// → auto-sends join("room1")
// → on_joined fires with assigned peer_id + existing peers

// 3. Exchange signaling messages (thread-safe)
signaling_send_offer(sc, "peer_B", offer_sdp);
signaling_send_answer(sc, "peer_A", answer_sdp);
signaling_send_candidate(sc, "peer_A", ice_candidate);

// 4. Leave and cleanup
signaling_leave(sc);
signaling_destroy(sc);  // blocks until thread exits
```

### With conference (typical usage)

```c
// Conference provides pre-wired callbacks:
signaling_config_t sig_cfg;
conference_get_signaling_callbacks(conf, &sig_cfg, "ws://server:9000", "room1");

signaling_client_t *sc = signaling_create(&sig_cfg);
conference_join(conf, sc);
signaling_connect(sc);
// → signaling events route directly into conference internals
// → conference handles offer/answer/peer lifecycle automatically
```

Bind conference signaling before connecting: a local WebSocket server can
return `joined` before `signaling_connect()` returns to the application.
The versioned `on_event` callback receives the whole parsed message, including
epoch/session identity, instead of legacy per-type dispatch. Its message is
borrowed only during the callback. With no `on_event`, legacy `on_mode` runs
before legacy `on_joined`. The conference helper installs its own v2 controller.

### Fixed SFU flow (`--mode sfu`)

1. Client joins and learns `mode="sfu"`.
2. Client creates one publishing connection to `"sfu"` for media and SCTP chat.
3. Server creates producers for the publisher's tracks.
4. Server sends subscription offers for remote publishers to other participants.
5. Consumers rewrite outbound stream identifiers and forward encrypted media
   through their own logical transports.
6. On leave/disconnect, subscriptions are removed before their producers;
   remaining participants receive `peer_left`.

The shared publisher listener and separate subscription listeners avoid
colliding DTLS associations when a native client shares its local UDP socket.
This intentionally uses more UDP ports than a single bundled connection per
participant, in exchange for simpler session ownership.
Overlapping publisher generations for the same participant also receive
separate listeners to prevent DTLS tuple aliasing.

### Automatic room control

The WebSocket/control thread owns room membership, concrete active/desired
topology, monotonic epoch assignment and prepare/commit acknowledgement state.
Rooms start in mesh and prepare SFU above the P2P threshold. Returning below the
threshold starts a hold-down before preparing mesh. A genuine P2P failure
request pins the room to SFU to avoid repeated failed downgrades.

The active graph continues during prepare. All admitted participants must
report ready before commit; all must acknowledge before old SFU sessions retire.
Preparation failure/timeout aborts only the pending epoch and blocks automatic
retry until membership changes or a first P2P failure requests fallback.
Joins/departures during prepare abort and rebuild with a fresh epoch and roster.
Joins during commit enter the already-selected current epoch; departures remove
their acknowledgement obligation. Missing commit acknowledgements close those
connections rather than rolling back peers that already retired old sessions.
Both phases have 10-second deadlines.

Auto rooms require version 2 and mesh/SFU/topology-switch capabilities. Legacy
version 1 clients are admitted only with explicitly forced mesh or SFU policy.
Fixed rooms reject mixed protocol versions explicitly rather than forwarding
versionless sessions into a version 2 graph.

### Legacy mesh message flow

```
Client A                Signaling Server              Client B
   │                         │                           │
   ├── join("room1") ───────→│                           │
   │                         │←── join("room1") ────────┤
   │                         │                           │
   │←── joined(id="A",      │                           │
   │     peers=["B"])        │                           │
   │                         │── peer_joined("A") ─────→│
   │                         │                           │
   ├── offer(to="B", sdp) ─→│── offer(from="A", sdp) ─→│
   │                         │                           │
   │←── answer(from="B",sdp)│←── answer(to="A", sdp) ──┤
   │                         │                           │
   │  ... media flows ...    │                           │
   │                         │                           │
   ├── leave() ─────────────→│── peer_left("A") ───────→│
```

## Components

### Message Format (`signaling_msg.h/c`)

JSON message building and parsing using cJSON.

**Build functions:** `sig_msg_build_join()`, `sig_msg_build_offer()`, `sig_msg_build_answer()`, `sig_msg_build_candidate()`, `sig_msg_build_leave()`

**Parse:** `sig_msg_parse()` → `sig_msg_t` with type, peer_id, from, sdp, candidate fields.

### Signaling Client (`signaling_client.h/c`)

WebSocket client that runs a libwebsockets event loop on a background thread.

**Lifecycle:**
1. `signaling_create(cfg)` — parse URL, init mutex
2. `signaling_connect()` — create lws context, start thread
3. On WebSocket connect → auto-send `join(meeting)`
4. Callbacks fire on lws thread
5. `signaling_leave()` → send leave message
6. `signaling_destroy()` → stop thread, free resources

`signaling_disconnect()` stops the service thread and drains callbacks before
conference peers are freed. `on_disconnected` lets the conference remove
participants even when the WebSocket closes without an explicit leave message.

**Thread safety:** Send functions (`signaling_send_offer`, etc.) are safe to call from any thread. Messages are queued via mutex and sent on the lws writable callback.

**Outgoing queue:** Circular buffer of 64 JSON strings. Messages are dequeued one at a time on `LWS_CALLBACK_CLIENT_WRITEABLE`.

**API:**

| Function | Thread-safe | Description |
|---|---|---|
| `signaling_create(cfg)` | — | Create client, parse URL |
| `signaling_connect()` | — | Start background thread, connect |
| `signaling_send_offer(to, sdp)` | Yes | Queue offer for sending |
| `signaling_send_answer(to, sdp)` | Yes | Queue answer for sending |
| `signaling_send_candidate(to, cand)` | Yes | Queue ICE candidate |
| `signaling_send_message(message)` | Yes | Queue a complete versioned session/topology message |
| `signaling_leave()` | Yes | Queue leave message |
| `signaling_disconnect()` | — | Stop service and drain callbacks |
| `signaling_destroy()` | — | Stop thread, cleanup |

Use the complete-message API for protocol v2. Legacy offer/answer/candidate
helpers cannot supply epoch/session ownership and are not AUTO-room helpers.

### Unified Server (`../app/server/`) and SFU (`../sfu/`)

The libwebsockets thread owns connection/meeting events. SFU transport and media
graph operations use copied ID/epoch commands marshalled through the existing
transport worker invocation queue. Worker events contain copied IDs and payloads;
a bounded control-event queue resolves them to current participants on the
WebSocket thread. No WebSocket peer pointers reach the media engine.

| File | Responsibility |
|------|---------------|
| `signaling_server.c` | lws event loop, WebSocket protocol handler |
| `meeting.c/h` | Meeting/peer management, message routing |
| `sfu/src/rtc_sfu.c` | Runtime transports, publishers/subscribers, SCTP and feedback |
| `sfu/include/rtc/rtc_sfu.h` | Copied commands/events and independent engine lifecycle |

**Connection lifecycle:**
```
LWS_CALLBACK_ESTABLISHED → create peer, assign 8-char random ID
LWS_CALLBACK_RECEIVE     → parse JSON, route message
LWS_CALLBACK_SERVER_WRITEABLE → send queued messages
LWS_CALLBACK_CLOSED      → remove peer, notify others in meeting
```

**Meeting manager:** `meeting_join()`, `meeting_peer_destroy()`, `meeting_route_message()`

**Peer ID:** Server assigns a random alphanumeric identifier whose capacity is
defined by `SIG_MAX_PEER_ID`. It identifies participants in signaling and in
conference messages and stays stable across topology changes. A later successful
join on a reused WebSocket receives a new call identity, preventing delayed
sessions from the previous room/call from matching a fresh epoch.

**Data channels:** SCTP/DCEP is standard and implemented in `mrtc_sctp` under
`rtc/`, shared by clients and the SFU. The conference's legacy `MRTC`
address/source envelope is application payload inside SCTP, not a replacement
for SCTP or DCEP. Version 2 uses `MRC2` envelopes with sender message IDs.
Envelope validation, destination routing and trusted sender replacement belong
to `app/server`, not the RTP engine; the sender message ID is preserved.

**Limits:** Eight peers per meeting, bounded outgoing queues, no authentication.
Default binding is `127.0.0.1`; remote-device testing requires explicit
`--listen-ip` and `--announced-ip`. The server is not a production WSS service.

## Dependencies

- **libwebsockets** — WebSocket protocol
- **cJSON** — JSON parsing
- **libmrtc_common** — types, logging, threading
- SFU server: **libmrtc_sfu**, **libmrtc_routing**, **libmrtc_sctp**, and **usrsctp**
- Client-disabled server: configure with `-DMRTC_ENABLE_CLIENT_API=OFF`;
  routing stays enabled for SFU/auto. Without routing, force mesh explicitly.

## Tests

| Test | Coverage |
|------|----------|
| `test_signaling_msg` | JSON message build/parse round-trip |
| `test_signaling` | Meeting creation, peer join/leave, message routing |
| `test_topology` | Independent rooms, prepare/ready/commit/ack, delayed downgrade, forced/legacy policy, failures and stale ownership |
| `test_sfu_session` | Actual audio/video, renegotiation, TWCC, SCTP, ICE restart, distinct overlapping epochs and retirement |
| `test_signaling_server_args` | Invalid, missing and conflicting CLI configuration |
| `test_server_runtime` | Real HTTP mounts/WebSockets: independent rooms, joins during both phases, downgrade, abort, stale ownership and same-ID mesh restart (optional Node 22+ test runner only) |
| Local browser harness | Actual media decoding, standard SCTP messages and publisher lifecycle through the SFU |
| Native churn harness | 120 real conference generations with media/data per generation |
