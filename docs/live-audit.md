# Live ASIO-to-UI path audit

Audit scope: the Windows ASIO live-capture route through the WebView2 UI, as
implemented in this checkout. This is a source audit, not verification against
an ASIO device or browser paint timing.

## Path summary

| Hop | Implemented and wired to the next hop? | Tests | Agreement with `docs/live.md` |
| --- | --- | --- | --- |
| ASIO capture and RtAudio callback | Yes. `capture_asio_live()` validates and declares the input origin, then calls `capture_asio_stream()`. The latter opens the selected device and installs `live_capture_callback()`, which timestamps and delegates each callback to `LiveCaptureTransport::receive()`. The older `capture_asio()` uses a distinct callback/transport and is not the WebView live-session route. | `tests/asio_tests.cpp` exercises the extracted transport and frame/gap accounting; it does not open RtAudio or exercise a physical ASIO callback/device. | Matches the documented bounded callback, explicit gaps, overflow reporting and reconnect behavior. Hardware/driver behavior remains unverified as the doc states. |
| `AudioInputSink` | Yes. The GUI capture route passes a `LiveTranscriptionSession` to ASIO. `LiveTranscriptionSession::push()` copies accepted blocks into preallocated queue slots; `IncrementalEngine::push()` delegates to the same session API for engine consumers, but is not the GUI route. | `tests/live_tests.cpp` covers live admission and worker behavior. `tests/engine_tests.cpp` covers the incremental engine and stereo ASIO-compatible sink/restart identity. | Matches the format, sequencing, bounded-input and one-take contracts. |
| Ingest, analysis, poll and resync | Yes. `LiveTranscriptionSession::Impl::ingest_loop()` drains blocks and records gaps; `analysis_loop()` runs ready analysis. The GUI owner loop in `perform_live_capture()` calls `poll_events()`, applies batches to `TranscriptionDocument`, and calls `resync()` with the paired snapshot when overflow requests it. | `tests/live_tests.cpp` covers gaps, skip-ahead, event-queue overflow/resync, document revision behavior, cancellation and threaded bounds. | Matches the worker split, event queue, paired snapshot/resync and reported-loss semantics. |
| C++/WebView2 bridge | Yes. `post_live_session()` coalesces pending native-window updates and posts `message_live_session`; the window handler serializes the latest note snapshot, events and metrics with `live_notes_json()` / `live_events_json()`, then `post_json()` calls `PostWebMessageAsJson()`. In the reverse direction, `WebMessageReceived::Invoke()` checks the local app origin and `handle_web_message()` enforces the 23-operation allowlist. `live-edit` batches are bounded to 256 commands; pending native live events are capped at 4,096. | `tests/ui_smoke.py` checks static bridge/UI wiring and unknown-command rejection. `tests/ui_workflow.js` via `ui_workflow_browser` covers browser-protocol live event coalescing and status behavior. Neither exercises the actual native `PostWebMessageAsJson()` failure or native pending-event cap. | Generally matches the documented native-dispatch metrics and event/resync handling. The doc does not detail the bridge's event batching/cap or its WebView post failure behavior. |
| JS live display | Yes for the editable canvas views and status UI. `app.js` receives `type: "live"`, coalesces events to an animation frame, calls `renderNotes()` with the latest complete snapshot, dispatches `transcribe:live-events`, and updates the recent-event panel, diagnostics and status bar. `editor.js` accepts `transcribe:notes` and redraws the staff, tablature and note timeline; audio-preview updates drive the waveform/spectrogram timeline. The separate Verovio renderer in `notation.js` listens for `transcribe:accepted-score`, so formal engraved MusicXML pages update after score acceptance, not for each live revision. | `tests/ui_workflow.js` / `ui_workflow_browser` exercises live-note revisions, event delivery/coalescing, finalized styling, timer, pause/resume and status; `ui_asset_smoke` checks required UI wiring and JavaScript syntax. The workflow test also checks that a live message after an accepted score cannot overwrite that score, but does not test a *new active ASIO take* while an older score exists. | Matches the live editable score/event display described in `docs/live.md`; the distinction between live canvas notation and accepted-score Verovio engraving is not explicit there. |

## Thread rules and loss/order audit

* No application-level allocation, mutex acquisition, logging, inference or UI
  notification was found in either RtAudio callback implementation. The live
  callback only obtains a monotonic timestamp and delegates to
  `LiveCaptureTransport::receive()`; the in-tree `LiveTranscriptionSession`
  sink admits/copies into preallocated slots with lock-free atomics. The legacy
  `capture_callback()` writes to its fixed-capacity rings. The source also
  statically checks the required atomic types for lock freedom.
* `capture_asio_stream()` is public and accepts any `AudioInputSink`; its
  callback calls that sink's virtual `push()`. The actual GUI uses the
  real-time-safe session implementation, but the interface cannot enforce that
  an external sink implementation itself avoids locks, allocation or blocking.
* Expected loss points are explicit: full input queues refuse/drop a block and
  advance the ASIO frame timeline; null input and device/driver discontinuities
  become gaps; SkipAhead can intentionally skip analysis; and a full live event
  queue discards older events and requests an atomic snapshot resync. These
  paths update metrics or emit status/gap events. During resync the GUI applies
  the document snapshot and sends the current complete note set to JS rather
  than relying on the truncated event history.
* `post_live_session()` can also discard the oldest pending GUI event details
  above its 4,096-event cap. It marks the outgoing metrics overloaded and
  increments `events_dropped`; its accompanying latest note snapshot preserves
  current note state. The browser keeps only the most recent eight events in
  the visible event panel by design.
* If `PostMessageW()` fails, `post_live_session()` clears the pending batch and
  throws, causing the capture operation to fail. By contrast, a failed
  `PostWebMessageAsJson()` in `post_json()` only calls `OutputDebugStringW()`;
  the WebView update is lost without an in-UI error or retry. This is the
  unreported live-display loss point.
* Event order is maintained through the single session consumer, the native
  pending-event append, and JS's append-before-animation-frame merge. Events
  carry sequence and revision values in JSON. The UI currently renders the
  latest full note snapshot instead of reducing events, so the bounded event
  history does not itself corrupt the displayed note state. No duplicate or
  out-of-order state application was found in the normal path.
* A prior accepted score is preserved when a new transcription starts, and
  `app.js` drops every live message while `hasAcceptedScore` is true. Thus a
  second ASIO take is captured and processed natively, but its live notes,
  events and diagnostics are not shown until completion/proposal handling.
  This conflicts with the unqualified live-display description and is a
  concrete UI coverage gap.

## Ranked defects and missing pieces

1. **High — live preview silently suppressed after an accepted score.** The
   `type: "live"` handler returns early when `hasAcceptedScore` is true, while
   transcription startup deliberately preserves an existing score. A
   subsequent ASIO take consequently has no live note/event/status updates in
   the UI. Change `ui/app.js` to separate the active live-take preview from the
   accepted score (or explicitly define and document this behavior).
2. **Medium — failed WebView IPC silently loses live updates.** `post_json()`
   logs a failed `PostWebMessageAsJson()` only to the debugger and does not
   retry, surface an error, or mark the live stream as failed. Change
   `src/gui_webview.cpp` to make the failure visible to the owner/UI or fail the
   active capture explicitly.
3. **Medium — native WebView live-delivery behavior lacks integration
   coverage.** Browser workflow tests cover JS coalescing and live display,
   while `ui_smoke.py` checks static bridge wiring and unknown commands. There
   is no test for native `post_live_session()` event capping or the
   `PostWebMessageAsJson()` failure/drop path. Add focused coverage in
   `tests/ui_workflow_browser.py` and/or `tests/workstation_host.py` when
   changing `src/gui_webview.cpp`.
4. **Low — callback-safety is not enforceable for arbitrary sinks.**
   `capture_asio_stream()` exposes an `AudioInputSink&` invoked directly from
   the RtAudio callback; only the in-tree session/engine implementations have
   the required behavior established here. Clarify or constrain the
   real-time-safe sink contract in `include/transcribe/asio_capture.hpp` and
   `include/transcribe/live.hpp`.
5. **Low — documentation does not distinguish the live staff canvas from
   final Verovio engraving or explain bridge coalescing.** Clarify the UI
   surfaces and bounded bridge event-history semantics in `docs/live.md`.

## Requested build and test

The first configure attempt used CMake's default `NMake Makefiles` generator
and failed because `nmake` is unavailable. Reconfiguring with the installed
MinGW/Clang toolchain succeeded with
`TRANSCRIBE_ENABLE_ASIO=ON`, `TRANSCRIBE_ENABLE_WEBVIEW2=ON`, and
`TRANSCRIBE_BUILD_TESTS=ON`. The full build completed successfully. It emitted
test-source warnings for Catch2's `__COUNTER__` extension and missing
initializers in `tests/inference_tests.cpp`.

CTest ran 219 tests: 217 passed and two failed; both failures reproduced with
`ctest --rerun-failed`:

* `guitar tablature assigns simultaneous notes to different strings`
  (`tests/transcribe_tests.cpp:703`) failed because the emitted tab text did
  not contain `"confidence"`.
* `dependency_manifest_smoke` (`tests/dependency_audit.py:23`) expects
  `project(transcriber VERSION 0.1.5`, while the current `CMakeLists.txt`
  declares version 0.1.6.

Live session/resync coverage and the `ui_workflow_browser` and `ui_asset_smoke`
tests passed. Neither failing test exercises the audited live ASIO-to-UI path.
