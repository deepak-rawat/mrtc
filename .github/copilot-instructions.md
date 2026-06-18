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
- `rtc/ARCHITECTURE.md` — core RTC runtime + protocol primitives
- `client/ARCHITECTURE.md` — WebRTC-style peer connection facade
- `media/ARCHITECTURE.md` — codecs, jitter buffer, media pipeline
- `signaling/ARCHITECTURE.md` — WebSocket signaling client + server
- `conference/ARCHITECTURE.md` — multi-peer conferencing
- `app/conf_sdl/ARCHITECTURE.md` — SDL3 video conference application

## Key Conventions

### Comments
Keep comments minimal. Add them only where they are really needed — to explain
*why* something non-obvious is done, document protocol/RFC references, or warn
about subtle edge cases. Do **not** write comments that merely restate what the
code already expresses; the code should speak for itself. Prefer clear names and
straightforward control flow over explanatory comments.

### API Patterns
- All public functions use `rtc_` prefix followed by module name (e.g., `rtc_stun_`, `rtc_ice_`, `rtc_peer_`)
- Functions return `rtc_err_t` (0 = `RTC_OK`, negative = error)
- Lifecycle depends on ownership:
  - Heap-allocated runtime/public objects (worker, listener, router, transport,
    producer, consumer, peer connection) use a three-call lifecycle:
    `rtc_xxx_create()` (returns pointer, `NULL` on error) → `rtc_xxx_close()`
    (stop activity) → `rtc_xxx_destroy()` (free; call after `close()`)
  - Caller-allocated protocol primitives (ICE, DTLS, SRTP, RTP, TURN, poller)
    use `rtc_xxx_init(rtc_xxx_t *)` / `rtc_xxx_close()`
  - Global library setup: `rtc_init()` / `rtc_client_init()`
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
- Config structs: `rtc_xxx_config_t` passed to `init`/`create` functions
- State structs: `rtc_xxx_t` (e.g., `rtc_ice_agent_t`, `rtc_dtls_transport_t`)
- Callbacks: `rtc_on_xxx_fn` typedefs with user data pointer

### OpenSSL Usage
Uses modern EVP APIs (3.0+). Avoid deprecated functions like `HMAC()` or low-level cipher APIs.

### Testing
Tests use a minimal harness (`tests/test_harness.h`) with `TEST()`, `RUN_TEST()`, `ASSERT*` macros. No external test framework. Each test is a standalone executable.

## Commit Message Style

Keep commit messages short and to the point. Avoid verbose explanations.

Structure:

- **Subject** (first line): `<scope>: <concise summary>` in lowercase,
  no trailing period, ideally <= 60 chars. Scope can be a module
  (`rtc`, `media`, `signaling`) or `module/subarea` (`rtc/peer`,
  `rtc/ice`).
- **Blank line** between subject and body.
- **Body**: free-form paragraphs explaining *why* and the user-visible
  effect (not a diff narration). Wrap lines at ~72 chars. Multiple
  paragraphs are fine, separated by single blank lines.

How to write it with `git commit`:

- Prefer a single `-F -` heredoc so blank lines and wrapping survive
  exactly as written:

  ```bash
  git commit -F - <<'EOF'
  <scope>: <subject>

  <body paragraph wrapped at ~72 chars>
  EOF
  ```

- Do **not** use one `-m` flag per body line — each `-m` becomes its
  own paragraph and inserts a blank line between them. If you must
  use `-m`, pass exactly two: one for the subject and one for the
  whole body (with `\n` for internal line breaks).

## Build Warnings Policy

**Treat all compiler warnings as bugs to fix, not noise to ignore.** When a
build emits warnings, resolve them in the same change that surfaced them.
Never silence a warning by disabling the flag or by reformatting the message
away — fix the underlying issue.

Common warnings and the preferred fix:

- **`-Wunused-variable` / `-Wunused-but-set-variable`**: Remove the variable
  if it is truly dead. If it is only used inside a conditional compilation
  block (e.g. `#ifndef _WIN32`), move its declaration inside the same
  `#ifdef` so it does not exist on platforms that do not use it.
- **`-Wunused-parameter`**: For callbacks with a fixed signature (e.g.
  libwebsockets `lws_callback_function`, pthread thread functions), add
  `(void)param;` at the top of the function body. Do not rename the
  parameter or remove it from the signature.
- **`-Wsign-compare`**: Cast the smaller/narrower side to the wider unsigned
  type at the comparison site (e.g. `(size_t)RTC_DC_HEADER_SIZE + payload_len > len`).
  Do not change the underlying types of unrelated APIs just to silence one
  comparison.
- **`-Wunused-function`**: Delete the function. If it is intentionally kept
  for future use, that justification belongs in a commit message, not in
  the tree — remove it and recover from git history if needed later.
- **`-Wformat` / `-Wformat-security`**: Fix the format string. Never pass a
  non-literal as the format argument to `printf`-family functions.
- **`-Wimplicit-fallthrough`**: Add an explicit `break;` or a `/* fall
  through */` comment recognized by the compiler.
- **`-Wmaybe-uninitialized`**: Initialize the variable at declaration. Do
  not add `= 0` blindly — first check whether a control-flow path really
  can leave it unset, and if so fix that path.

After fixing warnings, re-run the build and confirm the output is clean
before reporting the task done. A clean build is:

```bash
cd build && ninja 2>&1 | grep -E "warning|error" || echo "Clean build"
```

If that prints `Clean build`, the tree is warning-free.

## Formatting (no external clang-format required)

`clang-format` CLI is **not** installed. The repo's `.clang-format` is applied
by the VS Code C/C++ extension (`ms-vscode.cpptools`), which bundles its own
clang-format.

After editing any C/C++ file (`*.c`, `*.h`, `*.cc`, `*.cpp`, `*.hpp`), before
staging it for commit:

1. Open the file in the editor (run VS Code command `vscode.open` with the file URI).
2. Run the VS Code command `editor.action.formatDocument`.
3. Run the VS Code command `workbench.action.files.save` to persist.

Do this for every C/C++ file you modified in the change before `git add`. Do
not invoke `clang-format` from the terminal — it is not available.

## Explaining Code

When asked to explain a component, subsystem, or flow, default to a
**worked-example / "life-of-a-packet" walkthrough** rather than an abstract
description. Specifically:

- **Lead with the one idea** (and an analogy where it helps) before any detail.
- **Trace one concrete case end-to-end** — follow a single packet / request /
  call through the code from entry to exit.
- **Use concrete example values** (real-looking IPs, ports, ufrags, ssrcs,
  hex ids), not placeholders like `X` / `foo`.
- **Show each function call in sequence** and, where useful, its actual
  arguments.
- **Show how state changes after each step** (struct fields, the route maps,
  buffers, flags) so the reader sees cause and effect.
- **Annotate which thread** each step runs on (worker loop vs app thread).
- Include a **Mermaid sequence/flow diagram** and a **cause → effect summary
  table**.
- Call out **design rationale and trade-offs** as you go (why this lock, why
  this key, why marshalled), and be honest about costs and edge cases.
- Distinguish **public API** (what an app author calls) from **internal API**
  (what other library code calls on their behalf).

Keep abstract/theoretical descriptions secondary — the concrete trace is the
backbone of the explanation. Scale the depth to the request; when asked for
"more detail," add argument-level values, before/after state, byte-level wire
format, error paths, and multiple concurrent actors.

