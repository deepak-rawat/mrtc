# Copilot Instructions for mrtc

## Build & Run

See [README.md](../README.md) for build commands, test list, and run instructions.

Quick reference:
```bash
cd build && ninja         # Windows (MSYS2 UCRT64)
cd build && make          # Linux
```

## Architecture

See [ARCHITECTURE.md](../ARCHITECTURE.md) for the layer diagram, project structure,
and library dependency graph. Per-component design docs:
- `common/ARCHITECTURE.md` — platform abstractions
- `rtc/ARCHITECTURE.md` — core WebRTC protocol stack
- `media/ARCHITECTURE.md` — codecs, jitter buffer, media pipeline
- `signaling/ARCHITECTURE.md` — WebSocket signaling client + server
- `turn/ARCHITECTURE.md` — TURN relay server
- `conference/ARCHITECTURE.md` — multi-peer conferencing
- `app/conf_sdl/ARCHITECTURE.md` — SDL3 video conference application

## Key Conventions

### API Patterns
- All public functions use `rtc_` prefix followed by module name (e.g., `rtc_stun_`, `rtc_ice_`, `rtc_peer_`)
- Functions return `rtc_err_t` (0 = `RTC_OK`, negative = error)
- Components follow init/close lifecycle: `rtc_xxx_init()` / `rtc_xxx_close()`
- Output parameters are passed as pointers (first param for "method-like" functions)

### Error Handling
Error codes are defined in `rtc_common.h`:
- `RTC_OK` (0) - Success
- `RTC_ERR_GENERIC` (-1) through `RTC_ERR_SDP` (-9) for specific failures

### Platform Abstraction
- Socket type: `rtc_socket_t` (abstracts `SOCKET` on Windows, `int` on Unix)
- Thread type: `rtc_thread_t` (`HANDLE` on Windows, `pthread_t` on Unix)
- Mutex type: `rtc_mutex_t` (`CRITICAL_SECTION` on Windows, `pthread_mutex_t` on Unix)
- Condition variable: `rtc_cond_t` (`CONDITION_VARIABLE` on Windows, `pthread_cond_t` on Unix)
- Use `RTC_INVALID_SOCKET` instead of -1 or `INVALID_SOCKET`
- Platform helpers: `rtc_close_socket()`, `rtc_set_nonblocking()`, `rtc_time_ms()`
- Threading helpers: `rtc_thread_create()`, `rtc_thread_join()`, `rtc_mutex_*()`, `rtc_cond_*()`
- I/O poller: `rtc_poller_t` (epoll on Linux, kqueue on macOS, select on Windows)

### Transport Layer Ownership
- `rtc_transport_t` owns the UDP socket and runs a background thread with I/O poller
- Classifies incoming packets per RFC 7983 (STUN/DTLS/RTP) and dispatches via callback
- ICE agent borrows a pointer to transport (`rtc_transport_t *transport`), does NOT own it
- Peer connection owns the transport (`rtc_transport_t transport` embedded in struct)
- Send functions (`rtc_transport_send()`, `rtc_transport_send_to_remote()`) are thread-safe
- Recv callback fires on the transport's background thread — callers must protect shared state

### Logging
Use the logging macros from `rtc_common.h`:
```c
RTC_LOG_ERR("Error: %s", msg);
RTC_LOG_WARN("Warning: %s", msg);
RTC_LOG_INFO("Info: %s", msg);
RTC_LOG_DBG("Debug: %s", msg);
```

### Constants & Naming
- Protocol constants use `#define` with module prefix (e.g., `STUN_MAGIC_COOKIE`, `STUN_HEADER_SIZE`)
- Config structs: `rtc_xxx_config_t` passed to init functions
- State structs: `rtc_xxx_t` (e.g., `rtc_ice_agent_t`, `rtc_dtls_transport_t`)
- Callbacks: `rtc_on_xxx_fn` typedefs with user data pointer

### OpenSSL Usage
Uses modern EVP APIs (3.0+). Avoid deprecated functions like `HMAC()` or low-level cipher APIs.

### Testing
Tests use a minimal harness (`tests/test_harness.h`) with `TEST()`, `RUN_TEST()`, `ASSERT*` macros. No external test framework. Each test is a standalone executable.
