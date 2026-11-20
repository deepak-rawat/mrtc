# Agent guide

Applies repository-wide. Keep changes focused, code understandable and runtime
behavior explicit. Reuse existing helpers; avoid speculative abstractions.

## Start here

- [README](README.md): dependencies, platform setup, build and run commands.
- [Architecture](docs/ARCHITECTURE.md): source map, ownership and packet flow.
- [Testing](docs/TESTING.md): native, web, E2E, sanitizer and fuzz commands.
- [Protocol](docs/PROTOCOL.md), [standards](docs/STANDARDS.md),
  [performance](docs/PERFORMANCE.md), [roadmap](docs/ROADMAP.md): relevant contracts.

## Code conventions

- Runtime/client APIs use `rtc_<module>_`; retain existing `media_`,
  `conference_` and `signaling_` prefixes elsewhere.
- Follow existing config/state `_t` types, module-prefixed constants and
  callback typedefs with user data. Use pointer output parameters.
- Return `RTC_OK` or negative error codes; pointer-returning creates use NULL.
  Use `RTC_LOG_ERR/WARN/INFO/DBG`; report failures, never hide them.
- Heap runtime objects: create -> close -> destroy. Caller-allocated protocol
  primitives: init -> close. Match global init/cleanup to the owning library.
- Use socket/thread/mutex/condition/poller abstractions and
  `RTC_INVALID_SOCKET` from [common](common/include/rtc/rtc_common.h) and RTC,
  not platform-specific APIs outside platform implementations.
- Preserve worker ownership, callback lifetimes and SRTP replay/nonce state.
  Client and routing remain siblings over the shared runtime.
- Use OpenSSL EVP APIs (3.0+), not deprecated HMAC/low-level cipher APIs.
- Comment only non-obvious rationale, protocol references or subtle hazards.

## Format and validate

Format only changed C/C++ files, from the repository root, before staging:
```sh
clang-format --style=file -i path/to/changed.c path/to/changed.h
cmake --build build --parallel 4
```
Prefer clang-format on PATH; otherwise locate the C/C++ extension with
`code --locate-extension ms-vscode.cpptools` and use its `LLVM/bin` executable.
If neither exists, request installation rather than using editor automation.

- Treat compiler warnings as bugs: fix their cause, never disable warnings.
  Rebuild and inspect unfiltered output and exit status.
- Run the smallest relevant tests after formatting and before staging.
  Add focused regressions for changed behavior; do not weaken existing checks.
- Native tests use [test_harness.h](tests/test_harness.h), not an external framework.
  Web tests use TypeScript/Node; E2E uses the real server, CLI and shipped UI.
- Preserve default browser privacy/autoplay and decoded-media assertions.
  Cover default native configuration as well as controlled synthetic inputs.
- Documentation-only changes need link/command checks, not native rebuilds.
  Distinguish local results, configured CI checks and external qualification.

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

## Commits and explanations

- Commit subject: lowercase `<scope>: <summary>`, no trailing period, ideally
  <=60 characters. Separate the body with a blank line; explain why and the
  user-visible effect, wrapping near 72 columns. Prefer `git commit -F -`.
- Explain flows through a concrete packet/call, with example values, function
  sequence, state changes and thread ownership. Distinguish public/internal
  APIs; include a Mermaid diagram and cause/effect table when explaining a
  subsystem. Scale detail to the request and call out trade-offs.
