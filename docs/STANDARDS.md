# Standards support

Code/test inventory, **not RFC certification or an interoperability pass**.
Implemented means the listed behavior exists; Partial names known subset gaps;
Unsupported means no shipped implementation. Evidence links identify coverage,
not results from a new test run. External platform/network qualification remains
on the [roadmap](ROADMAP.md).

The active path is worker/listener/logical transport. Transports share the
worker-owned [ICE implementation](../rtc/src/rtc_ice.c); listeners gather local
candidates and transports own TURN allocations and encrypted media. Component
tests exercise the same ICE implementation as client/SFU runtime integration.

## Transport and negotiation

| Standard | Status | Current support and boundary | Evidence |
|---|---|---|---|
| STUN: RFC 5389/8489 | Partial | Binding, HMAC-SHA1 integrity, fingerprint attributes and ICE/TURN messages; no authentication-v2/SHA-256 suite or alternate-server support | [STUN](../rtc/tests/test_stun.c), [implementation](../rtc/src/rtc_stun.c) |
| ICE: RFC 8445 | Partial | Full/lite roles, conflicts, nomination and active restart; not every checklist/dual-stack extension | [Runtime](../rtc/tests/test_network_runtime.c) |
| Consent: RFC 7675 | Implemented | Authenticated checks and selected-path expiry; application reconnect policy is separate | [Runtime](../rtc/tests/test_network_runtime.c) |
| Trickle/ICE SDP: RFC 8838/8839 | Partial | Candidates/end-of-candidates and ICE-lite; retain mDNS/TCP metadata, defer `.local` to authenticated peer-reflexive paths, no multicast resolver/TCP ICE | [SDP](../rtc/tests/test_sdp.c), [runtime](../rtc/tests/test_network_runtime.c) |
| TURN: RFC 5766/8656/6156 | Partial | Authenticated UDP/TCP/TLS server legs, UDP relays, IPv4/IPv6, refresh and failed-allocation restart; bounded pool limits below | [Coturn fixture](../rtc/tests/run_turn_runtime.sh), [peer relay](../client/tests/test_peer_integration.c) |
| DTLS: RFC 6347 | Implemented | Require DTLS >=1.2; OpenSSL controls available newer versions. Datagram-memory BIO on OpenSSL >=3.2, memory-BIO fallback otherwise | [Implementation](../rtc/src/rtc_dtls.c), [tests](../rtc/tests/test_dtls.c) |
| DTLS-SRTP: RFC 5764 | Implemented | Negotiate profiles, export keys, pin remote SHA-256 fingerprint on runtime; signaling authenticity remains external | [Runtime negative tests](../rtc/tests/test_network_runtime.c) |
| SRTP/SRTCP: RFC 3711 | Partial | AES-128-CM/HMAC-SHA1-80, authenticated per-SSRC replay/rollover state; no MKI/rekeying API | [Crypto tests](../rtc/tests/test_srtp.c) |
| AEAD SRTP: RFC 7714 | Implemented | AES-128-GCM and AES-256-GCM for RTP/SRTCP, including DTLS profile selection | [Crypto](../rtc/tests/test_srtp.c), [DTLS](../rtc/src/rtc_dtls.c) |
| UDP demux: RFC 7983 | Implemented | Shared UDP classification and logical endpoint routing; distinct DTLS associations need distinct tuples | [Packet I/O](../rtc/tests/test_packet_io.c), [listener](../rtc/tests/test_listener.c) |
| SDP/JSEP: RFC 8866/3264/8829 | Partial | BUNDLE/rtcp-mux, bounded fields, initial PT mapping, directions, feedback/extensions, rejection, renegotiation and local-offer rollback; no full Unified Plan/established codec mutation | [Peer integration](../client/tests/test_peer_integration.c), [SDP](../rtc/tests/test_sdp.c) |
| WebSocket: RFC 6455 | Implemented | libwebsockets signaling transport; no supplied application authentication or production TLS deployment | [Server runtime](../app/server/test_server_runtime.mjs) |

### TURN and identity limits

[The public transport API](../rtc/include/rtc/rtc_transport.h) supports up to four
identities sharing four total endpoint attempts; alternatives have a five-second
allocation budget. TLS checks the configured hostname/IP and trust chain on
every attempt; TCP/TLS is only the client-to-server leg, not a TCP media relay.
Exhaustion never falls back to a direct path. Healthy allocations stay pinned;
failed allocations can recover on explicit local/remote ICE restart while
retaining DTLS/SRTP/SCTP state. One relay family is selected; no NAT64,
simultaneous dual-family allocation or transparent healthy-allocation migration.

DTLS configuration does not constitute a DTLS 1.3/RFC 9147 qualification claim.
Transport fingerprint pinning also does not authenticate the room's signaling
service. These are separate deployment/qualification requirements.

## Media and data

| Standard | Status | Current support and boundary | Evidence |
|---|---|---|---|
| RTP/RTCP: RFC 3550 | Partial | Reports + SDES/CNAME, membership/expiry/reconsideration, publisher RTP/NTP clocks, monotonic RTT, BYE and collision recovery; no large-group BYE backoff; impaired synchronized playout unqualified | [RTCP](../rtc/tests/test_rtcp.c), [collision/retirement](../rtc/tests/test_loopback.c) |
| Feedback: RFC 4585/5104 | Implemented | NACK/PLI/FIR build/parse and negotiated routing with bounded recovery; not RTX/FEC | [Feedback](../rtc/tests/test_rtcp_feedback.c), [routed loss](../routing/tests/test_routing_media.c) |
| RTP extensions: RFC 8285 | Partial | One-byte form, directional MID/transport-cc negotiation; no two-byte/encrypted extensions | [Extensions](../rtc/tests/test_rtp_ext.c), [peer wire tests](../client/tests/test_peer.c) |
| Transport-wide CC / GCC drafts | Implemented | Sequence allocation, arrival feedback and delay/loss estimator; optional build feature, not RFC 8888 | [TWCC](../rtc/tests/test_twcc.c), [BWE](../rtc/tests/test_bwe.c) |
| REMB draft | Implemented | Negotiated single-source bound, consumer aggregation and native encoder limits; shared encoding, no independent subscriber layers | [Media routing](../routing/tests/test_routing_media.c), [media session](../rtc/tests/test_media_session.c) |
| VP8 payload: RFC 7741 | Partial | Fragmentation, bounded sequence-aware assembly, PictureID/keyframe handling; no temporal-layer scheduling | [VP8](../media/tests/test_vp8.c), [encrypted video loopback](../media/tests/test_video_loopback.c) |
| Opus payload: RFC 7587 | Partial | 48 kHz, codec-duration clocked playout and bounded PLC with DTX gaps; fmtp/FEC negotiation remains limited | [Backend](../media/src/codec_opus.c), [playout](../media/tests/test_jitter_buffer.c), [pipeline](../media/tests/test_media_pipeline.c) |
| Data channels: RFC 8831/8832/8261 | Implemented | usrsctp SCTP-over-DTLS/DCEP, stream parity/reset, fragmentation, partial reliability and backpressure; room envelopes are application-defined | [Data channels](../client/tests/test_data_channel.c), [peer integration](../client/tests/test_peer_integration.c) |
| Congestion feedback: RFC 8888 | Unsupported | TWCC/REMB drafts above do not implement this wire format | [Feedback formats](../rtc/include/rtc/rtc_rtcp.h) |
| RTX/FEC, simulcast/SVC | Unsupported | No shipped repair-payload/layer-selection pipeline; NACK retransmission is not RTX | [Media pipeline](../media/src/media_pipeline.c) |

Regular feedback is compound RR/SR + SDES before SRTCP; reduced-size RTCP is not
advertised. Video fragments share a frame timestamp; incomplete frames do not
become decoder input. The SFU rewrites downstream SSRC/sequence spaces and
maintains leg-specific repair history; it does not blindly relay rewritten NACKs.

Crypto source tables preserve replay/nonce state across source churn; capacity
exhaustion requires a newly keyed transport, not resetting counters or ICE alone.
Frame-aware pacing/subscriber allocation are implementation controls, not
independent per-subscriber encodings or a WAN quality guarantee.
Native codecs shipped are VP8/Opus; parsing H.264 SDP does not provide an H.264 codec.

## What remains unqualified

[E2E tests](TESTING.md#e2e-setup-and-failures) drive the real UI/native clients,
check fresh decoded media/SCTP, restart and mesh/SFU migration, and preserve
browser privacy/autoplay. They are not blanket browser/OS certification.
Actual Safari, broader external Firefox/native-OS runs, hostile networks,
long soaks, capacity, lip-sync/media latency and independent protocol/security
review remain gates. The SFU terminates per-hop crypto, not participant E2EE.
Local browser automation uses the [pinned Chromium baseline](TESTING.md#e2e-setup-and-failures);
newer Chromium audio-playout failures remain an external qualification boundary.
