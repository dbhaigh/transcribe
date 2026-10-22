# Editing, time and playback

## Ownership and GUI scope

`ScoreDocument` is the accepted, serializable score; its identity and revision are checked
before native edit transactions are applied. The WebView editor holds one
pending edit projection, shared with the note list/piano roll through events.
Staff and tablature use the same note IDs and linked positions. The optional
Verovio engraving represents the accepted revision, not an independently
editable score: pending edits mark it as a draft, disable its playback overlay,
and invalidate outstanding engraving requests.

This checkout builds `src/gui_webview.cpp` and its native transcription
backend. There is no separate legacy native-widget GUI target/source in the
current CMake configuration. Shared C++ score/time/history fixes apply to
native clients; Windows playback transport is implemented in the WebView host.
No claim is made about a legacy GUI binary from a different checkout.

## Authoritative timeline

The origin is **recording-relative seconds**, starting at source frame zero.
Sample positions count **frames**, not interleaved channel samples:

* `recording_sample_seconds(frame, rate)` is `frame / rate`.
* `recording_sample_position(seconds, rate)` rounds to the nearest frame,
  with half-frame ties rounded upward. An unknown/zero rate, negative or
  nonfinite time, and counter overflow are explicit errors.
* Source note-event sample positions use the document's `source_sample_rate`;
  source-event and measured score-note boundaries are immutable evidence,
  not values to overwrite when quantizing or dragging notation.
* Score onset/duration are in **quarter-note units**, independent of the meter
  denominator. A quarter unit is not necessarily a conducting beat.
* `score_position_seconds` and `score_position_quarters` map through the tempo
  map's segment anchors, extrapolating the final segment when needed. Validated
  maps begin at second/quarter zero and have strictly increasing, continuous
  anchors, making the two conversions inverse. The existing effective tempo
  bounds of 1-1000 BPM are shared by the JavaScript projection.
* Meter/pickup convert quarter positions into measures. Tablature refers to
  notation note IDs; it has no separate timing clock.

The native host publishes source rate and full tempo-map anchors. The shared
`ui/timeline.js` projection is used for measure navigation and canvas
coordinates, rather than assuming `seconds * initial BPM`.
Tempo edits preserve note quarter positions and project new seconds; the
native transaction converts those seconds with the **edited** tempo map.
Undo restores both the context and its note projection.

Waveform/spectrogram columns retain their individual recording-relative start
and end times. Rolling previews retain at most 512 columns; discarded history
and delivery gaps are not stretched into continuous audio. Incremental UI
messages are no longer overwritten by a last-message-only animation queue.
The native preview mailbox may still coalesce chunks under load; a gap remains
a gap. Staff, tab, waveform, spectrogram, confidence and piano roll share the
same zero-based extent and horizontal margins. Different panel widths use
the same normalized mapping, not identical pixel coordinates.

## Editing and history

Native `ScoreEditor` validates a candidate document before committing. Undo
and redo retain intact before/after snapshots, including tuning, tabs, rests,
source evidence and musical context, and publish a new monotonic revision.
Moving a snapshot's document out during restoration previously broke later
undo/redo cycles; snapshots are now reusable.

The WebView history deep-copies pending musical/tab/context/review state,
selection and live-deletion tombstones. Pitch/time/fingering validation is
performed before publishing the draft to other views. Invalid same-string
overlaps roll back; cancelled pointer drags restore their starting snapshot.
New edits invalidate the redo branch, including keyboard pitch changes.
Selection is restored before rendering contexts that access the selected note.
Drag scaling is fixed at pointer-down so moving the final note cannot change
the time scale underneath the same gesture.

Pitch changes clear stale tab assignments instead of running a competing
per-note JavaScript fingering solver. Explicitly unassigned positions remain
unassigned. Native export/save/autosave accepts the complete linked assignment
projection rather than silently re-solving it. Manual string/fret changes must
preserve sounding pitch; an Unassigned choice and frets through 36 are supported.
Live-capture manual edits retain the current capture solver's 24-fret limit.
The backend revalidates the entire transaction. Each live UI operation is sent
as one batch, with a shared queue limit of 256 commands. The worker snapshots
the live document and its fingering map, applies the batch, checks assigned
string occupancy/pitch, and restores both on failure before publishing.
Rejections are reported rather than exposing a partial pitch/fingering edit.

Keyboard shortcuts remain Space, Delete, arrows, Ctrl/Cmd+Z, Shift+Ctrl/Cmd+Z,
Ctrl/Cmd+Y, Ctrl/Cmd+S and O. Offline edits/undo/drag/insert are locked while a
native operation owns the score. Live editing remains available for the initial
capture. Live fingering removal is propagated explicitly, including through
undo. Already accepted scores are not replaced by live previews from reanalysis.

## Job and source lifecycle

Native transcription messages carry a job identity. Cancelled/stale messages
cannot publish an accepted result; cancellation leaves the previously accepted
score intact. Reanalysis is a proposal even when the accepted score is empty.
Proposal acceptance asks before replacing unexported edits. Transient live
notes from a cancelled initial job are cleared.

Changing the input selection stops playback and clears preview buffers/queued
notifications. It does not delete an accepted score: the next analysis remains
a proposal. Original playback is disabled until a compatible source is accepted.
Preview queues are cleared at job transitions and completion. Export and save
preserve the accepted revision check; existing export formats are unchanged.

## Playback and remaining limitations

Synthesized preview schedules triangle oscillators on `AudioContext.currentTime`.
Its cursor uses that same clock, including the 80 ms scheduling lead-in, rather
than starting immediately on `performance.now()`. Pause/resume retains recording
seconds; stop, edits, source changes and page shutdown invalidate asynchronous
startup. Ended oscillators disconnect their gain/output nodes.

Original WAV playback now uses Windows MCI with a 30 ms position polling timer,
instead of `PlaySound` plus an estimated whole-file wall clock. Tokened native
position/end notifications drive the browser cursor and reject old transport
notifications. Native errors clear transport state. Explicit stop is distinct
from natural end so looping does not revive a stopped transport.

Limitations:

* Original audio and event-region audition are WAV-only and use Windows MCI.
  There is no original-audio pause/seek control, compressed-file playback or
  recorded-live-audio player in this interface.
* MCI milliseconds/30 ms polling and browser rendering are **not sample-accurate
  output-device latency measurements**. Hardware latency is not calibrated.
  AudioContext output-device latency is likewise not compensated.
* Synthesized playback is a lightweight pitch/duration preview, not a guitar
  performance or a full General MIDI synthesizer. It schedules the current
  note list rather than a bounded streaming synthesizer.
* JavaScript exact sample counters stop at `Number.MAX_SAFE_INTEGER`; conversion
  rejects larger positions. Native frame counters remain 64-bit.
* Engraved playback highlighting follows accepted note groups, not continuously
  interpolated horizontal spacing through rests and tied continuation fragments.
* Browser tests mock audio hardware and native messages. They do not verify
  physical output timing or actual MCI device behavior.
* WebView undo/redo is pending-edit history: an accepted backend commit or
  project load starts a new history, as before. It is not serialized in a
  project and cannot undo an already exported deletion across that boundary.

## Reproduction and regression tests

```powershell
cmake -S . -B build-tests -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-tests -j 4
ctest --test-dir build-tests --output-on-failure -R "recording samples|repeated score undo|canonical|ui_workflow_browser|interpretation_semantics|fingering_export_semantics"
python tests\ui_smoke.py ui
```

Use `-G "MinGW Makefiles"` when configuring with MinGW on Windows. The browser
test uses Python's standard library, an ephemeral localhost server and a private
temporary browser profile. Set `TRANSCRIBE_BROWSER` to a Chromium-family
executable if autodetection does not find Edge/Chrome/Chromium. CTest explicitly
skips it with code 77 if none is available; no package is installed.

For interactive inspection, serve the repository with
`python -m http.server 8765 --bind 127.0.0.1`, then open
`http://127.0.0.1:8765/tests/ui_workflow.html`.
The page reports pass/fail and runs real UI handlers with deterministic
animation callbacks, fake AudioContext time and a mock native message bridge.
It exercises conversions, edit rollback, pitch/tab synchronization, repeated
undo/redo, insertion/deletion/selection, cancelled dragging, tempo edits,
proposal consent, stale previews, live deletion tombstones, timestamped audio
gaps, asynchronous playback stop, audio-clock alignment and native playback
termination tokens. C++ tests additionally verify serialization and repeated
native snapshot restoration.

Validation on Windows during implementation:

* Full model-free CTest: **175 passed, 2 skipped / 177 registered**. The skips
  require the optional ONNX window model and an external real-audio fixture.
* ONNX/ASIO configuration: **31 selected CTests passed**, including the browser
  harness and semantic export suites; focused score tests passed **181
  assertions in 17 cases**.
* Final WebView2/ONNX/ASIO GUI build and UI asset smoke passed. Final browser
  tests passed with installed headless Edge; Node.js was unavailable, but the
  browser parsed and executed the production JavaScript.
* Physical MCI playback, audio output latency and ASIO live-edit interaction
  were not hardware-validated. Live-batch rollback was compiled in the native
  host; the browser harness tests the UI/message side with mocked hardware.
