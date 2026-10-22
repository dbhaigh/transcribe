# Workstation usability and interaction contract

## Source audit before implementation

The supported Windows interface is the existing WebView2 workstation. The native
fallback was explicitly removed; optional ONNX, ASIO and Verovio integrations
change available capabilities, not the editor architecture.

### Existing journeys

| Journey | Current behavior and source |
|---|---|
| Open and analyze | `ui/app.js` sends `browse` and `transcribe` to `src/gui_webview.cpp`; a worker in `src/gui_transcription.cpp` decodes and analyzes. MusicXML and tab output paths are mandatory. Instrument and quality controls are buried inside a closed settings disclosure. |
| Record | Enumerated ASIO devices supply supported rates/channels. Record starts capture; pause suspends analysis, not recording; stop finalizes. Hardware availability gates controls. |
| Review | Waveform, band-energy spectrogram, note timeline, editable notation sketch and tab share sample seconds through `ui/timeline.js`. Optional Verovio pages engrave accepted scores in a worker. |
| Edit | `ui/editor.js` owns the draft, transactional undo/redo, note inspector, evidence review and alternative fingerings. Export/save sends a revision-checked transaction to the authoritative C++ score. Live results cannot silently replace an accepted draft. |
| Save | Projects preserve score, source reference and settings; autosave is a recovery copy, not an explicit save. Theme, panel sizes and analysis defaults persist locally. Isolation acceptance does not persist. |
| Playback | Synthesized notes and Windows WAV playback share cursor seconds. They are different sound sources; MIDI synthesis is not original guitar audio. |

### Confirmed interaction gaps

- First-run empty state offers opening/recording but no instrument/quality/review
  sequence. Important setup is hidden and the quality labels imply more than the
  selected backend necessarily supports.
- Progress displays raw percentages as if end-to-end estimates; callbacks are
  phase-dependent. Cancellation has no pending feedback and is reported as an
  error. Model loading can only cancel cooperatively.
- Unsaved indication only appears on named projects, not an unnamed edited take.
- Document-wide editor shortcuts also intercept focused buttons and dialogs.
  There is no discoverable shortcut reference or keyboard previous/next note.
- Audio canvases lack readable time, amplitude and frequency labels. The
  spectrogram uses backend band energies, not an arbitrary logarithmic FFT.
- The note timeline scans all notes and calculates pitch ranges repeatedly;
  spread-based pitch extrema can exceed argument limits for large scores.
- Existing styles contain dense 8-10 px labels and hard-coded colors that weaken
  contrast and theme consistency. Existing focus, resize and small-window
  behavior should be retained, not replaced.

## Implementation priorities

1. Expose one nonmodal open/setup/analyze/review guide using the existing controls.
   Add persistent contextual help and a keyboard-accessible shortcut dialog.
2. Unify readable type, spacing, focus, status and dialog treatments on existing
   theme tokens. Retain resizable panels and Windows scaling.
3. Make lifecycle states explicit, clamp phase progress without inventing an ETA,
   acknowledge cancellation and retain the accepted score on failure/cancellation.
4. Add visible sample-time navigation/axes and keyboard note selection. Preserve
   native text editing and button activation.
5. Bound viewport work and add critical-journey regression tests. Validate the
   real Windows host where available; report hardware and optional-build gaps.

## Remaining limits

No hard responsiveness guarantee is possible for arbitrarily large scores:
deserialization, draft snapshots and accepted-score publication still scale with
score size. Engraving runs outside the UI thread; canvas viewport work and event
lists must stay bounded. Native system file/confirmation dialogs retain Windows
behavior. Physical ASIO devices, audible playback and assistive-technology
validation require a Windows hardware/manual pass beyond browser automation.

## Implemented interaction changes

- Essential instrument/analysis setup is outside the advanced disclosure.
  A dismissible, restorable guide uses the real Open/Transcribe/review controls.
- Theme-aware tokens, readable labels, consistent control sizes, accessible
  contextual-help disclosures, a shortcut dialog, skip link and visible focus
  share the existing layout architecture.
- An activity card distinguishes empty, processing, completed, canceled and
  failed operations. Phase progress is clamped and labelled; missing progress
  is indeterminate. Cancellation acknowledges once without pretending to be
  instantaneous.
- Typed cancellation status now crosses the C++/JavaScript boundary. Source
  inspection also confirmed that the old GUI Stop capture path requested stop
  and then marked every capture result canceled. It now distinguishes ASIO
  finalization from offline cancellation, retaining the existing capture
  worker's finish/flush behavior. Physical-device validation is still pending.
- Primary status no longer gets overwritten by background recovery-autosave
  success or high-frequency latency telemetry. Detailed queue/latency values
  remain available in an inspector disclosure. Unsaved indication works for
  unnamed drafts as well as named projects.
- Two pre-existing output-folder buttons shared an ID, leaving the footer one
  unwired. IDs are now unique, both actions reach the backend, and the asset
  test rejects duplicate IDs.
- Time/amplitude axes and actual backend logarithmic frequency limits describe
  the preview data. Shared zoom, range-slider scrolling, Shift+wheel and keyboard
  navigation all use the existing sample-seconds mapping.
- Pitch bounds and the note interval index are cached across viewport changes.
  Timeline drawing is capped at 4,000 sampled overview events (plus selection),
  explicitly labelled; views below that cap paint every visible event. The draft and
  exports keep all notes. Existing 80-row event list and cursor-only playback
  painting remain in use. Editor shortcuts do not override modal dialogs,
  form controls, links or resize separators.

## Reproducing validation

After building with the documented CMake commands (substitute your build paths):

```powershell
python tests\ui_workflow_browser.py
python tests\ui_smoke.py ui
.\build-tests\transcriber-tests.exe "audio preview builds bounded waveform and log-frequency spectrogram data"
python tests\workstation_host.py --webview .\build-gui\transcriber-gui.exe
```

The browser runner uses an available Chromium/Edge with a temporary profile and
real elapsed time, not virtual-time timing. It skips with code 77 if no supported
browser is available. Critical-journey integration uses the production scripts
with a mocked message transport, including recording events, editing/undo,
proposals, cancellation and error recovery. It checks the actual 10,000-note
editor load/pan (not only the index helper), caps materialized rows/overview
painting and uses generous 3 s load / 1 s pan interaction budgets. These are
hang/regression guards, not transcription throughput thresholds.

The Windows host runner starts the actual executable, sends requests through
the production bridge, checks C++ MusicXML/MIDI/tab output, edits, undo/redo,
project save/reopen, cancellation preservation and missing-file recovery.
Its LOCALAPPDATA, WebView profile, exports and recovery copies are isolated
under a test-owned temporary directory. Profile cleanup waits up to 20 seconds
for WebView child-process handles rather than ignoring sharing violations.

Responsive tests cover 480, 720, 1024 and 1440 px without horizontal page overflow.
Both themes' core text/muted/accent-on-panel tokens must meet 4.5:1 contrast.
This does not certify every rendered color, screen reader or Windows DPI setting.

## Validation results (2026-10-21)

| Check executed | Result |
|---|---|
| Release dependency-light audio-preview test | Passed: 11 assertions, 1 case, including exact frequency limits |
| ASan/UBSan audio-preview test | Passed: 11 assertions, 1 case; Windows leak detection disabled |
| Selected CTest UI suite | All 3 passed: browser workflow, UI assets, notation fixtures |
| Browser journeys | 27 groups passed, including recording protocol, transactional edits, undo/redo, proposal protection, cancellation, source changes, viewport/selection, modal isolation, settings, panel collapse and responsive/contrast checks |
| Minimal Windows WebView2 host | Strict Release build and real C++ journey passed; ONNX/ASIO/Verovio disabled |
| Windows host with ONNX + ASIO + Verovio | Strict Release build and real C++ journey passed; real Verovio SVG pages verified after scrolling virtualized engraving into view |
| Visual inspection | Dark desktop and light compact layouts inspected in the browser; no new remote assets |
| Whitespace validation | `git diff --check` passed |

The final real-clock browser run loaded the actual 10,000-note editor in
108.8 ms and panned to a ten-second window in 50.4 ms on this Windows host,
while other build/host validation ran concurrently. These are single-run
observations, not median/p95 guarantees. The zoomed timeline contained 11 events;
the overview and event-list rendering caps were asserted. Initial regression
tests also exposed collapsed-panel grid auto-placement and duplicate button IDs;
both were fixed and the tests rerun successfully.

The IDE test adapter discovered no C++ tests; the built Catch executable was run
directly instead. The Node-only syntax check was skipped because Node is absent;
the actual production scripts parsed and executed in Chromium and WebView2.
No physical ASIO capture, audible playback, screen-reader certification, GPU
execution or exhaustive Windows DPI matrix is claimed. The native fallback
remains removed; these are configurations of the same supported WebView2 GUI.
