# mrtc — Protocol Implementation Status

This is an implementation inventory, not an RFC certification or a security
audit. Coverage percentages are intentionally omitted: implementing one packet
format does not establish complete interoperability with a specification.

The active network path is the worker/listener/logical-transport runtime.
The older standalone ICE agent remains a protocol component and test fixture;
changes there alone do not change client or SFU connectivity.

## Summary

| Standard | Implemented behavior | Remaining boundary |
|---|---|---|
| RFC 3550 — RTP/RTCP | Compound reports/feedback + per-publisher SDES/CNAME, interval loss/jitter, live bounded membership/sender expiry, timer/reverse reconsideration, publisher RTP/NTP mapping across SFU legs, monotonic SR-history RTT, incoming/outgoing BYE and inactivity expiry | Supported unicast profile; SSRC collision recovery and large-group BYE backoff remain incomplete; impaired-network synchronized playout needs qualification |
| RFC 3711 — SRTP/SRTCP | AES-128-CM/HMAC-SHA1-80, per-SSRC rollover/replay state, authenticated replay rejection, capacity checks | No MKI/rekeying API or independent hardening audit |
| RFC 7714 — SRTP AEAD | AES-128-GCM and AES-256-GCM; RTP/SRTCP protection and DTLS profile negotiation | Not every optional cipher/profile is offered |
| RFC 8489 / RFC 5389 — STUN | UDP messages, integrity/fingerprint, address validation, ICE checks and credential-based TURN authentication | Authentication v2, alternate servers and TCP/TLS STUN are not implemented |
| RFC 8445 — ICE | Full/lite operation, controlling/controlled roles, conflicts, nomination, consent and active restart on the runtime | Not a complete implementation of every checklist/dual-stack extension |
| RFC 7675 — consent | Authenticated consent requests and expiration of an unresponsive selected path | Application reconnect policy remains application-owned |
| RFC 8838 / RFC 8839 — trickle ICE and SDP | Asynchronous candidates, remote ingestion, end-of-candidates, ICE-lite signaling and retained browser candidate metadata | mDNS is deferred to authenticated peer-reflexive checks, not resolved by a multicast-DNS service |
| RFC 8656 / RFC 5766 — TURN | Worker-owned UDP relay allocation over UDP/TCP/verified TLS, bounded pre-allocation address failover, permissions/channels, refresh and relay-only connectivity | One server identity/peer per logical transport; no multiple-server failover, live allocation migration or authentication v2; IPv6 relay qualification remains open |
| RFC 6347 / RFC 9147 — DTLS | Minimum DTLS 1.2; newer versions negotiated when supported by OpenSSL; datagram-preserving BIOs on OpenSSL 3.2+ | Older OpenSSL uses the existing memory-BIO fallback |
| RFC 5764 — DTLS-SRTP | Profile negotiation, key export and remote SHA-256 fingerprint pinning | Signaling authenticity is not provided by fingerprint pinning alone |
| RFC 7983 — packet demultiplexing | Shared UDP classification and logical-transport routing | Distinct DTLS associations still require distinct network tuples |
| RFC 8866 / RFC 3264 / RFC 8829 — SDP/JSEP | Multi-media SDP, initial dynamic PT mapping, RTP rejection/rtcp-mux validation, feedback/extension intersection, repeated offer/answer, glare rejection/rollback and ICE restart | Established codec/payload-type mutation and full Unified Plan are not supported |
| RFC 8285 — RTP extensions | One-byte extension form, MID and transport-cc negotiation | Two-byte form and encrypted extensions are not implemented |
| RFC 4585 / RFC 5104 — feedback | NACK, PLI and FIR build/parse/routing; receiver loss feedback and bounded retransmission history | No RTX/FEC implementation |
| Transport-wide CC draft / GCC | Transport-wide sequence numbers, arrival feedback and delay/loss estimation | This is not a claim to implement the different RFC 8888 wire format |
| REMB draft — `goog-remb` | Negotiated single-source bitrate feedback; conservative minimum fresh-consumer video bound mapped to the original source; native encoder target constraints | Not a subscriber-wide allocator, pacer or simulcast selector; not an RFC 8888 implementation |
| RFC 7741 — VP8 payload | Fragmentation, bounded sequence-aware assembly, PictureID lengths and keyframe detection | No temporal-layer scheduling or arbitrary cross-frame reorder buffer |
| RFC 7587 — Opus payload | Opus at 48 kHz, one encoded audio frame per packet | Codec format-parameter coverage and in-band FEC negotiation remain limited |
| RFC 8831 / RFC 8832 / RFC 8261 — data channels | usrsctp SCTP-over-DTLS, DCEP, stream resets, reliable/partial-reliable delivery and backpressure | Application message schemas remain application-specific |
| RFC 6455 — WebSocket | Signaling framing/transport through libwebsockets | No application authentication or production signaling deployment is supplied |

## RTP, Media Integrity and Feedback

All fragments of a video frame use one RTP timestamp. The sender advances its
timestamp after the marker packet, using the configured frame rate. Each
registered peer has its own RTP/SRTP sequence space, while a shared encoder
avoids encoding the same source independently for every peer.

VP8 frame starts require S=1 and partition ID 0. The bitstream P bit identifies
keyframes; the descriptor N bit identifies a non-reference frame and must not
be treated as an interframe/keyframe flag.

Receive processing is bounded to one in-flight frame and a finite reordering
window. Missing fragments do not become complete decoder input. Loss feedback
uses bounded NACK state and rate-limited keyframe requests; a new peer requests
a fresh keyframe instead of starting from an arbitrary reference-dependent frame.

The SFU rewrites consumer SSRC/sequence spaces and maintains bounded
retransmission state. Upstream and downstream feedback must be interpreted in
their respective sequence spaces, not blindly relayed.

Standalone NACK/PLI/FIR/TWCC is prefixed with RR + SDES before SRTCP protection.
Reduced-size RTCP is not advertised, so its optional negotiation is unnecessary
for this profile. Callers reserve `RTC_TRANSPORT_RTCP_PROTECT_OVERHEAD` for the
compound prefix and crypto trailer. SDP feedback is payload-scoped (including
wildcards), answers intersect capabilities and RTP extension IDs with the
offer, and native recovery respects negotiated NACK/PLI/FIR support.

**Validation:** [VP8 tests](media/tests/test_vp8.c),
[media pipeline tests](media/tests/test_media_pipeline.c),
[real encrypted video/loss loopback](media/tests/test_video_loopback.c), and
[media-session feedback tests](rtc/tests/test_media_session.c).

## Encryption and Identity

SRTP/SRTCP contexts serialize cryptographic counters and maintain independent
per-SSRC receive replay windows. Replay state advances only after packet
authentication. Protection APIs receive writable buffer capacity explicitly.

Runtime callers supply the remote SDP fingerprint with
`rtc_transport_set_remote_dtls_parameters()` before the handshake. The client
facade and SFU do this when applying remote descriptions. A fingerprint mismatch
is a connection failure, not a warning followed by accepted media.

DTLS requires version 1.2 or newer. No maximum is pinned: OpenSSL 4 can
negotiate DTLS 1.3, while the tests also force a real DTLS 1.2 handshake.
OpenSSL 3.2+ datagram-memory BIOs preserve UDP boundaries and use an explicit
MTU; stream-style reads must not split a handshake flight into invalid packets.

**Validation:** [SRTP tests](rtc/tests/test_srtp.c),
[DTLS tests](rtc/tests/test_dtls.c), and
[runtime identity/connectivity tests](rtc/tests/test_network_runtime.c).

## ICE, TURN and Restart

The common parser retains valid numeric, mDNS host and TCP candidate metadata
both in trickle messages and embedded SDP. Malformed candidates fail explicitly
instead of being silently dropped. UDP runtime transports classify TCP
alternatives as ineligible and defer `.local` names without leaking them to
unicast DNS or blocking the worker. Acceptance/defer counters do not mark a
transport connected; an authenticated incoming check must establish the
peer-reflexive tuple. Tests verify this with a controlling endpoint that has no
numeric remote candidate and subsequently carries real encrypted RTP.

ICE control operations execute on the owning worker. Full ICE handles
controlling/controlled roles and role conflicts; ICE-lite answers authenticated
checks. Consent expiration invalidates an unresponsive path. Active restart
refreshes ICE credentials and checks without inherently replacing the DTLS
transport, RTP streams or SCTP association.

TURN allocations use separate worker-owned UDP sockets. This is necessary
because a TURN allocation is identified by its network tuple, not by a logical
transport identifier behind a shared listener. Permission, channel and
allocation refresh and cleanup are timer-driven.

The client supports one authenticated `turn:` UDP/TCP or `turns:` TLS URL
and a relay-only policy. TLS verifies the configured server name/IP and trust
chain, using OpenSSL's default trust store or an explicit CA file. TCP/TLS is
the client-to-server leg; relay candidates remain UDP. Stream I/O is nonblocking,
framed and bounded (64 KiB send queue, 500 ms queued-write deadline after
connection, five-second connect/TLS deadline). TCP_NODELAY prevents small
control messages waiting behind Nagle/delayed acknowledgements. Multiple-server
configurations fail explicitly. One identity may try up to four resolved
addresses before allocation, with a five-second budget per attempt when
alternatives exist. Address-family alternatives are retained, and callers may
provide a copied numeric address list. TLS always verifies the original host.
Successful allocations are pinned; exhausting addresses never enables a direct
path. Retry timers and failed sockets are worker-owned and cancelled on close.
Candidates and end-of-candidates can arrive after the initial SDP.

**Validation:** [network runtime tests](rtc/tests/test_network_runtime.c),
[local coturn fixture](rtc/tests/run_turn_runtime.sh), and
[client negotiation/restart/relay integration](client/tests/test_peer_integration.c).
Coturn-dependent CTests are `test_network_turn` and `test_peer_turn`.
Their `_tcp` / `_tls` variants cover encrypted relay delivery, refresh and
native SCTP/restart continuity. TLS negative tests reject an untrusted
certificate and a trusted certificate with the wrong identity. Unit tests cover
partial framing and ChannelData padding.

## Live Negotiation and Data Channels

The client facade serializes changes to live transceivers and descriptions on
the worker. It supports add/remove/direction changes and explicit rollback.
Glare is rejected so applications can choose a deterministic rollback policy.
Unsupported established codec/payload-type changes leave current descriptions
unchanged.

Data channels use maintained usrsctp rather than custom DTLS message framing.
Tests cover stream-ID parity, negotiated IDs, empty-message PPIDs, a fragmented
48 KB message, real loss/retransmission, partial reliability, backpressure and
threshold callbacks, stream reset, and close callbacks.

**Validation:** [data-channel tests](client/tests/test_data_channel.c) and
[peer integration tests](client/tests/test_peer_integration.c).

## Room Topology and Browser Validation

The unified `mrtc_server` defaults to per-room AUTO policy: up to two admitted
participants use P2P, then the coordinator prepares SFU. Forced mesh/SFU and
configurable thresholds/hold-down remain available. This is an application
protocol, not an additional WebRTC RFC.

[Protocol v2](signaling/PROTOCOL.md) separates participant identity from
epoch/session identity and defines prepare/ready/commit/ack/abort. Capture,
gallery identity and selected media survive handoff; old/pending descriptions
cannot mutate another generation. A different remote endpoint requires new
DTLS/SCTP sessions even though an ICE restart of one endpoint can preserve them.
Application message IDs have bounded duplicate suppression, not an exactly-once
delivery guarantee across arbitrary failures.

The independent SFU engine uses the existing runtime/routing/SCTP libraries,
not the native client or codecs. The control loop owns room policy and
WebSocket/HTTP state; the media worker receives stable-ID commands and emits
copied events. These are threads in one process, not isolated worker processes.

The [actual browser client](app/web/README.md) uses built-in WebRTC. The
[interop harness](tests/interop/README.md) drives that UI through the C server's
HTTP mount. Real Join clicks, default mDNS privacy and autoplay policies are
preserved. Tests require per-source decoded video/audio in both directions,
standard SCTP messages, fixed modes, live 2→3→2 migration, selected audio,
capture/tile lifetime, restart, timeout/abort continuity and stale messages.
Packet receipt or a mode label alone is not treated as successful media.

The separate churn harness checks 120 native conference generations. Native
and UI configuration regressions remain separate coverage; browser/probe
success does not certify every SDL device/launch configuration.

## Deferred Work and Deployment Limits

- SSRC collision recovery and large-group BYE backoff; impaired-network qualification of capture-clock synchronization, membership reconsideration and BYE/source expiry.
- FEC/RTX, simulcast/SVC and additional codecs.
- Multiple TURN servers, post-allocation recovery, dual-stack completeness and STUN authentication v2; hostile-network qualification of pre-allocation address failover.
- Full SDP/Unified Plan coverage and established codec changes.
- Production authentication, authorization, HTTPS/WSS deployment, quotas,
  sophisticated per-subscriber bitrate allocation and independent security review.

See [ROADMAP.md](ROADMAP.md) for priorities and
[README.md](README.md) for dependencies and validation commands. Passing these
tests does not make the educational stack production-ready.
