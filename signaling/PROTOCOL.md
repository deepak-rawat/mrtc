# Room signaling protocol

The room coordinator owns membership and topology decisions. Clients must not
switch topology independently based on their local participant count.

## Versions and identity

Version 2 messages have `"version":2`. Missing version means legacy version 1;
legacy clients are fixed-mode only and must not silently enter an automatic
room requiring live migration.

- `peer_id` is the stable participant identity for a joined call.
- `display_name` is optional presentation text, not an identifier.
- `epoch` is a positive uint32 topology generation, assigned by the coordinator.
- `session_id` identifies one PeerConnection and its SDP/candidate exchange.
- `to`/`from` retain participant addressing; `"sfu"` addresses publishing.
- `peers` contains other participant IDs, excluding the recipient.
- Optional `peer_names` maps the IDs in `peers` to display names.

Participant IDs contain ASCII letters, digits, `_` or `-`, and occupy fewer
than 16 bytes. Session IDs occupy fewer than 64 bytes.

Session IDs are deterministic:

| Role | Format |
|---|---|
| Mesh pair | `m:<epoch>:<lexically-smaller-id>:<larger-id>` |
| SFU publishing | `p:<epoch>:<publisher-id>` |
| SFU subscription | `s:<epoch>:<subscriber-id>:<publisher-id>` |

For version 2 mesh pairs, the smaller participant in ASCII byte order makes the
initial offer (`strcmp` in C, `<` for these ASCII IDs in JavaScript, not a
locale-sensitive comparison). Shared C helpers and protocol tests define the same session naming used
by the web client. SDP/candidate messages carry both `epoch` and `session_id`;
stale generations are discarded explicitly without affecting active sessions.
Ordinary ICE restart keeps the session ID; candidate username fragments still
identify their ICE generation.

## Join and fixed operation

```json
{"type":"join","version":2,"meeting":"demo","display_name":"Alice","capabilities":{"mesh":true,"sfu":true,"topology_switch":true}}
```

The coordinator responds with `joined`, including the selected concrete
`mode` (`mesh` or `sfu`), `epoch`, `peer_id`, `peers` and optional `peer_names`.
`auto` is a server policy, not a media topology announced as active.

`peer_joined`/`peer_left` report membership. `offer`, `answer` and `candidate`
carry session addressing. An empty candidate is end-of-candidates, not an
address. Established codec changes remain subject to the RTC implementation's
constraints.

Forced mesh and SFU modes do not initiate topology changes. Automatic rooms
start in mesh, use the configured maximum P2P participant count, and migrate to
SFU when the admitted membership exceeds it. Muted participants still count.
Downgrades wait for a configurable hold-down; failed P2P connectivity can
request SFU fallback without creating a repeated downgrade/failure loop.

## Make-before-break transitions

Topology messages carry `version`, the target `epoch`, and `mode`:

- `topology_prepare`: build a pending graph while the active graph continues.
  Includes the current `peers` roster.
- `topology_ready`: client has usable required pending media/data sessions.
- `topology_commit`: select the pending graph for rendering and sending.
- `topology_ack`: client has selected the committed graph; old sessions can
  drain and be retired.
- `topology_abort`: discard only the pending graph; `reason` explains failure.
- `topology_request`: client requests SFU fallback after an actual P2P failure;
  the coordinator remains authoritative.

At most active and pending/retiring generations coexist. Membership changes,
timeouts and participant departures must reconcile or cancel a transition;
they must not free state still referenced by callbacks.

Transport readiness alone is not proof of decoded media. Tests check actual
per-source media after handoff. New audio is not played twice during prepare.
Intentionally muted, inactive and absent tracks must not cause an impossible
readiness wait. Flowing tracks require usable pending media before handoff.
Participant/gallery identities and capture sources survive a topology change.
Closing an old PeerConnection must not stop a capture track reused by another.

New SCTP associations are required when the remote endpoint changes. Each
application sends on one selected graph; draining sessions may still receive.

## Conference data-channel envelope

The SCTP/DCEP transport remains standard WebRTC. Version 2 room messages use
this application payload, with integers in network byte order:

```text
4 bytes   "MRC2"
16 bytes  NUL-terminated participant/destination ID, zero-padded
8 bytes   nonzero uint64 sender message ID
remaining application payload
```

Mesh senders identify themselves in the ID field. SFU publishers put the
destination ID or `*` there; the trusted room coordinator replaces it with the
sender ID before forwarding. The sender's message ID is preserved. Receivers
deduplicate a bounded recent-message history by sender and message ID while
graphs overlap. No exactly-once delivery guarantee across arbitrary failures
is implied.

Legacy `"MRTC"` envelopes have the same ID field but no message ID. They remain
readable for fixed-mode compatibility, not a substitute for version negotiation.

## Bounds and failures

Malformed fields, unsupported versions, oversized strings and invalid session
ownership fail explicitly. A syntactically valid but stale generation is not
applied to the current session. A failed transition preserves the old graph and
surfaces a reason to the UI; it must not report a successful migration.
