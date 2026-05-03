# TURN Server

Minimal TURN relay server (RFC 5766) for development and testing.

## Architecture

Single-threaded UDP server that processes TURN allocation requests and relays traffic between peers.

```
Client A ──UDP──→ ┌─────────────┐ ──UDP──→ Client B
                  │ TURN Server │
Client A ←──UDP── │  (relay)    │ ←──UDP── Client B
                  └─────────────┘
```

## Components

| File | Responsibility |
|------|---------------|
| `turn_server.c` | Main UDP event loop, socket setup |
| `turn_handler.c/h` | TURN protocol handling |

## Protocol Support

- **Allocate** — assign relay transport address
- **Refresh** — keep allocation alive
- **CreatePermission** — authorize peer addresses
- **ChannelBind** — map channel numbers to peers for efficient forwarding
- **ChannelData** — framed relay data (4-byte header: channel + length)
- **Long-term credentials** — MD5(user:realm:pass) + nonce validation

## Current Status

Minimal implementation for local development. Not yet integrated with the main application's ICE gathering (relay candidates not gathered).

## Dependencies

- **OpenSSL** — HMAC for credential validation
- **libmrtc_common** — types, STUN message parsing
