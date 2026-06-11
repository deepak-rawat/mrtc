# mrtc — RFC Compliance Report

Status of each IETF RFC relevant to the WebRTC protocol stack as implemented in mrtc.

**Legend:**  
✅ Implemented · ⚠️ Partial · ❌ Not Implemented · 📋 Planned

---

## Summary

| RFC | Title | Status | Coverage | Successor |
|-----|-------|--------|----------|:---------:|
| [RFC 3550](#rfc-3550--rtprtcp) | RTP / RTCP | ⚠️ Partial | ~50% | Updated by 10 RFCs |
| [RFC 3711](#rfc-3711--srtp) | SRTP | ⚠️ Partial | ~75% | Updated by 6904, 8723 |
| [RFC 4566](#rfc-4566--sdp) | SDP | ⚠️ Partial | ~70% | **Obsoleted by 8866** |
| [RFC 4585](#rfc-4585--rtcp-feedback) | RTCP Feedback (NACK, PLI, FIR) | ⚠️ Partial | NACK/PLI/FIR implemented | Updated by 5506, 8108 |
| [RFC 5109](#rfc-5109--ulpfec) | UlpFEC | ❌ Not Impl | 0% | Current |
| [RFC 5389](#rfc-5389--stun) | STUN | ✅ Implemented | ~95% | **Obsoleted by 8489** |
| [RFC 5764](#rfc-5764--dtls-srtp) | DTLS-SRTP | ✅ Implemented | ~95% | Updated by 7983 |
| [RFC 5766](#rfc-5766--turn) | TURN | ⚠️ Partial | ~50% | **Obsoleted by 8656** |
| [RFC 6347](#rfc-6347--dtls-12) | DTLS 1.2 | ✅ Implemented | ~95% | **Obsoleted by 9147** |
| [RFC 6455](#rfc-6455--websocket) | WebSocket (signaling) | ✅ Implemented | via libwebsockets | Updated by 8441 |
| [RFC 7587](#rfc-7587--opus-rtp) | Opus RTP Payload | ✅ Implemented | ~90% | Current |
| [RFC 7741](#rfc-7741--vp8-rtp) | VP8 RTP Payload | ⚠️ Partial | ~85% | Current |
| [RFC 7983](#rfc-7983--packet-demux) | Multiplexing STUN/DTLS/RTP | ✅ Implemented | 100% | Current |
| [RFC 8445](#rfc-8445--ice) | ICE | ⚠️ Partial | ~45% | Updated by 8863 |
| RFC 8285 | One-byte/two-byte RTP header extensions | ⚠️ Partial | one-byte form | Current |
| [RFC 8829](#rfc-8829--jsep) | JSEP (offer/answer model) | ⚠️ Partial | ~60% | Current |
| [RFC 8834](#rfc-8834--media-transport) | Media Transport over RTP | ⚠️ Partial | ~50% | Current |
| [RFC 8839](#rfc-8839--ice-sdp) | ICE SDP Attributes | ⚠️ Partial | ~60% | Current |
| draft-holmer-rmcat-transport-wide-cc | Transport-Wide Congestion Control | ⚠️ Partial | wire + GCC | (was RFC 8888 row) |

---

## RFC 3550 — RTP/RTCP

**"RTP: A Transport Protocol for Real-Time Applications"**

> **Updated by:** RFC 5506 (reduced-size RTCP), RFC 5761 (RTP/RTCP mux on single port),
> RFC 6051 (rapid RTP sync), RFC 6222/7022 (CNAME generation), RFC 7160 (multiple media types),
> RFC 7164 (RTP/RTCP mux considerations), RFC 8083 (multimedia congestion feedback),
> RFC 8108 (SSRC-specific RTCP), RFC 8860 (deprecates RTP/RTCP transport address pairing).
> Still the base specification — not obsoleted.

Files: `rtc/src/rtc_rtp.c`, `rtc/src/rtc_rtcp.c`, `rtc/include/rtc/rtc_rtp.h`, `rtc/include/rtc/rtc_rtcp.h`

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

> **Updated by:** RFC 6904 (encrypted SRTP header extensions), RFC 8723 (SRTP double encryption).
> Still the base specification — not obsoleted.

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

> ⚠️ **Obsoleted by RFC 8866** (January 2021). RFC 8866 is a clarification update with no
> wire-format changes. Key differences: removes `k=` line entirely, clarifies `b=` semantics,
> tightens ABNF grammar, mandates ICE-era usage patterns. Migration effort: **low** —
> mrtc's minimal SDP subset is unaffected; simply update doc references.

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
| — | `a=extmap:` (RTP header extensions) | Generated and parsed for negotiated RTP extensions |
| — | `a=ssrc:` (SSRC attributes) | Generated and parsed for audio/video media |
| — | `a=rtcp-fb:` (RTCP feedback capabilities) | Not generated; needed for NACK/PLI/FIR |
| — | `a=ice-options:trickle` | Generated and parsed |
| — | Full SDP parsing | Minimal parser sufficient for WebRTC; not RFC-complete |

### Known Limitations

- SDP parsing remains intentionally minimal and accepts only the subset needed by this stack.
- **Relay candidates labeled "host"**: Missing `ICE_CANDIDATE_RELAY` case defaults to "host" type string.

---

## RFC 4585 — RTCP Feedback

**"Extended RTP Profile for RTCP-Based Feedback (RTP/AVPF)"**

> **Updated by:** RFC 5506 (allows reduced-size RTCP-FB without compound), RFC 8108 (SSRC-specific attributes).
> Still the base specification — not obsoleted.

| Feature | Status |
|---------|--------|
| Generic NACK (negative acknowledgement) | ✅ Build, parse, dispatch implemented |
| PLI (Picture Loss Indication) | ✅ Build, parse, dispatch implemented |
| FIR (Full Intra Request) | ✅ Build, parse, dispatch implemented |
| NACK retransmit buffer | ✅ Per-video sender buffer with logical raw retransmit |
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

> ⚠️ **Obsoleted by RFC 8489** (February 2020). Key changes: long-term auth mechanism v2
> (SHA-256 + PASSWORD-ALGORITHMS/PASSWORD-ALGORITHM attributes), ALTERNATE-DOMAIN attribute,
> STUN test vectors updated, nonce rotation required. Migration effort: **medium** —
> current long-term auth (MD5) still works but new servers may require auth v2.

Files: `rtc/src/rtc_stun.c`, `rtc/src/rtc_stun.h`

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

> **Updated by:** RFC 7983 (demux clarification — already implemented by mrtc).
> Still the base specification — not obsoleted.

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

> ⚠️ **Obsoleted by RFC 8656** (February 2020). Key changes: TURN over TCP/TLS/DTLS transports,
> dual-stack (IPv4+IPv6) allocation, REQUESTED-ADDRESS-FAMILY attribute, uses RFC 8489 auth.
> Migration effort: **medium** — current UDP-only TURN client works but modern TURN servers
> may require RFC 8489 long-term auth v2 and offer TURN-over-TLS.

Files: `rtc/src/rtc_turn.c`, `rtc/src/rtc_turn.h`

### Implemented (Client)

| Section | Feature | Notes |
|---------|---------|-------|
| 6 | Allocate Request | With 401 challenge-response for external TURN services |
| 7 | Refresh Request | Including lifetime=0 for deallocation |
| 9 | CreatePermission Request | Single peer address |
| 11 | ChannelBind Request | Channel number 0x4000 |
| 11.4 | ChannelData framing | 4-byte header (channel + length) + payload |
| — | Long-term credential mechanism | MD5(username:realm:password) |
| — | 401 Unauthorized challenge response | Realm + nonce extraction and retry |

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

> ⚠️ **Obsoleted by RFC 9147** (DTLS 1.3, April 2022). DTLS 1.3 aligns with TLS 1.3:
> 0-RTT, encrypted handshake, removed static RSA/DH, unified record layer.
> Migration effort: **high** — requires OpenSSL 3.2+ with DTLS 1.3 support (not yet widely
> available). DTLS 1.2 remains valid and interoperable for WebRTC; browsers still support it.

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

> **Updated by:** RFC 7936 (clarification), RFC 8307 (well-known URIs),
> RFC 8441 (WebSocket bootstrapping over HTTP/2). Still the base specification.
> Migration effort: **none** — handled by libwebsockets.

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

Files: `rtc/src/rtc_transport.c`, `rtc/src/rtc_listener.c`

### Implemented

| Feature | Notes |
|---------|-------|
| First-byte classification | Full demux table: STUN (0–3), DTLS (20–63), TURN ChannelData (64–79), RTP/RTCP (128–191) |
| Single UDP socket multiplexing | Shared listeners demux many logical transports on one UDP socket |
| Callback dispatch per packet type | STUN routed by ufrag/transaction, DTLS/RTP/RTCP routed by selected tuple |

> **Fully compliant** with RFC 7983 packet classification.

---

## RFC 8445 — ICE

**"Interactive Connectivity Establishment"**

> **Updated by:** RFC 8863 (Trickle ICE, January 2021). Defines incremental candidate
> exchange during gathering. Migration effort: **medium** — requires implementing
> `add_ice_candidate()`, SDP `a=ice-options:trickle`, and
> `a=end-of-candidates` signaling.

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
| — | Remote credential and candidate storage | From SDP parsing and trickled candidates |
| RFC 8863 | Trickle candidate ingestion | `rtc_peer_connection_add_ice_candidate()` parses and stores remote candidates |
| RFC 8839 | `a=ice-options:trickle` | Emitted and parsed |

### Not Implemented

| Section | Feature | Impact |
|---------|---------|--------|
| 5.1.1.2 | **Relay (TURN) candidate gathering** | TURN client exists but not wired into ICE gather |
| 5.1.3 | **Full local trickle gathering** | Local candidates are still emitted synchronously today |
| 7.3 | **ICE-controlled role** | Always controlling; no role negotiation with peer |
| 7.2.5.3 | Triggered checks | Not implemented |
| 8 | **Regular nomination** | Simplified: first successful check wins |
| 8 | Aggressive nomination | Not explicitly supported |
| 6 | Candidate pair formation / checklist ordering | Simplified: ordered by priority, not full pair algorithm |
| 7.2.5.2.2 | Peer reflexive candidate discovery | Not implemented |
| 11 | ICE keepalives (STUN Binding Indications) | Not implemented |
| 12 | ICE restart after connection | Restart is supported only before connection start |
| — | Multiple components (RTP + RTCP) | Single component only (RTCP-mux assumed) |
| — | IPv6 candidate gathering | Not implemented |
| — | `a=end-of-candidates` signaling | End-of-candidates is signaled via NULL callback, not SDP attribute |

### Known Limitations

- ICE checklist, nomination, role conflict, and consent freshness remain simplified.

---

## RFC 8829 — JSEP

**"JavaScript Session Establishment Protocol"**

Files: `client/src/rtc_peer.c`, `client/include/rtc/rtc_peer.h`

### Implemented

| Feature | Notes |
|---------|-------|
| `createOffer()` | Generates SDP from transceivers + ICE + DTLS state |
| `createAnswer()` | Generates answer SDP after remote offer is set |
| `setLocalDescription()` | Applies local SDP, starts ICE gathering |
| `setRemoteDescription()` | Applies remote SDP, starts connection if both set |
| `addTrack()` | Creates transceiver with sendrecv direction |
| `removeTrack()` | Marks sender inactive and updates transceiver direction before connection start |
| `addIceCandidate()` | Adds trickle ICE candidate |
| Signaling state machine | stable → have-local-offer → stable (on answer) |
| Automatic ICE+DTLS+SRTP after both descriptions set | Connection starts on the runtime logical transport |

### Not Implemented

| Feature | Notes |
|---------|-------|
| `restartIce()` after connection | Pre-connect restart exists; post-connect ICE restart is not supported |
| Rollback (set local desc type="rollback") | Not supported |
| `getTransceivers()` returns copy | Returns direct pointers |
| Renegotiation (subsequent offer/answer) | Not supported |
| `RTCRtpTransceiver.stop()` | Not implemented |
| Unified Plan compliance | Basic transceiver model, not full Unified Plan |

---

## RFC 8834 — Media Transport and Use of RTP

**"Media Transport and Use of RTP in WebRTC"**

Files: `client/src/rtc_peer.c`, `rtc/src/rtc_rtp.c`

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
| RTP header extensions | `transport-cc` and abs-send-time helpers exist; `mid` and audio-level are not wired |
| Reduced-size RTCP | Always full-size SR/RR |
| Bandwidth adaptation based on RTCP | GCC/Transport-CC implemented; encoder auto-wiring remains app-owned |
| SSRC multiplexing per spec | SSRC maps are used for peer sender/receiver demux |

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
| `a=ice-options:trickle` | Emitted and parsed |
| `a=ice-lite` | Not supported |
| `a=end-of-candidates` | Not signaled |
| `a=remote-candidates` | Not supported |

---

## RFC 8888 — Transport-CC

**"RTP Control Protocol (RTCP) Feedback for Congestion Control"**

Implemented for the draft-holmer transport-wide CC wire format used by the stack.

| Feature | Status |
|---------|--------|
| Transport-wide sequence numbers | ✅ Implemented |
| `abs-send-time` RTP header extension helper | ✅ Implemented helper |
| RTCP Transport Feedback packets | ✅ Build + parse implemented |
| Delay-based bandwidth estimation | ✅ Simplified GCC trendline estimator |
| Google Congestion Control (GCC) | ✅ Delay + loss controller implemented |

---

## Other Relevant Standards (Not Explicitly Referenced)

| Standard | Topic | Status | Successor |
|----------|-------|--------|-----------|
| RFC 8831 | WebRTC Data Channels | ⚠️ Custom wire protocol over DTLS; not SCTP-based per spec | Current |
| RFC 4960 | SCTP | ❌ Not implemented (data channels use custom DTLS framing) | **Obsoleted by RFC 9260** |
| RFC 8261 | SCTP over DTLS | ❌ Not implemented | Current |
| RFC 8826 | WebRTC Security Architecture | ⚠️ DTLS+SRTP used; no consent freshness, no SRTP replay protection | Current |
| RFC 7675 | STUN Consent Freshness | ❌ Not implemented | Current |
| RFC 3264 | SDP Offer/Answer Model | ⚠️ Basic offer/answer works; no renegotiation | Updated by RFC 6157, 8843 |
| RFC 5245 | ICE (obsoleted by 8445) | N/A — Uses RFC 8445 | Obsoleted by RFC 8445 |

---

## RFC Migration Plan

Roadmap for updating mrtc code from current (older) RFC implementations to their
successors, ordered by priority.

### Phase A: Critical Security & Interop (do first)

#### A1 — SRTP Replay Protection (RFC 3711 §3.3.2)
- **Why now:** Security-critical gap; captured packets can be replayed.
- **Work:** Add 64-packet sliding window replay list in `rtc_srtp.c`. Check packet index against window before decryption.
- **Files:** `rtc/src/rtc_srtp.c`, `rtc/src/rtc_srtp.h`
- **Effort:** Small

#### A2 — STUN → RFC 8489 Long-Term Auth v2
- **Why now:** RFC 5389 is obsoleted. Modern TURN servers (coturn 4.6+) prefer SHA-256 auth. Current MD5 long-term auth has known weaknesses.
- **Work:**
  - Add `PASSWORD-ALGORITHMS` and `PASSWORD-ALGORITHM` attribute support in `rtc_stun.c`
  - Implement SHA-256 based HMAC key derivation alongside MD5
  - Add `USERHASH` attribute (optional, privacy feature)
  - Support `NONCE` with `STUN-COOKIE` prefix for nonce rotation detection
- **Files:** `rtc/src/rtc_stun.c`, `rtc/src/rtc_stun.h`
- **Effort:** Medium

#### A3 — TURN → RFC 8656
- **Why now:** RFC 5766 is obsoleted. Depends on A2 (RFC 8489 auth).
- **Work:**
  - Add `REQUESTED-ADDRESS-FAMILY` attribute for dual-stack (IPv4+IPv6)
  - Support `ADDITIONAL-ADDRESS-FAMILY` for dual allocation
  - Wire TURN relay candidates into ICE gathering (`rtc_ice.c`)
  - Add allocation/permission/channel refresh timers
  - Fix TURN auth key truncation bug (pass `lt_key` as `uint8_t*` with explicit length, not `const char*`)
- **Files:** `rtc/src/rtc_turn.c`, `rtc/src/rtc_turn.h`, `rtc/src/rtc_ice.c`
- **Effort:** Large

### Phase B: RTCP & Feedback (aligns with ROADMAP Phase 2)

#### B1 — RTCP Feedback: NACK/PLI/FIR (RFC 4585 + RFC 5506)
- **Why now:** Required for video quality and browser interop.
- **Work:**
  - Implement Generic NACK (PT=205, FMT=1), PLI (PT=206, FMT=1), FIR (PT=206, FMT=4)
  - Add `a=rtcp-fb:` SDP generation and parsing
  - Support reduced-size RTCP (RFC 5506) — send FB without compound SR/RR+SDES
  - Add NACK retransmit buffer (512 packets) in sender
  - Wire PLI → `video_encoder_request_keyframe()`
- **Files:** `rtc/src/rtc_rtcp.c`, `rtc/include/rtc/rtc_rtcp.h`, `client/src/rtc_peer.c`, `rtc/src/rtc_sdp.c`, `rtc/include/rtc/rtc_sdp.h`
- **Effort:** Large

#### B2 — RTCP Compound Packets & SDES CNAME (RFC 3550 §6.1 + RFC 7022)
- **Why now:** Spec requires SR/RR+SDES in every compound packet. RFC 7022 updates CNAME generation.
- **Work:**
  - Generate compound RTCP (SR/RR + SDES CNAME) instead of bare SR/RR
  - Generate CNAME per RFC 7022 (random, persistent per session)
  - Implement bandwidth-adaptive RTCP interval (replace fixed 5s timer)
- **Files:** `rtc/src/rtc_rtcp.c`, `client/src/rtc_peer.c`
- **Effort:** Medium

#### B3 — RTP/RTCP Mux Compliance (RFC 5761)
- **Why now:** Already using single port, but should validate per RFC 5761 demux rules.
- **Work:** Verify RTCP PT range demux in `rtc_transport.c` matches RFC 5761 §4. Already mostly done via RFC 7983.
- **Files:** `rtc/src/rtc_transport.c`
- **Effort:** Small (audit only)

### Phase C: ICE Improvements

#### C1 — Full Trickle ICE Gathering (RFC 8863)
- **Why now:** All browsers use trickle ICE. Current all-in-SDP approach adds latency.
- **Work:**
  - Emit local candidates incrementally as gathering discovers them
  - Add `a=end-of-candidates` signaling
  - Handle dynamic checklist updates as remote candidates arrive
- **Files:** `rtc/src/rtc_ice.c`, `client/src/rtc_peer.c`, `rtc/src/rtc_sdp.c`
- **Effort:** Large

#### C2 — ICE Controlled Role (RFC 8445 §7.3)
- **Why now:** Required for interop when mrtc is the answerer.
- **Work:**
  - Add ICE-CONTROLLED attribute support (already defined in `rtc_stun.h`)
  - Implement role determination from SDP offer/answer (offerer=controlling, answerer=controlled)
  - Handle role conflict (tie-breaker comparison, 487 error)
  - Implement regular nomination (controlled side waits for USE-CANDIDATE)
- **Files:** `rtc/src/rtc_ice.c`, `client/src/rtc_peer.c`
- **Effort:** Medium

### Phase D: Bandwidth Estimation (aligns with ROADMAP Phase 3)

#### D1 — RTP Header Extensions (RFC 8834, RFC 5285)
- **Why now:** Required for transport-cc and abs-send-time.
- **Work:**
  - Implement one-byte header extension format (RFC 5285)
  - Add `a=extmap:` SDP generation and parsing
  - Support `abs-send-time`, `transport-cc` seq number, `mid`, `audio-level`
- **Files:** `rtc/src/rtc_rtp.c`, `rtc/src/rtc_sdp.c`, `rtc/include/rtc/rtc_rtp.h`
- **Effort:** Medium

#### D2 — Transport-CC (RFC 8888)
- **Why now:** Needed for delay-based bandwidth estimation.
- **Work:**
  - Sender: attach per-packet transport-wide sequence numbers via RTP header extension
  - Receiver: track arrival times, build RTCP Transport Feedback packets
  - Sender: compare send/recv deltas for delay-based estimation
- **Files:** `rtc/src/rtc_rtcp.c`, `client/src/rtc_peer.c`, new `rtc/src/rtc_bwe.c`
- **Effort:** Large

#### D3 — GCC (Google Congestion Control)
- **Why now:** Replace AIMD with proper delay-based + loss-based estimator.
- **Work:** Implement GCC algorithm using Transport-CC feedback. Replace `rtc_rate_control.c`.
- **Files:** new `rtc/src/rtc_bwe.c`, `rtc/src/rtc_rate_control.c`
- **Effort:** Large

### Phase E: SDP & Protocol Refresh (low urgency)

#### E1 — SDP → RFC 8866
- **Why now:** RFC 4566 is obsoleted, though changes are editorial.
- **Work:** Audit SDP generation/parsing against RFC 8866 ABNF. Remove any `k=` line handling. Update doc references.
- **Files:** `rtc/src/rtc_sdp.c`
- **Effort:** Small

#### E2 — SRTP Header Extension Encryption (RFC 6904)
- **Why now:** Browsers encrypt `mid` and `audio-level` header extensions by default.
- **Work:** After implementing header extensions (D1), add encryption for signaled extensions in SRTP protect/unprotect.
- **Files:** `rtc/src/rtc_srtp.c`
- **Effort:** Medium

#### E3 — DTLS 1.3 (RFC 9147)
- **Why now:** DTLS 1.2 remains interoperable; DTLS 1.3 is optional.
- **Work:** Wait for OpenSSL DTLS 1.3 support to stabilize (OpenSSL 3.2+). Then update `rtc_dtls.c` to negotiate DTLS 1.3 when available, falling back to 1.2.
- **Files:** `rtc/src/rtc_dtls.c`
- **Effort:** Medium (mostly OpenSSL API changes)

### Phase F: Forward Error Correction (aligns with ROADMAP Phase 4)

#### F1 — UlpFEC (RFC 5109)
- **Why now:** Important for lossy networks. No successor RFC — still current.
- **Work:** XOR-based FEC encode/decode, SDP `a=rtpmap:XX ulpfec/90000` negotiation.
- **Files:** new `rtc/src/rtc_fec.c`, `rtc/src/rtc_sdp.c`
- **Effort:** Large

### Migration Dependency Graph

```
A1 (SRTP replay)     ─── standalone, do first
A2 (STUN→8489)       ─── standalone
A3 (TURN→8656)       ─── depends on A2
B1 (RTCP feedback)   ─── standalone
B2 (RTCP compound)   ─── standalone
B3 (RTP/RTCP mux)    ─── standalone (audit)
C1 (Trickle ICE)     ─── standalone
C2 (ICE controlled)  ─── standalone
D1 (RTP extensions)  ─── prerequisite for D2, E2
D2 (Transport-CC)    ─── depends on D1
D3 (GCC)             ─── depends on D2
E1 (SDP→8866)        ─── standalone
E2 (SRTP hdr enc)    ─── depends on D1
E3 (DTLS 1.3)        ─── standalone (wait for OpenSSL)
F1 (UlpFEC)          ─── standalone
```

### Effort Summary

| Phase | Items | Total Effort | Key Benefit |
|-------|-------|-------------|-------------|
| A | 3 | Small + Medium + Large | Security, modern TURN interop |
| B | 3 | Large + Medium + Small | Video quality, spec compliance |
| C | 2 | Large + Medium | Connection speed, browser interop |
| D | 3 | Medium + Large + Large | Bandwidth adaptation |
| E | 3 | Small + Medium + Medium | Spec freshness, header privacy |
| F | 1 | Large | Loss resilience |
