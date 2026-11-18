# Real UI interoperability

Build `app/web/` first. Build `app/server/mrtc_server` and `app/cli/mrtc_cli` in a
native directory such as `build/reorg-web`. No signaling implementation lives
in the test fixture: Playwright drives the shipped UI served by the real
`app/server/mrtc_server` executable.

```sh
cd tests/interop
npm ci
npm run test:controller
npm run test:native
npm run test:browser
npm test
npm run test:migration
npm run test:churn
# All of the above:
npm run test:all
```

`MRTC_BUILD_DIR` is relative to the repository root (absolute paths also work).
`MRTC_WEB_ROOT` optionally overrides the built app's root-relative/absolute path.
`MRTC_CHROME_PATH` optionally selects an existing Chromium executable.
Use `npm run install:browser` if the browser is missing. It installs headless
Chromium under this harness's ignored `node_modules/.cache`, not system-wide.
`PLAYWRIGHT_BROWSERS_PATH` overrides that location. Playwright remains pinned
to 1.65.1.

`MRTC_BROWSER=chromium|firefox|webkit` selects the engine (default Chromium).
Install the selected runtime with `npx playwright install firefox`, for example.
Only Chromium accepts `MRTC_CHROME_PATH`. Chromium and Firefox have separate CI
jobs; configured jobs are not qualification results. WebKit runs are useful
additional checks but do not certify actual Safari. All engines retain their
normal privacy/autoplay behavior; no fake-media permission bypass is enabled.

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

## Headless native controller

`test:native` and `test:churn` use only Node built-ins: they need no browser,
SDL, npm installation, display or capture devices. They launch the shipped
`mrtc_cli --control stdio` against the real server, using synthetic sources
through its shared application session. Windows executable paths receive
the `.exe` suffix automatically. Native-only fixtures omit `--web-root` and
accept an HTTP readiness response regardless of status, so no `app/web/dist`
is needed and `MRTC_WEB_ROOT` is ignored for these suites.

`test:native` exercises forced mesh/SFU and automatic 2→3→2 and custom 4→5→4.
Every active topology epoch must produce fresh decoded video-frame and
audio-sample **deltas for every remote source**, not merely packet counters.
It checks exact-once chat, roster removal, generation-incrementing same-process
rejoin, independent audio/video mute after drain, receive-only participants,
and two participants sending no media while exchanging SCTP messages.
`none` disables sending that source; it does not disable remote subscriptions.
Churn likewise leaves and rejoins the same CLI process 120 times, requiring
fresh bidirectional decode and bidirectional SCTP on every cycle. Mesh
receive-only coverage observes and verifies both real peer-ID offerer orders.

`native-client.mjs` speaks version-1 JSONL. Requests contain `version: 1`,
unique `id`, and `command`; commands are `join` (`server`, `room`, `name`,
`video`/`audio`: `synthetic` or `none`, optional `stun` and `data_only`), `leave`,
`send_message` (`text`, optional `peer_id`), `mute` (`source`, `muted`),
`stats`, and `quit`. Results contain `event: "result"`, `id`, `ok` and
`stats` for a snapshot. Async `joined`, `peer_joined`, `peer_left`, `message`,
`topology`, and `error` events carry `generation` and `epoch`.
Joined/message events identify `peer_id`; messages include `text`.
Snapshots expose `peer_id`, `generation`, `epoch`, `mode`, `status`, and
`peers` with `peer_id`, `name`, `video_frames`, `audio_samples`, and `epoch`.
Counters reset when a new epoch becomes active. Browser chat receives an
explicit test-controller reply; the application contains no probe auto-echo.

Commands have 10-second deadlines, event/media/topology waits at most
45 seconds, and graceful quit at most 5 seconds after its response.
Unexpected EOF, process exit, malformed JSON/protocol events and async errors
immediately reject pending commands and event waits. Frames are capped at
1 MiB, outstanding commands at 128, event history at 2,048 entries, and logs at
200 KB per stream/name.
Only the deliberate migration-abort scenario permits the exact browser
`topology preparation timed out` abort report and requires the matching native
topology event; native async errors and unrelated browser errors still fail.
Churn also has a 360-second total
deadline, overridable with `MRTC_CHURN_TIMEOUT_MS`.

Failures persist bounded command/event output, stderr, the exception and final
native/browser snapshots beneath the repository's `build/interop-failures`.
`MRTC_TEST_ARTIFACTS` overrides that parent directory. Snapshots can report an
unavailable/exited session rather than concealing a process failure.
`test:controller` uses minimal fake child processes only to test JSONL
framing, correlation, deadlines, exits, bounded history and error handling;
it never substitutes fake signaling/media for the real E2E suites.
