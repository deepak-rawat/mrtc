# SDL3 desktop application

`mrtc_desktop` is the native desktop client. `conf_sdl` compiles the same
sources and retains the historical launch location:

- macOS: `build/app/conf_sdl/conf_sdl.app/Contents/MacOS/conf_sdl`
- Other platforms: `build/app/conf_sdl/conf_sdl`

The new executable is under `build/app/desktop` (inside `mrtc_desktop.app` on
macOS). Configure with `MRTC_BUILD_DESKTOP=ON`; SDL3 is discovered through
pkg-config. No font or widget dependency is required.

## Ownership

```text
main.c: SDL events, selected physical devices, external frame capture
   |
   +-- ui_sdl3.c: join form, named gallery, audio output, chat, status
   |
   +-- app/common/app_session: options, RTC lifecycle, conference/signaling,
                              synthetic video/tone generation and pacing,
                              thread-safe participant snapshots
           |
           +-- conference: P2P/SFU negotiation and media pipelines
```

All SDL operations, including texture uploads/destruction, camera/audio device
access and text input, run on the main thread. Internal conference callbacks
copy frames, audio, messages, topology and errors under `tiles_mutex`; they never
call SDL. Peer-left marks every source for removal. The next render releases
textures, buffers and map entries, preserving surviving tile slots. A peer
appears as a named placeholder before decoded video arrives. Snapshot names
come from the shared session, not from media heuristics.

Session destruction stops callbacks before UI mutexes, textures and devices are
freed. Leave stops the session and capture devices, clears participants/chat,
and returns to the join form. A subsequent join reuses the UI and session.

## UI and controls

Without arguments the application opens an editable server/room/name form.
Tab cycles fields; clicking selects a field. Text is append-only with
code-point-safe Backspace. Enter joins. Video and audio buttons independently
select camera/synthetic and microphone/tone. Synthetic video and tone are the
defaults; choosing a real device never falls back to generated media on failure.
The join screen previews the selected video source in wide/tall windows; compact
windows prioritize the form. Synthetic pre-join preview uses the shared session's
preview tick without starting RTC or publishing audio/video.
Camera permission denial, unsupported formats and audio failures produce
visible status messages.

The in-call header shows the room, server-authorized P2P/SFU mode and participant
count. The gallery reserves space for the bottom controls and optional chat
panel, letterboxes video without distorting its aspect ratio, displays named
placeholders, and includes a labeled You tile. Remote speaking borders use
recent decoded RMS audio only; silence is **not** interpreted as remote mute.
Only the local user's explicit mute/camera states are displayed.

Click Mute, Camera, Chat, Send or Leave. With no focused text field, M and V
toggle local audio/video, and C toggles chat. Chat opens with input focused;
Enter sends, Escape releases focus. Global shortcuts are suppressed while
typing, including while typing M/V/C in chat. Window close quits. There is no
single-key quit shortcut that can accidentally terminate text entry.

Chat stores at most 32 messages and 256 message bytes, preserving complete UTF-8
code points and ignoring invalid/binary suffixes. The panel shows the most
recent messages that fit; messages wrap to at most two lines, and long lines
and field values are ellipsized to available width. Scrolling, selection, rich
text and IME pre-edit display are not provided.

Labels use SDL's scaled debug-text ASCII atlas. **UTF-8 names and chat are
preserved for signaling and editing; each non-ASCII code point displays as one
`?`**, as noted in `--help`. Full Unicode glyph rendering would
require a separately chosen font/shaping dependency; none is silently added.

## Command line

```sh
build/app/desktop/mrtc_desktop.app/Contents/MacOS/mrtc_desktop \
  --meeting design --server ws://127.0.0.1:8080 --name Alice --stun ''
```

`--meeting` auto-joins for compatibility. Omit it for the join form.
`--camera`/`--synthetic` and `--mic`/`--tone` select sources.
`--duration SECONDS` exits cleanly after a bounded run (1–86400 seconds).
`--stats-file PATH` writes decoded remote audio/video totals and successful
SDL remote texture upload counts once a second and at exit, for diagnostics.
The default STUN URL is `stun:stun.l.google.com:19302`; `--stun ''` disables
STUN. Synthetic dimensions/frame rate and source labels come from the shared
session (320x240 at 15 fps, `camera`; 48 kHz mono `mic`). Camera requests the same
dimensions in I420 format.

## Focused validation

```sh
cmake --build build --target mrtc_desktop conf_sdl test_ui_sdl3 --parallel 4
build/app/desktop/test_ui_sdl3
MRTC_UI_SNAPSHOT=build/desktop-ui.bmp build/app/desktop/test_ui_sdl3
```

Tests use SDL dummy video/audio drivers and exercise deferred callback removal,
named placeholders, frame replacement without tile reordering, layout and
aspect-fit calculations, actual window resizing, join/source/control hit tests,
UTF-8 Backspace, text-safe shortcuts and bounded chat. The optional snapshot
environment variable exists only in the UI test binary and saves the real SDL
render output as BMP; the last test produces a named gallery/chat fixture.
The last join-form render is also saved to `<path>.join.bmp`.
Physical camera/microphone permission behavior still requires device testing.

The actual desktop bundle and stdio CLI can also be exercised together:

```sh
MRTC_BUILD_DIR=build node app/desktop/test_media_smoke.mjs
```

When Node 22+ is available, this is also registered as the
`test_desktop_media` CTest.

This reuses the interop server fixture and versioned CLI controller. It starts
an isolated localhost server and real synthetic desktop process using SDL dummy
video/audio drivers, verifies fresh decoded audio/video on both clients and
fresh main-thread SDL texture uploads after a baseline, then checks clean CLI
and timed desktop exit. STUN is explicitly disabled. Evidence is written under
`build/desktop-media-smoke`; all owned processes are stopped in `finally`.
