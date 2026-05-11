# Common Library (libmrtc_common)

Cross-platform abstractions for sockets, threading, timing, and logging shared by all components.

## Public API (`include/rtc/rtc_common.h`)

### Error Handling
- `rtc_err_t` — standard return type (0 = `RTC_OK`, negative = error)
- Error codes: `RTC_ERR_GENERIC`, `RTC_ERR_SOCKET`, `RTC_ERR_STUN`, `RTC_ERR_ICE`, `RTC_ERR_DTLS`, `RTC_ERR_SRTP`, `RTC_ERR_TIMEOUT`, `RTC_ERR_INVALID`, `RTC_ERR_SDP`

### Socket Abstraction
- `rtc_socket_t` — `SOCKET` on Windows, `int` on Unix
- `rtc_addr_t` — IP address + port
- `RTC_INVALID_SOCKET` — platform-agnostic invalid sentinel
- `rtc_addr_from_string()` / `rtc_addr_to_string()` — parse/format IP:port
- `rtc_close_socket()`, `rtc_set_nonblocking()`

### Threading
- `rtc_thread_t` — `HANDLE` on Windows, `pthread_t` on Unix
- `rtc_mutex_t` — `CRITICAL_SECTION` on Windows, `pthread_mutex_t` on Unix
- `rtc_cond_t` — `CONDITION_VARIABLE` on Windows, `pthread_cond_t` on Unix
- Functions: `rtc_thread_create()`, `rtc_thread_join()`, `rtc_mutex_init/lock/unlock/destroy()`, `rtc_cond_init/wait/signal/destroy()`

### Utilities
- `rtc_random_bytes(buf, len)` — cryptographic random (via OpenSSL)
- `rtc_random_string(buf, len)` — alphanumeric random string
- `rtc_time_ms()` — monotonic millisecond clock

### Logging
Macro-based with compile-time levels:
```c
RTC_LOG_ERR("Error: %s", msg);
RTC_LOG_WARN("Warning: %s", msg);
RTC_LOG_INFO("Info: %s", msg);
RTC_LOG_DBG("Debug: %s", msg);
```

Configuration:
- `rtc_set_log_level(level)` — set minimum log level
- `rtc_set_log_file(path)` — redirect output to file (in addition to stderr)
- `rtc_log_close()` — close log file

## Design Decisions

- **No allocator:** all structures are stack-allocated, users provide memory
- **Preprocessor dispatch:** `#ifdef _WIN32` selects Win32 vs POSIX implementations
- **Minimal surface:** only abstractions actually needed by the project

## Implementation

Single source file: `src/rtc_common.c` (~400 LOC)

## Dependencies

- **OpenSSL** — `rtc_random_bytes()` uses `RAND_bytes()`
- **pthread** (Unix) / **WinAPI** (Windows)
