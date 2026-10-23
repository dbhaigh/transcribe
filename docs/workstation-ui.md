# WebView2 transcription workstation

The optional workstation is Transcriber's sole graphical interface and is the
WebView2 front end to its C++ application state, not a separate sample
application. It uses the same
transcription requests, canonical score revision, editing commands, export
path, live events, and MusicXML accepted by the native application.

## First recording

1. Choose **Open audio** (or **O**) and select a recording. WAV is built in;
   other formats need a build with FFmpeg.
2. In **Transcription setup**, select an instrument prior and **Full analysis**
   or **Fast preview**. A prior is not source separation. The built-in file
   tracker is monophonic; an ONNX model retains its own supported contract.
   Full analysis is the existing backend configuration, not an accuracy promise.
3. Select **Transcribe**. Review output locations before starting if you want
   to change the generated MusicXML/tab destinations. The activity card reports
   processing, completed, canceled, empty-result and error states.
4. Select a note in any view. Inspect measured onset/offset, score duration,
   raw evidence and suggested fingerings, then edit, save a project or export.

The nonmodal first-run guide tracks this journey. **Hide guide** persists
locally; **Help → Show first-run guide** restores it. **Help** / **F1** opens
the keyboard reference. Question-mark controls beside model, instrument,
analysis mode, tuning, capo, rhythm, tempo, meter and confidence labels expand
keyboard-accessible explanations; the same text describes the associated field
for assistive technology.

The interface uses local Segoe UI/Consolas fallbacks, shared theme colors,
4/8/12/16 px spacing tokens, consistent control/dialog treatments and visible
focus rings. No network font or new UI framework is required.

## Projects

**Save** preserves the accepted score, tuning/capo, rhythm/model settings and
annotations in the [native project format](projects.md). **Save with audio**
explicitly copies the recording beside a newly chosen project file; move both
files together. Ordinary saves keep external recordings external. **Score copy**
omits the recording. **Locate audio** repairs a missing reference without
replacing notes or starting transcription; Save afterwards to retain the new
reference. WAV relocation rejects incompatible rate/frame/channel metadata.

Write session annotations in **Summary → Project notes**. Reopening restores
accepted edits, tab assignments and revision audit records, but starts a new
undo/redo stack. Project integrity errors leave the currently accepted score
intact; recovery remains an explicit choice.

## Workspace

- The transport opens an audio file, starts/stops supported ASIO capture,
  transcribes, plays or pauses the score preview, plays the original WAV,
  toggles score looping, navigates measures, undoes/redoes edits, and exports.
  MusicXML, tablature, MIDI when configured, and score JSON are written from the
  accepted score document. JSON output is editable score data, not a replacement
  for the original live event stream.
- The ASIO source controls enumerate input devices and expose the formats
  reported by each device. During capture, stable-ID provisional and stabilized
  note events update the same notation, tablature, and timeline used by editing.
  Live edits are sent to the capture worker and retained in the document history.
  Stop drains queued audio and finalizes open notes. Cancel instead discards the
  capture and restores the previously accepted score without claiming any notes
  were finalized.
- The analysis worker only posts a lightweight event-ready notification. The
  WebView2/UI thread polls and applies event batches to the live document,
  resynchronizing from a coherent note snapshot if the bounded event queue
  overflows. It coalesces note upserts, retractions, finalizations, gaps, and a
  metrics/status snapshot into protocol-version 1 updates at a 17 ms tick
  (under 60 Hz).
  Stable note IDs carry monotonically increasing revisions; the UI ignores
  duplicate or stale revisions. The bounded event-detail list reports when
  older entries were coalesced. The status-bar health strip reports driver
  latency, callback period, analysis-window delay, input and analysis queue
  waits, analysis execution, confirmation, event wait, and native UI dispatch.
  Software stage timings include p50/p95 where sampled; zero-observation
  timings and unavailable driver latency are shown as unmeasured or unknown.
  Provisional/stabilized target misses are compared with the configured targets
  (defaults 200/300 ms). **OVERLOAD** reports queue/worker pressure and loss
  counts; **INPUT GAP** separately reports known missing frames, sequence gaps,
  and discontinuities. These are pipeline-stage measurements, not physical
  end-to-end latency. The OVERLOAD state clears when current queue/lag pressure
  subsides; its loss counters remain cumulative for the capture. Native UI dispatch is measured from the coalesced
  events-ready notification to the beginning of UI-thread polling.
- Advanced settings expose the `live-hop-ms` interval for Spectral and ONNX
  windowed live backends (10-1800 ms, default 250 ms). It does not alter YIN;
  the selected interval must fit the backend's analysis window.
- During capture, the editable notation, tablature, and piano-roll share one
  time viewport and render only visible live notes in animation-frame batches.
  Provisional notes are hollow/dashed, stabilized notes use a solid mark and
  `S`, finalized notes return to normal notation, corrections receive a brief
  outline, and retractions cross-fade before removal. Hatched **UNANALYSED**
  spans mark analysis gaps across all three canvases. Unresolved tablature is
  shown as `?`; the UI never guesses a string or fret. **Follow live** tracks
  the newest evidence edge; manual scrolling or zooming turns it off and shows
  **Jump to live**. Verovio engraving is held during capture and updated only
  after capture finishes.
- After a live capture is finalized, **Refine recording** runs offline
  refinement on a worker. ONNX backends use windowed refinement; built-in
  backends use full-context refinement. Proposed changes are reviewed separately
  from the accepted score. Manual-edit conflicts are withheld unless
  individually approved; the original event stream remains in the live
  document. An applied refinement can be undone until a subsequent score edit
  is exported.
- The source panel displays the selected audio waveform and spectrogram,
  preview-sample peak level, recording status, and setup options.
- The central score presents the editable notation and tablature timelines.
  When the optional Verovio renderer is available, accepted MusicXML is
  rendered as synchronized notation and tablature. See
  [`notation-rendering.md`](./notation-rendering.md) for its feature limits.
- Selecting a score note exposes its accepted pitch, measured onset and
  offset, supplied calibrated confidence, available raw evidence, source
  detector, and playable-fingering alternatives. Missing evidence is shown as
  unavailable; it is not synthesized by the UI.
- The status bar reports backend availability, capture/analysis state,
  progress, and the live health strip while live metrics are available.

Progress percentages describe the **current backend phase**, not end-to-end
completion or an ETA. Unknown progress is indeterminate. **Cancel analysis**
acknowledges once, disables repeated requests and waits for a safe backend
boundary; model initialization/in-flight calls cannot always stop immediately.
Canceling does not replace an accepted score. **Stop capture** instead requests
capture termination and finalizes the captured notes. A second **Cancel
capture** action while finalization is in progress discards that capture rather
than accepting or exporting its notes. The versioned native bridge reports
`idle`, `arming`, `capturing`, `interrupted`, `finishing`, `finished`, and
`failed` capture states. Saving and export are non-cancellable while committing
files; their controls do not offer a false cancel action.

The waveform is a normalized **peak-envelope preview**, not individual samples.
Its amplitude and seconds axes share the viewport with the spectrogram,
timeline, notation sketch and tablature. Spectrogram labels use actual backend
log-band limits (55 Hz through half the analyzed sample rate), not guessed FFT
frequencies; intensities show relative energy, not calibrated dB. Old preview
messages without frequency metadata display an unknown-Hz label.
Preview resolution is bounded, so zooming does not recover missing sample detail.
Progressive previews can retain only a recent portion; absent regions are not
represented as new audio.

Use timeline **+ / − / Full take**, the horizontal position slider,
**Page Up / Page Down**, or Shift+wheel/trackpad horizontal scrolling.
The slider's arrows are native accessible range controls. **0** resets to the
full take. Engraved-page zoom is separate from the shared time viewport.
At over 4,000 visible timeline events the display explicitly labels its sampled
overview; zoom in to inspect all events. The canonical score is not sampled.
Only the last 80 event-list rows are materialized.

Source and inspector widths can be resized with their separators or keyboard
arrows and collapsed from either the panel or top bar. Layout widths, collapse
states, audio paths, and theme are saved locally in the WebView profile. Dark
and light themes, high-contrast focus indicators, responsive layouts, labels,
and transport/edit keyboard shortcuts are provided.

With the editor focused, **Left/Right** selects previous/next notes, scrolling
the time viewport if necessary; without selection it navigates measures.
**Up/Down** edits pitch by a semitone, **Delete** removes a note, **Space** plays
or pauses the synthesized score, **Ctrl+Z** undoes, **Ctrl+Shift+Z** / **Ctrl+Y**
redoes, and **Ctrl+S** saves the project. Text fields, focused controls, editable
content and open dialogs keep their native keyboard behavior. Tab moves between
controls and Escape closes a modal dialog.

**Unsaved edits** is visible even for an unnamed recording, with an asterisk
in its title. Explicit project Save clears it; exporting files alone does not
claim to have saved the project. Autosave status is secondary and never clears
unsaved edits or replaces the primary operation status. Project settings,
analysis defaults, panel widths and themes retain their existing persistence;
source-isolation approval is deliberately session-only.

Tempo is inferred by default; switching off **Auto** sends the entered BPM as
an explicit interpretation constraint. Meter, key, pickup, and written
transposition likewise constrain interpretation of the next score proposal.
Tuning, capo, fingering alternatives, instrument prior, and analysis quality
configure supported backend operations. These controls do not alter measured
audio timestamps. Editing and undo/redo operate on the canonical score
revision.

Re-analysis does not silently replace an accepted score. If one is already
present, the result is presented as a proposal. The user must explicitly
choose **Replace score with proposal & export**; the backend exports the
proposal before updating the accepted revision. A failed export leaves the
current accepted score intact and keeps the proposal available for retry.

## Backend scope and limitations

- ASIO device lists and supported sample rates are driver-reported. The capture
  path rejects unavailable formats rather than silently opening a different
  device or changing the requested configuration.
- ONNX GUI inference currently uses the CPU provider. The displayed model and
  provider label reflects that implementation, not a generic provider picker.
- Original-recording playback uses the Windows WAV playback path. Score
  preview uses synthesized audio; it is not the original performance.
- The input level and clipping indicator are computed from backend preview
  samples. They are not hardware-driver clipping telemetry.
- Physical live capture, reconnect behavior, audible playback and assistive
  technology have not been hardware/manual-validated in this environment.
  Automated browser tests exercise capture messages; actual WebView2/C++ tests
  exercise file transcription, editing, project persistence and export.
- Pitch alternatives and confidence are displayed only when supplied by the
  backend. Raw evidence shown in the inspector is capped for display; this
  does not change the canonical event evidence.
- UI smoke tests verify markup, asset wiring, bridge operation allowlisting,
  and selected JavaScript integration points. They are not a substitute for
  device testing or pixel-based WebView2 visual regression tests; those depend
  on a pinned WebView2 runtime and graphics environment.

The WebView2 UI is an optional Windows build feature and does not add a runtime
requirement to the default CLI-only build. It is the only GUI; disabling
WebView2 produces no graphical application. Verovio is a separate optional
offline renderer; without it, the existing editable canvas remains available.

See [the usability audit and validation record](./workstation-usability.md) for
the source-level findings, test commands and remaining qualification work.
