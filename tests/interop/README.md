# Real UI interoperability

Build `web/` first. Build the new backend, `sfu_probe`, and `chat` in a dedicated
native directory such as `build/reorg-web`. No signaling implementation lives
in the test fixture: Playwright drives the shipped UI served by the real
`app/server/mrtc_server` executable.

```sh
cd tests/interop
npm ci
npm run test:browser
npm test
npm run test:migration
npm run test:churn
```

`MRTC_BUILD_DIR` is relative to the repository root (absolute paths also work).
`MRTC_WEB_ROOT` optionally overrides the built app's root-relative/absolute path.
`MRTC_CHROME_PATH` optionally selects an existing Chromium executable.
Use `npm run install:browser` if the browser is missing. It installs headless
Chromium under this harness's ignored `node_modules/.cache`, not system-wide.
`PLAYWRIGHT_BROWSERS_PATH` overrides that location. Playwright remains pinned
to 1.65.1.

Fixtures reserve loopback ephemeral ports and terminate only their own child
processes. No existing application or server is reused. Tests use explicit
synthetic capture through the same Join control, not fake device fallbacks.
Chromium runs headless with default networking/privacy and autoplay policies,
and system audio muted. The real Join click unlocks media playback; decoded
audio counters still verify media reception. Native interoperability requires
that the synthetic browser actually emitted `.local` host candidates, so the
test cannot silently stop exercising mDNS privacy.

`test:browser` verifies three actual browser participants in each forced/auto
mode. `npm test` covers forced SFU/mesh, decoded browser/native media both directions,
SCTP messages, SFU direction renegotiation, live ICE restart, tile removal and
rejoin, and native CLI chat. `test:migration` covers native/browser auto 2→3→2,
per-source decode after each handoff, stable gallery nodes, capture lifetime,
single selected audio stream, coordinator timeout/abort with active-path
continuity, and replayed stale-epoch SDP. The browser-only suite additionally verifies a
non-default threshold with real media at 4→5→4 participants. Override that
boundary with `MRTC_P2P_MAX` (2..7 so an overflow participant fits the room);
the native/browser migration case deliberately remains 2→3→2.
`MRTC_DOWNGRADE_DELAY_MS` overrides its hold-down (default 250).
`test:churn` preserves 120 real native reconnects; override
`MRTC_CHURN_CYCLES` only for a local smoke run.
