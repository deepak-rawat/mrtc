# Common Library (libmrtc_common)

Cross-platform abstractions for sockets, threading, timing, logging, and generic
data structures shared by all components.

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

## Data Structures

Self-contained generic containers for hot-path lookups and growable lists
across mrtc components. All three are **not thread-safe** — caller serializes
access. Pointers returned by `at`/`get` are invalidated by any operation that
may grow or rehash the container.

### Dynamic Array (`include/rtc/rtc_vec.h`)
Byte-typed dynamic array with explicit element size.
- `rtc_vec_init(v, elem_size)` / `rtc_vec_free(v)`
- `rtc_vec_push(v, &elem)` — O(1) amortized, doubles capacity on grow
- `rtc_vec_at(v, idx)` — O(1) random access, NULL if out of range
- `rtc_vec_remove(v, idx)` — O(n), preserves order
- `rtc_vec_swap_remove(v, idx)` — O(1), does not preserve order
- `rtc_vec_reserve(v, min_cap)`, `rtc_vec_clear(v)`, `rtc_vec_len(v)`
- Use cases: SDP candidate list, TURN channels/permissions, conference peers/sources

### `uint32_t → void*` Hash Map (`include/rtc/rtc_u32_map.h`)
Open-addressing hash map for hot-path numeric-key lookups.
- Linear probing on power-of-2 sized table, Fibonacci hashing (`key * 2654435769u`)
- Grows at 75% load, compacts when tombstones ≥ 1/8 of cap and load < 1/2
- `rtc_u32_map_set/get/has/remove`, `rtc_u32_map_next` for iteration
- `NULL` is a valid stored value — use `has()` to disambiguate
- Use cases: SSRC → receiver lookup (per-packet hot path), TURN allocation lookup, data channel id → channel

### `string → void*` Hash Map (`include/rtc/rtc_str_map.h`)
Open-addressing hash map keyed by C strings.
- FNV-1a 32-bit hash, linear probing, cached per-entry hash
- Two key-ownership modes:
  - `rtc_str_map_init(m)` — borrowed keys (caller keeps strings alive)
  - `rtc_str_map_init_owned(m)` — keys are `strdup`'d and freed automatically
- Same API surface as `rtc_u32_map`
- Use cases: peer_id → peer, meeting name → meeting, label → source, UI tile lookup

## Design Decisions

- **No allocator:** platform helpers leave memory to the caller; data-structure
  containers use `malloc`/`realloc`/`free` directly
- **Preprocessor dispatch:** `#ifdef _WIN32` selects Win32 vs POSIX implementations
- **Minimal surface:** only abstractions actually needed by the project
- **Data structures are owned, not intrusive:** containers hold copies (`vec`)
  or pointers (`map` values); they do not mandate fields in user structs

## Implementation

| File | Purpose |
|------|---------|
| `src/rtc_common.c` | Platform abstractions, logging, random, time, addr helpers |
| `src/rtc_vec.c` | Dynamic array |
| `src/rtc_u32_map.c` | uint32 → void* open-addressing hash map |
| `src/rtc_str_map.c` | string → void* open-addressing hash map |

## Tests

| File | Coverage |
|------|----------|
| `tests/test_vec.c` | init/free, push/at, struct elements, reserve, remove/swap_remove, clear, stress, reuse |
| `tests/test_u32_map.c` | get-on-empty, set/get/overwrite, remove/reinsert, NULL values, rehash, stress, iteration, key=0 |
| `tests/test_str_map.c` | borrowed & owned modes, overwrite, remove, NULL values, empty key, rehash, stress, iteration, invalid args |

Each test binary is registered with CTest and built when `MRTC_BUILD_TESTS=ON`.

## Dependencies

- **OpenSSL** — `rtc_random_bytes()` uses `RAND_bytes()`
- **pthread** (Unix) / **WinAPI** (Windows)
- Data structures: standard C library only (`stdlib.h`, `string.h`)
