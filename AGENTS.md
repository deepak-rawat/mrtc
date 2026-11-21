# Agent guide

Applies repository-wide. Keep changes focused, code understandable and runtime
behavior explicit. Reuse existing helpers; avoid speculative abstractions.

## Start here

- [README](README.md): dependencies, platform setup, build and run commands.
- [Architecture](docs/ARCHITECTURE.md): source map, ownership and packet flow.
- [Testing](docs/TESTING.md): native, web, E2E, sanitizer and fuzz commands.
- [Protocol](docs/PROTOCOL.md), [standards](docs/STANDARDS.md),
  [performance](docs/PERFORMANCE.md), [roadmap](docs/ROADMAP.md): relevant contracts.

Run commands from the repository root. Use the README's platform-specific
configuration/dependency commands (MSYS2 UCRT64 on Windows). Build with one job
per available logical CPU, regardless of the configured generator:

- macOS/Linux: `cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN)"`
- Windows (MSYS2 UCRT64): `cmake --build build --parallel "$(nproc)"`

The architecture guide contains the layer diagram, dependency graph and
component source map; the consolidated guides replace old per-component docs.

## Code conventions

- Runtime/client APIs use `rtc_<module>_`; retain existing `media_`,
  `conference_` and `signaling_` prefixes elsewhere. Examples: `rtc_stun_`,
  `rtc_ice_`, `rtc_peer_`.
- Protocol constants use `#define` with the module prefix, such as
  `STUN_MAGIC_COOKIE` and `STUN_HEADER_SIZE`.
- Config structs use `rtc_xxx_config_t` and are passed to init/create; state
  structs use `_t`, such as `rtc_ice_agent_t` and `rtc_dtls_transport_t`.
  Follow existing callback `_fn` typedefs with a user-data pointer.
- Use pointer output parameters; method-like functions take the object pointer
  first. Follow the `rtc_err_t` convention: `RTC_OK` (0) or a negative error.
  Preserve existing `int`/`rtc_err_t` signatures rather than changing unrelated
  APIs. Codes in [rtc_common.h](common/include/rtc/rtc_common.h) range from
  `RTC_ERR_GENERIC` (-1) through `RTC_ERR_SDP` (-9).
- Heap runtime/public objects (worker, listener, router, transport, producer,
  consumer, peer connection): create (NULL on failure) -> close (stop activity)
  -> destroy (free). Caller-allocated protocol primitives (ICE, DTLS, SRTP, RTP,
  TURN, poller): init -> close.
- Match global setup/cleanup to the owning library: `rtc_init()` /
  `rtc_cleanup()` for core consumers; `rtc_client_init()` /
  `rtc_client_cleanup()` for client consumers.
- Use `RTC_LOG_ERR`, `RTC_LOG_WARN`, `RTC_LOG_INFO` and `RTC_LOG_DBG` from
  [rtc_common.h](common/include/rtc/rtc_common.h); report failures, never hide
  them. Pass a literal format string, for example `RTC_LOG_ERR("Error: %s", msg)`.
- Preserve worker ownership, callback lifetimes and SRTP replay/nonce state.
  Client and routing remain siblings over the shared runtime.
- Use OpenSSL EVP APIs (3.0+), not deprecated `HMAC()` or low-level cipher APIs.
- Keep comments minimal: explain non-obvious reasons, protocol/RFC references
  or subtle edge cases, not what the code already expresses. Prefer clear names
  and straightforward control flow over explanatory comments.

### Platform abstractions

Use helpers from [common](common/include/rtc/rtc_common.h) and RTC rather than
platform-specific APIs outside platform implementations:

| Abstraction | Windows | Unix |
|---|---|---|
| `rtc_socket_t` | `SOCKET` | `int` |
| `rtc_thread_t` | `HANDLE` | `pthread_t` |
| `rtc_mutex_t` | `CRITICAL_SECTION` | `pthread_mutex_t` |
| `rtc_cond_t` | `CONDITION_VARIABLE` | `pthread_cond_t` |

- Use `RTC_INVALID_SOCKET`, not a literal -1 or `INVALID_SOCKET`.
- Socket/time helpers: `rtc_close_socket()`, `rtc_set_nonblocking()`,
  `rtc_time_ms()`.
- Threading helpers: `rtc_thread_create()`, `rtc_thread_join()`,
  `rtc_mutex_*()`, `rtc_cond_*()`.
- I/O poller: `rtc_poller_t` (epoll on Linux, kqueue on macOS, select on Windows).

## Format and validate

Format only changed C/C++ files, from the repository root, before staging:
```sh
clang-format --style=file -i path/to/changed.c path/to/changed.h
```
Then build with the platform's maximum-CPU command above.
Prefer clang-format on PATH; otherwise locate the C/C++ extension with
`code --locate-extension ms-vscode.cpptools` and use its `LLVM/bin` executable.
Use `clang-format.exe` on Windows and the quoted full executable path when
needed; discover the installed extension path rather than hard-coding a version.
This applies to modified `*.c`, `*.h`, `*.cc`, `*.cpp` and `*.hpp` files.
If neither executable exists, report it and request installation rather than
using editor commands or Computer Use.

- Run the smallest relevant tests after formatting and before staging.
  Add focused regressions for changed behavior; do not weaken existing checks.
- Native tests use [test_harness.h](tests/test_harness.h) with `TEST()`,
  `RUN_TEST()` and `ASSERT*` macros, not an external framework. Each suite is a
  standalone executable.
- Web tests use TypeScript checking and Node contract tests. Playwright in
  [tests/e2e](tests/e2e/) drives the actual built UI served by `mrtc_server`;
  never substitute a separate signaling implementation to make tests pass.
- Preserve default browser privacy/autoplay and decoded-media assertions.
  Cover actual default native configuration, controlled synthetic capture and
  topology handoffs.
- Documentation-only changes need link/command checks, not native rebuilds.
  Distinguish local results, configured CI checks and external qualification.

### Compiler warnings

Treat every compiler warning as a bug and resolve it in the change that
surfaced it. Never disable the warning flag or reformat a warning away.

| Warning | Preferred fix |
|---|---|
| `-Wunused-variable`, `-Wunused-but-set-variable` | Remove dead variables; move declarations used only in conditional compilation inside the matching `#ifdef` |
| `-Wunused-parameter` | For fixed-signature callbacks, add `(void)param;` at the top of the body; do not rename or remove the parameter |
| `-Wsign-compare` | Cast the smaller/narrower side to the wider unsigned type at the comparison, e.g. `(size_t)header_size + payload_len > len`; do not change unrelated API types |
| `-Wunused-function` | Delete the function; recover it from git history if needed later rather than keeping speculative code |
| `-Wformat`, `-Wformat-security` | Fix the format string; never use a non-literal as a printf-family format argument |
| `-Wimplicit-fallthrough` | Add `break;` or an intentional `/* fall through */` recognized by the compiler |
| `-Wmaybe-uninitialized` | Find and fix the path leaving the value unset; initialize appropriately, not blindly with `= 0` |

After fixing warnings, rerun the maximum-CPU build command above and confirm
clean, unfiltered compiler output and a successful exit status before reporting
completion. Do not use filters that hide failures or success-shaped fallbacks.

## Keep documentation current and short

- Update relevant docs with changes to behavior, APIs, commands or defaults.
  Update standards/roadmap claims only when supported by code or evidence.
- Keep user docs to the root README and six existing flat guides in `docs/`;
  target no more than 900 combined lines. Add no component READMEs or extra
  guides unless requested.
- Replace stale text instead of appending. Remove duplication; link to the
  authoritative section. Preserve essential commands, contracts and caveats.
- No implementation diaries, historical test logs or exhaustive API inventories.
  This file holds agent rules; user guides hold usage/design details.

## Commit messages

Keep messages short and to the point:

- Subject: lowercase `<scope>: <concise summary>`, no trailing period, ideally
  <=60 characters. Scope can be a module (`rtc`, `media`, `signaling`) or subarea
  (`rtc/peer`, `rtc/ice`).
- Separate subject and body with a blank line. Use free-form paragraphs
  explaining why and the user-visible effect, not a narration of the diff.
  Wrap body lines near 72 columns; separate paragraphs with one blank line.
- Prefer a single `-F -` heredoc so paragraphs and wrapping are preserved:

  ```sh
  git commit -F - <<'EOF'
  <scope>: <subject>

  <body paragraph wrapped at ~72 characters>
  EOF
  ```

- Do not use one `-m` per body line: each creates a separate paragraph. If using
  `-m`, use exactly two flags, one for the subject and one for the entire body,
  preserving internal line breaks.

## Explaining code

Default to a worked-example / "life-of-a-packet" walkthrough, not an abstract
description:

- Lead with the one idea, using an analogy when helpful.
- Trace one concrete packet, request or call end-to-end, from entry to exit.
- Use concrete IPs, ports, ufrags, SSRCs and hex IDs, not `X`/`foo` placeholders.
- Show each function call in sequence and its actual arguments when useful.
- Show before/after state changes: struct fields, route maps, buffers and flags.
- Annotate the thread at each step (app thread versus worker loop).
- Include a Mermaid sequence/flow diagram and a cause/effect summary table.
- Explain design rationale and trade-offs as you go: why a lock, route key or
  marshalled operation exists; be honest about costs and edge cases.
- Distinguish public API calls made by applications from internal calls made
  by library code on their behalf.

Keep theory secondary to the concrete trace. Scale depth to the request; for
"more detail," add argument-level values, before/after state, byte-level wire
formats, error paths and multiple concurrent actors.
