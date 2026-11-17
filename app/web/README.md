# Browser client

The browser uses built-in WebRTC (VP8, Opus and SCTP), not native/WASM codecs.
Capture starts only after **Join**. The explicitly selected synthetic option
uses a changing canvas and a Web Audio oscillator; it never replaces failed
camera/microphone permissions silently.

```sh
cd app/web
npm ci
npm run typecheck
npm run build
```

Serve `app/web/dist` with `build/app/server/mrtc_server --web-root app/web/dist`.
Open its HTTP URL on localhost. The default signaling endpoint is the same
origin's `/ws` (WS for HTTP, WSS for HTTPS). For LAN/non-localhost use HTTPS
and WSS: browser capture requires a secure context. An explicit Server URL
supports a separately hosted development page (`npm run dev`).

The server chooses mesh/P2P, SFU, or automatic policy. The UI displays the
concrete active topology and switching/errors; it never independently chooses
a topology. One publishing PC plus one subscription PC per other SFU source is
intentional. Pending video is decoded in muted sinks, then stable participant
tiles select the committed streams. Capture tracks are owned by the call, not
by any one PC. **Leave** stops capture.

Readiness requires connected data transports and decoded pending media for
sources flowing on the active path in the last 1.5 seconds. Received new
subscriptions are also checked. Data-only participants need no subscription;
the protocol has no per-source media declaration, so an as-yet-unannounced
new source cannot be included in that readiness proof. Interop tests therefore
also require every expected source to decode after commit.

`window.mrtc.stats()` provides bounded diagnostic snapshots. `renegotiate()`
exposes publisher direction/restart testing; normal join/leave/chat tests use
actual UI controls.
