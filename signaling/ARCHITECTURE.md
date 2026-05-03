# Signaling Library (libmrtc_signaling) + Server

WebSocket-based signaling for meeting coordination, peer discovery, and SDP/ICE exchange.

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

| Direction | Type | Fields |
|-----------|------|--------|
| C→S | `join` | meeting |
| C→S | `leave` | — |
| S→C | `joined` | peer_id, peers[] |
| S→C | `peer_joined` | peer_id |
| S→C | `peer_left` | peer_id |
| C→S | `offer` | to, sdp |
| C→S | `answer` | to, sdp |
| C→S | `candidate` | to, candidate |
| S→C | (relayed) | from (replaces to), sdp/candidate |
| S→C | `error` | message |

Example: `{"type":"offer","to":"peer_abc123","sdp":"v=0\r\no=..."}`

## Usage Example

### Standalone (without conference)

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
signaling_connect(sc);
conference_join(conf, sc);
// → signaling events route directly into conference internals
// → conference handles offer/answer/peer lifecycle automatically
```

### Message flow

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
| `signaling_leave()` | Yes | Queue leave message |
| `signaling_destroy()` | — | Stop thread, cleanup |

### Signaling Server (`server/`)

Single-threaded server using libwebsockets event loop.

| File | Responsibility |
|------|---------------|
| `signaling_server.c` | lws event loop, WebSocket protocol handler |
| `meeting.c/h` | Meeting/peer management, message routing |

**Connection lifecycle:**
```
LWS_CALLBACK_ESTABLISHED → create peer, assign 8-char random ID
LWS_CALLBACK_RECEIVE     → parse JSON, route message
LWS_CALLBACK_SERVER_WRITEABLE → send queued messages
LWS_CALLBACK_CLOSED      → remove peer, notify others in meeting
```

**Meeting manager:** `meeting_join()`, `meeting_peer_destroy()`, `meeting_route_message()`

**Peer ID:** Server assigns a random 8-character alphanumeric string to each connecting client. This ID is used in all signaling messages to identify peers.

**Limits:** 8 peers per meeting (mesh topology limit), no authentication.

## Dependencies

- **libwebsockets** — WebSocket protocol
- **cJSON** — JSON parsing
- **libmrtc_common** — types, logging, threading

## Tests

| Test | Coverage |
|------|----------|
| `test_signaling_msg` | JSON message build/parse round-trip |
| `test_signaling` | Meeting creation, peer join/leave, message routing |
