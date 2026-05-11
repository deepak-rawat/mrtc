# mrtc — RFC Compliance Report

Status of each IETF RFC relevant to the WebRTC protocol stack as implemented in mrtc.

**Legend:**  
✅ Implemented · ⚠️ Partial · ❌ Not Implemented · 📋 Planned

---

## Summary

| RFC | Title | Status | Coverage |
|-----|-------|--------|----------|
| [RFC 3550](#rfc-3550--rtprtcp) | RTP / RTCP | ⚠️ Partial | ~50% |
| [RFC 3711](#rfc-3711--srtp) | SRTP | ⚠️ Partial | ~75% |
| [RFC 4566](#rfc-4566--sdp) | SDP | ⚠️ Partial | ~70% |
| [RFC 4585](#rfc-4585--rtcp-feedback) | RTCP Feedback (NACK, PLI, FIR) | ❌ Not Impl | 0% |
| [RFC 5109](#rfc-5109--ulpfec) | UlpFEC | ❌ Not Impl | 0% |
| [RFC 5389](#rfc-5389--stun) | STUN | ✅ Implemented | ~95% |
| [RFC 5764](#rfc-5764--dtls-srtp) | DTLS-SRTP | ✅ Implemented | ~95% |
| [RFC 5766](#rfc-5766--turn) | TURN | ⚠️ Partial | ~50% |
| [RFC 6347](#rfc-6347--dtls-12) | DTLS 1.2 | ✅ Implemented | ~95% |
| [RFC 6455](#rfc-6455--websocket) | WebSocket (signaling) | ✅ Implemented | via libwebsockets |
| [RFC 7587](#rfc-7587--opus-rtp) | Opus RTP Payload | ✅ Implemented | ~90% |
| [RFC 7741](#rfc-7741--vp8-rtp) | VP8 RTP Payload | ⚠️ Partial | ~85% |
| [RFC 7983](#rfc-7983--packet-demux) | Multiplexing STUN/DTLS/RTP | ✅ Implemented | 100% |
| [RFC 8445](#rfc-8445--ice) | ICE | ⚠️ Partial | ~45% |
| [RFC 8829](#rfc-8829--jsep) | JSEP (offer/answer model) | ⚠️ Partial | ~60% |
| [RFC 8834](#rfc-8834--media-transport) | Media Transport over RTP | ⚠️ Partial | ~50% |
| [RFC 8839](#rfc-8839--ice-sdp) | ICE SDP Attributes | ⚠️ Partial | ~60% |
| [RFC 8888](#rfc-8888--transport-cc) | Transport-CC | ❌ Not Impl | 0% |

---

## RFC 3550 — RTP/RTCP

**"RTP: A Transport Protocol for Real-Time Applications"**

Files: `rtc/src/rtc_rtp.c`, `rtc/src/rtc_rtcp.c`, `rtc/include/rtc/rtc_rtp.h`, `rtc/src/rtc_rtcp.h`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 5.1 | RTP fixed header (V, P, X, CC, M, PT, seq, ts, SSRC) | Full 12-byte header serialization and parsing |
| 5.1 | Version field = 2 | Enforced on build and validated on parse |
| 5.1 | Marker bit | Set/get supported, used for frame boundaries |
| 5.1 | Payload type | Dynamic PT via `rtc_codec_t` |
| 5.1 | Sequence number auto-increment | Per-session tracking in `rtc_rtp_session_t` |
| 5.1 | Timestamp auto-increment | Per-codec clock rate (e.g. 48000 for Opus, 90000 for VP8) |
| 5.1 | SSRC random generation | Per-session unique SSRC |
| 6.4.1 | Sender Report (SR, PT=200) | Build and parse with NTP timestamp, RTP timestamp, packet/octet counts |
| 6.4.2 | Receiver Report (RR, PT=201) | Build and parse with fraction lost, cumulative lost, highest seq, jitter, LSR, DLSR |
| A.8 | Interarrival jitter calculation | Per-packet jitter estimate per RFC formula |
| — | Periodic RTCP transmission | Every 5 seconds from peer connection |
| — | RTCP multiplexed with RTP | Single UDP socket via `a=rtcp-mux` |

### Partially Implemented

| Section | Feature | Status |
|---------|---------|--------|
| 5.1 | CSRC list (Contributing Sources) | CC field parsed but CSRC entries ignored |
| 5.3 | RTP header extensions (X bit) | Extension bit detected but extension data not parsed |
| 5.1 | Padding (P bit) | P bit handled in header but padding bytes not stripped |
| 6.4.1 | SR NTP timestamp accuracy | Approximated from monotonic clock, not wall-clock |
| 6.4.2 | RR loss calculation | Assumes seq starts at 0; `packets_expected = highest_seq + 1` is wrong if initial seq ≠ 0 |
| 6.1 | Compound RTCP packets | Only single SR or RR sent per interval, not compound packets |

### Not Implemented

| Section | Feature | Status |
|---------|---------|--------|
| 6.1 | Compound RTCP requirement | Spec requires SR/RR + SDES in every compound packet |
| 6.5 | SDES (Source Description, PT=202) | Constant defined (`RTCP_PT_SDES = 202`) but no build/parse |
| 6.6 | BYE (PT=203) | Not implemented |
| 6.7 | APP (Application-Defined, PT=204) | Not implemented |
| 6.2 | RTCP transmission interval calculation | Fixed 5-second interval; spec requires bandwidth-adaptive timing |
| 6.3.5 | RTCP timer reconsideration | Not implemented |
| 6.2.1 | Minimum RTCP interval (5s reduced by random factor) | Not randomized |
| 7 | Translators and mixers | Not applicable (endpoint-only) |
| 8 | SSRC collision detection | Not implemented |
| 13 | RTP over TCP | Not implemented (UDP only) |

---

## RFC 3711 — SRTP

**"The Secure Real-time Transport Protocol"**

Files: `rtc/src/rtc_srtp.c`, `rtc/src/rtc_srtp.h`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 3.1 | SRTP packet format (RTP header + encrypted payload + auth tag) | Full protection/unprotection |
| 3.3 | Authentication tag (HMAC-SHA1-80, 10 bytes) | Appended on protect, verified on unprotect |
| 3.4 | SRTCP (encrypted RTCP + SRTCP index + auth tag) | Full protect/unprotect with E-flag |
| 4.1.1 | IV construction (salt ⊕ (SSRC ∥ packet_index)) | Correctly implemented for AES-128-CM |
| 4.1.1 | AES-128-CM (Counter Mode) encryption | Using OpenSSL EVP API |
| 4.3.1 | Key Derivation Function (KDF) | AES-CM PRF with label-based derivation |
| 4.3.1 | Session key, salt, and auth key derivation | Separate derivation for RTP and RTCP |
| 3.3.1 | Rollover Counter (ROC) for 48-bit index | ROC tracking on 16-bit sequence wrap |
| — | Session key caching | Keys cached and re-derived only when necessary |

### Not Implemented

| Section | Feature | Impact |
|---------|---------|--------|
| 3.3.2 | **Replay protection (replay list)** | **CRITICAL** — Captured packets can be replayed. RFC requires a replay list with configurable window (default 64). |
| 3.3.1 | ROC guessing for out-of-order packets | Partial — only simple wrap detection, no extended guessing algorithm |
| 4.1.2 | AES-256-CM cipher suite | Only AES-128-CM supported |
| 4.1.3 | NULL cipher (for debugging) | Not supported |
| 4.2.1 | HMAC-SHA1-32 (truncated to 32 bits) | Only 80-bit tag supported |
| 9 | Master Key Index (MKI) | Not implemented; single master key assumed |
| 9.1 | Key management / rekeying | No key rotation mechanism |
| 3.1 | SRTP buffer size validation | `protect` appends auth tag without verifying buffer capacity |

---

## RFC 4566 — SDP

**"SDP: Session Description Protocol"**

Files: `rtc/src/rtc_sdp.c`, `rtc/include/rtc/rtc_sdp.h`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 5.1 | `v=` (protocol version = 0) | Generated and parsed |
| 5.2 | `o=` (origin: username, session ID, version, network type, address) | Generated with fixed values |
| 5.3 | `s=` (session name) | Fixed "mrtc" |
| 5.7 | `t=` (timing = "0 0") | Generated |
| 5.14 | `m=` (media descriptions: audio, video, application) | Multi-media line generation and parsing |
| 5.7 | `c=` (connection data) | Placeholder `0.0.0.0` |
| 5.13 | `a=` (attributes) | Extensive attribute support (see below) |
| — | `a=rtpmap:` codec parameters | Payload type, codec name, clock rate, channels |
| — | `a=fingerprint:` DTLS fingerprint | SHA-256 hex fingerprint |
| — | `a=setup:` DTLS role | active / passive / actpass |
| — | `a=ice-ufrag:` / `a=ice-pwd:` | Per-media ICE credentials |
| — | `a=candidate:` lines | Full candidate formatting and parsing |
| — | `a=mid:` media identification | Per-media section MID tag |
| — | `a=group:BUNDLE` | BUNDLE group for all media sections |
| — | `a=rtcp-mux` | Generated for all media sections |
| — | `a=sendrecv` / `a=sendonly` / `a=recvonly` / `a=inactive` | Direction attribute generation |
| — | `m=application ... webrtc-datachannel` | Data channel media line with `a=sctp-port:5000` |

### Not Implemented

| Section | Feature | Notes |
|---------|---------|--------|
| 5.8 | `b=` (bandwidth) | Not generated or parsed |
| 5.2 | Origin version incrementing | Fixed version, not incremented on reoffer |
| 5.4 | `i=` (session/media information) | Not generated |
| 5.5 | `u=` (URI) | Not generated |
| 5.6 | `e=` / `p=` (email/phone) | Not generated |
| 5.9 | `z=` (time zones) | Not generated |
| 5.10 | `k=` (encryption keys) — deprecated | Not generated (correct to omit) |
| — | `a=fmtp:` (format parameters) | Not generated; no codec-specific params |
| — | `a=extmap:` (RTP header extensions) | Not generated; needed for transport-cc, abs-send-time |
| — | `a=ssrc:` (SSRC attributes) | Not generated |
| — | `a=rtcp-fb:` (RTCP feedback capabilities) | Not generated; needed for NACK/PLI/FIR |
| — | `a=ice-options:trickle` | No trickle ICE support |
| — | Full SDP parsing | Minimal parser sufficient for WebRTC; not RFC-complete |

### Known Bugs

- **Buffer overflow in `sdp_write_media_section`**: `*remain -= n` unsigned underflow wraps to `SIZE_MAX`, causing writes past `sdp->raw[8192]`. Remote-triggerable via many candidates.
- **Parse underflow in `rtc_sdp_parse`**: `vlen = line_len - prefix_len` underflows when `line_len < prefix_len`, causing massive `memcpy` overflow.
- **Relay candidates labeled "host"**: Missing `ICE_CANDIDATE_RELAY` case defaults to "host" type string.

---

## RFC 4585 — RTCP Feedback

**"Extended RTP Profile for RTCP-Based Feedback (RTP/AVPF)"**

📋 **Planned for Phase 2** (see ROADMAP.md)

| Feature | Status |
|---------|--------|
| Generic NACK (negative acknowledgement) | ❌ Not implemented |
| PLI (Picture Loss Indication) | ❌ Not implemented |
| FIR (Full Intra Request) | ❌ Not implemented |
| REMB (Receiver Estimated Maximum Bitrate) | ❌ Not implemented |
| NACK retransmit buffer | ❌ Not implemented |
| `a=rtcp-fb:` SDP negotiation | ❌ Not implemented |

---

## RFC 5109 — UlpFEC

**"RTP Payload Format for Generic Forward Error Correction"**

📋 **Planned for Phase 4** (see ROADMAP.md)

| Feature | Status |
|---------|--------|
| XOR-based FEC packet generation | ❌ Not implemented |
| FEC recovery at receiver | ❌ Not implemented |
| `a=rtpmap:XX ulpfec/90000` SDP | ❌ Not implemented |

---

## RFC 5389 — STUN

**"Session Traversal Utilities for NAT"**

Files: `rtc/src/rtc_stun.c`, `rtc/include/rtc/rtc_stun.h`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 6 | STUN message format (20-byte header: type, length, magic cookie, transaction ID) | Full serialization and parsing |
| 6 | Magic Cookie (`0x2112A442`) | Validated on parse |
| 6.1 | Message types: Binding Request (0x0001), Success Response (0x0101) | Build and parse |
| 15.1 | MAPPED-ADDRESS (0x0001) | Parsed |
| 15.2 | XOR-MAPPED-ADDRESS (0x0020) | Build and parse with XOR encoding |
| 15.4 | MESSAGE-INTEGRITY (0x0008) | HMAC-SHA1 generation and verification |
| 15.5 | FINGERPRINT (0x8028) | CRC-32 ⊕ 0x5354554E generation and verification |
| 15.3 | USERNAME (0x0006) | Build for ICE connectivity checks |
| 15.6 | ERROR-CODE (0x0009) | Parsed (used for TURN 401 handling) |
| 15.10 | SOFTWARE (0x8022) | Written as "mrtc" |
| — | STUN Binding Request to public server | Blocking request with timeout |
| — | Transaction ID generation | Random 96-bit IDs |
| — | Long-term credential mechanism (MD5(user:realm:pass)) | For TURN authentication |

### ICE-Specific Attributes (RFC 8445)

| Attribute | Code | Status |
|-----------|------|--------|
| PRIORITY (0x0024) | Implemented | Written in connectivity check requests |
| USE-CANDIDATE (0x0025) | Implemented | Flag attribute for nomination |
| ICE-CONTROLLED (0x8029) | Implemented | With tie-breaker |
| ICE-CONTROLLING (0x802A) | Implemented | With tie-breaker (always used) |

### TURN Attributes (RFC 5766)

| Attribute | Code | Status |
|-----------|------|--------|
| CHANNEL-NUMBER (0x000C) | ✅ | |
| LIFETIME (0x000D) | ✅ | |
| XOR-PEER-ADDRESS (0x0012) | ✅ | |
| DATA (0x0013) | ✅ | |
| REALM (0x0014) | ✅ | |
| NONCE (0x0015) | ✅ | |
| XOR-RELAYED-ADDRESS (0x0016) | ✅ | |
| REQUESTED-TRANSPORT (0x0019) | ✅ | |

### Not Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 7.2.2 | Error response handling (beyond 401) | Only 401 Unauthorized handled for TURN |
| 7.2.1 | Retransmission (RTO doubling) | Single attempt with fixed timeout |
| 10 | Short-term credential mechanism | Not used (long-term credentials only) |
| 15.7 | REALM attribute build | Parsed from server response but not self-generated |
| 15.8 | NONCE attribute build | Parsed from server response but not self-generated |
| — | STUN over TCP/TLS | UDP only |
| — | ALTERNATE-SERVER (0x8023) | Not handled |

### Known Bug

- **IPv6 OOB read**: Checks `alen >= 8` but IPv6 MAPPED-ADDRESS needs 20 bytes. Reads past buffer for IPv6 addresses.

---

## RFC 5764 — DTLS-SRTP

**"Datagram Transport Layer Security (DTLS) Extension to Establish Keys for the Secure Real-time Transport Protocol (SRTP)"**

Files: `rtc/src/rtc_dtls.c`, `rtc/src/rtc_dtls.h`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 4.1 | SRTP protection profile negotiation | `SRTP_AES128_CM_SHA1_80` registered via `SSL_CTX_set_tlsext_use_srtp` |
| 4.2 | Key material export (`EXTRACTOR-dtls_srtp`) | `SSL_export_keying_material` called after handshake |
| 4.2 | Client/server key material split | Correct split into client_write/server_write key+salt pairs |
| — | SDP `a=fingerprint:sha-256` | SHA-256 fingerprint extracted and exchanged |
| — | SDP `a=setup:` role signaling | active/passive/actpass role from SDP |

### Not Implemented

| Feature | Notes |
|---------|-------|
| Multiple SRTP profile negotiation | Only `SRTP_AES128_CM_SHA1_80`; no `SRTP_AEAD_AES_128_GCM` etc. |
| Fingerprint verification against SDP | Callback always accepts; fingerprint compared separately (if at all) |

---

## RFC 5766 — TURN

**"Traversal Using Relays around NAT"**

Files: `rtc/src/rtc_turn.c`, `rtc/include/rtc/rtc_turn.h`, `turn/turn_handler.c`, `turn/turn_server.c`

### Implemented (Client)

| Section | Feature | Notes |
|---------|---------|-------|
| 6 | Allocate Request | With 401 challenge-response |
| 7 | Refresh Request | Including lifetime=0 for deallocation |
| 9 | CreatePermission Request | Single peer address |
| 11 | ChannelBind Request | Channel number 0x4000 |
| 11.4 | ChannelData framing | 4-byte header (channel + length) + payload |
| — | Long-term credential mechanism | MD5(username:realm:password) |
| — | 401 Unauthorized challenge response | Realm + nonce extraction and retry |

### Implemented (Server)

| Feature | Notes |
|---------|-------|
| Allocate handling | UDP relay socket allocation |
| Refresh handling | Lifetime extension and deallocation |
| CreatePermission | Permission storage per allocation |
| ChannelBind | Channel ↔ peer address mapping |
| ChannelData relay | Bidirectional forwarding |
| Long-term credentials | MD5-based authentication |

### Not Implemented

| Section | Feature | Impact |
|---------|---------|--------|
| 5 | **Relay candidate integration with ICE** | TURN allocations are not wired into ICE candidate gathering |
| 7.2 | Permission refresh timers (5 min) | No timer-based refresh — permissions expire silently |
| 11.3 | Channel refresh timers (10 min) | No timer-based refresh — channels expire |
| 7.1 | Allocation refresh timers | No automatic refresh before allocation expires |
| — | Multiple allocations / permissions | Single allocation per client, single permission |
| — | Event-driven integration | Blocking socket operations, not integrated with transport I/O poller |
| — | TCP relaying | UDP only |
| — | TURN over TLS/DTLS | UDP only |

### Known Bugs

- **Auth key truncation**: Binary MD5 `lt_key` passed as `const char*` to HMAC functions using `strlen()`. Null bytes truncate key (~63% chance of breakage).
- **Nonce never rotated** (server): Single nonce for server lifetime enables replay attacks.

---

## RFC 6347 — DTLS 1.2

**"Datagram Transport Layer Security Version 1.2"**

Files: `rtc/src/rtc_dtls.c`, `rtc/src/rtc_dtls.h`

### Implemented

| Feature | Notes |
|---------|-------|
| DTLS handshake (client and server roles) | Full handshake via OpenSSL memory BIOs |
| Self-signed EC certificate (P-256) | Modern `EVP_PKEY_CTX` API (OpenSSL 3.0+) |
| SHA-256 fingerprint extraction | Hex-formatted for SDP exchange |
| Flight retransmission | `DTLSv1_handle_timeout` + timer integration |
| State machine | NEW → CONNECTING → CONNECTED / FAILED / CLOSED |
| Memory BIO mode | Application controls UDP transmission |
| SRTP profile registration | `SRTP_AES128_CM_SHA1_80` via `SSL_CTX_set_tlsext_use_srtp` |
| Application data after handshake | `SSL_read` / `SSL_write` for data channel messages |

### Not Implemented

| Feature | Notes |
|---------|-------|
| DTLS 1.3 | Uses DTLS 1.2 only |
| Client certificate verification | Callback always accepts (`SSL_VERIFY_PEER` with dummy callback) |
| Connection ID extension | Not used |
| DTLS renegotiation | Not supported |

---

## RFC 6455 — WebSocket

**"The WebSocket Protocol"**

Files: `signaling/src/signaling_client.c`, `signaling/server/signaling_server.c`

### Implemented

| Feature | Notes |
|---------|-------|
| WebSocket client connections | Via libwebsockets library |
| WebSocket server | Via libwebsockets library |
| Text message framing | JSON signaling messages |
| Connection lifecycle | Open, message, close callbacks |

> Implementation delegated entirely to libwebsockets. No raw RFC 6455 framing code.

---

## RFC 7587 — Opus RTP Payload

**"RTP Payload Format for the Opus Speech and Audio Codec"**

Files: `media/src/codec_opus.c`, `media/src/audio_codec.c`

### Implemented

| Feature | Notes |
|---------|-------|
| Opus encoding/decoding | libopus, 48 kHz fixed sample rate |
| RTP packetization | One Opus frame per RTP packet |
| Mono and stereo | Configurable channels |
| Variable bitrate | Opus VBR mode |
| 20ms frame duration | Fixed at 960 samples per frame (48kHz × 20ms) |

### Not Implemented

| Feature | Notes |
|---------|-------|
| `a=fmtp:` Opus parameters | No `maxaveragebitrate`, `stereo`, `useinbandfec` signaling |
| FEC (in-band Opus FEC) | Opus supports built-in FEC; not enabled |
| DTX (Discontinuous Transmission) | Not enabled |
| CBR mode | Always VBR |
| Multiple frames per packet | Single frame only |

---

## RFC 7741 — VP8 RTP Payload

**"RTP Payload Format for VP8 Video"**

Files: `media/src/vp8_packetizer.c`, `media/src/vp8_packetizer.h`, `media/src/codec_vp8.c`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 4.2 | VP8 payload descriptor (1-byte) | S-bit (start of partition), PID |
| 4.2 | Marker bit (M) on last packet of frame | Correct frame boundary signaling |
| — | Packetization (split frame into MTU-sized RTP packets) | Max 1200 bytes per packet |
| — | Depacketization (reassemble frame from RTP packets) | Sequence-ordered reassembly |
| — | Keyframe detection | Via VP8 payload header (P-bit in first byte) |
| — | VP8 encoding/decoding | libvpx integration |

### Not Implemented

| Section | Feature | Impact |
|---------|---------|--------|
| 4.2 | **2-byte PictureID (M-bit extension)** | Breaks interop with peers sending extended PictureID |
| 4.2 | TL0PICIDX (temporal layer index) | No temporal scalability |
| 4.2 | TID (temporal layer ID) | No SVC support |
| 4.2 | KEYIDX (key frame index) | Not parsed |
| 4.4 | VP8 payload descriptor extensions (X-bit) | I, L, T, K extension bits ignored |

---

## RFC 7983 — Multiplexing STUN/DTLS/RTP/TURN

**"Multiplexing Scheme Updates for Secure Real-time Transport Protocol (SRTP) Extension for Datagram Transport Layer Security (DTLS)"**

Files: `rtc/src/rtc_transport.c`

### Implemented

| Feature | Notes |
|---------|-------|
| First-byte classification | Full demux table: STUN (0–3), DTLS (20–63), TURN ChannelData (64–79), RTP/RTCP (128–191) |
| Single UDP socket multiplexing | All protocols share one transport socket |
| Callback dispatch per packet type | Registered handlers for STUN, DTLS, RTP |

> **Fully compliant** with RFC 7983 packet classification.

---

## RFC 8445 — ICE

**"Interactive Connectivity Establishment"**

Files: `rtc/src/rtc_ice.c`, `rtc/src/rtc_ice.h`

### Implemented

| Section | Feature | Notes |
|---------|---------|-------|
| 5.1.1 | Host candidate gathering | Interface enumeration via `getifaddrs` (Linux/macOS) / `GetAdaptersAddresses` (Windows) |
| 5.1.1 | Server-reflexive (SRFLX) candidate gathering | STUN Binding Request to configured STUN server |
| 5.1.2.1 | Candidate priority calculation | `priority = (type_pref << 24) + (local_pref << 8) + (256 - component_id)` |
| 7 | Connectivity checks via STUN Binding Requests | With USERNAME, PRIORITY, ICE-CONTROLLING, USE-CANDIDATE |
| 7 | STUN Binding Response generation | For incoming connectivity checks |
| — | ICE credential generation | Random `ufrag` and `pwd` |
| — | State machine | NEW → GATHERING → CHECKING → CONNECTED / FAILED / CLOSED |
| — | ICE-controlling role | Always controlling with tie-breaker |
| — | Remote credential and candidate storage | From SDP parsing |

### Not Implemented

| Section | Feature | Impact |
|---------|---------|--------|
| 5.1.1.2 | **Relay (TURN) candidate gathering** | TURN client exists but not wired into ICE gather |
| 5.1.3 | **Trickle ICE** | All candidates gathered before offer; no incremental signaling |
| 7.3 | **ICE-controlled role** | Always controlling; no role negotiation with peer |
| 7.2.5.3 | Triggered checks | Not implemented |
| 8 | **Regular nomination** | Simplified: first successful check wins |
| 8 | Aggressive nomination | Not explicitly supported |
| 6 | Candidate pair formation / checklist ordering | Simplified: ordered by priority, not full pair algorithm |
| 7.2.5.2.2 | Peer reflexive candidate discovery | Not implemented |
| 11 | ICE keepalives (STUN Binding Indications) | Not implemented |
| 12 | ICE restart | Not implemented |
| — | Multiple components (RTP + RTCP) | Single component only (RTCP-mux assumed) |
| — | IPv6 candidate gathering | Not implemented |
| — | `a=end-of-candidates` signaling | Not implemented |

### Known Bug

- **ICE/transport socket race**: Synchronous `recvfrom()` in ICE on same socket that transport thread polls. Either thread can steal packets.

---

## RFC 8829 — JSEP

**"JavaScript Session Establishment Protocol"**

Files: `rtc/src/rtc_peer.c`, `rtc/include/rtc/rtc_peer.h`

### Implemented

| Feature | Notes |
|---------|-------|
| `createOffer()` | Generates SDP from transceivers + ICE + DTLS state |
| `createAnswer()` | Generates answer SDP after remote offer is set |
| `setLocalDescription()` | Applies local SDP, starts ICE gathering |
| `setRemoteDescription()` | Applies remote SDP, starts connection if both set |
| `addTrack()` | Creates transceiver with sendrecv direction |
| `addIceCandidate()` | Adds trickle ICE candidate |
| Signaling state machine | stable → have-local-offer → stable (on answer) |
| Automatic ICE+DTLS+SRTP after both descriptions set | Connection starts on transport thread |

### Not Implemented

| Feature | Notes |
|---------|-------|
| `removeTrack()` | No track removal |
| `restartIce()` | No ICE restart |
| Rollback (set local desc type="rollback") | Not supported |
| `getTransceivers()` returns copy | Returns direct pointers |
| Renegotiation (subsequent offer/answer) | Not supported |
| `RTCRtpTransceiver.stop()` | Not implemented |
| Unified Plan compliance | Basic transceiver model, not full Unified Plan |

---

## RFC 8834 — Media Transport and Use of RTP

**"Media Transport and Use of RTP in WebRTC"**

Files: `rtc/src/rtc_peer.c`, `rtc/src/rtc_rtp.c`

### Implemented

| Feature | Notes |
|---------|-------|
| RTP/RTCP multiplexing on single port | Via `a=rtcp-mux` and RFC 7983 demux |
| SRTP mandatory encryption | All RTP/RTCP protected via SRTP/SRTCP |
| BUNDLE (single transport for all media) | All m= lines share one ICE/DTLS/SRTP transport |
| Dynamic payload types | Negotiated via SDP |

### Not Implemented

| Feature | Notes |
|---------|-------|
| RTP header extensions | No `abs-send-time`, `transport-cc`, `mid`, `audio-level` |
| Reduced-size RTCP | Always full-size SR/RR |
| Bandwidth adaptation based on RTCP | AIMD only; no GCC/Transport-CC |
| SSRC multiplexing per spec | O(n) lookup, no proper demux table |

---

## RFC 8839 — ICE SDP Attributes

**"Session Description Protocol (SDP) Offer/Answer Procedures for Interactive Connectivity Establishment (ICE)"**

Files: `rtc/src/rtc_sdp.c`

### Implemented

| Feature | Notes |
|---------|-------|
| `a=ice-ufrag:` | Per-media section |
| `a=ice-pwd:` | Per-media section |
| `a=candidate:` | Full candidate line formatting with foundation, component, protocol, priority, address, port, type |
| `a=fingerprint:` | SHA-256 fingerprint |
| `a=setup:` | DTLS role (actpass for offerer, active for answerer) |

### Not Implemented

| Feature | Notes |
|---------|-------|
| `a=ice-options:trickle` | No trickle ICE |
| `a=ice-lite` | Not supported |
| `a=end-of-candidates` | Not signaled |
| `a=remote-candidates` | Not supported |

---

## RFC 8888 — Transport-CC

**"RTP Control Protocol (RTCP) Feedback for Congestion Control"**

📋 **Planned for Phase 3** (see ROADMAP.md)

| Feature | Status |
|---------|--------|
| Transport-wide sequence numbers | ❌ Not implemented |
| `abs-send-time` RTP header extension | ❌ Not implemented |
| RTCP Transport Feedback packets | ❌ Not implemented |
| Delay-based bandwidth estimation | ❌ Not implemented |
| Google Congestion Control (GCC) | ❌ Not implemented |

---

## Other Relevant Standards (Not Explicitly Referenced)

| Standard | Topic | Status |
|----------|-------|--------|
| RFC 8831 | WebRTC Data Channels | ⚠️ Custom wire protocol over DTLS; not SCTP-based per spec |
| RFC 4960 | SCTP | ❌ Not implemented (data channels use custom DTLS framing) |
| RFC 8261 | SCTP over DTLS | ❌ Not implemented |
| RFC 8826 | WebRTC Security Architecture | ⚠️ DTLS+SRTP used; no consent freshness, no SRTP replay protection |
| RFC 7675 | STUN Consent Freshness | ❌ Not implemented |
| RFC 3264 | SDP Offer/Answer Model | ⚠️ Basic offer/answer works; no renegotiation |
| RFC 5245 | ICE (obsoleted by 8445) | N/A — Uses RFC 8445 |

---

## Implementation Priority Recommendations

Based on the ROADMAP and current gaps:

1. **Phase 2 (RTCP Feedback)** — NACK, PLI, FIR, REMB are critical for video quality and interop with browsers.
2. **Phase 3 (Bandwidth Estimation)** — Transport-CC and GCC needed for real-world network adaptation.
3. **RFC 3711 Replay Protection** — Security-critical gap; should be addressed immediately.
4. **RFC 8445 ICE Controlled Role** — Required for interop when peer is the offerer.
5. **RFC 7741 2-byte PictureID** — Required for interop with Chrome/Firefox VP8 streams.
6. **Phase 4 (FEC)** — Important for lossy networks.
