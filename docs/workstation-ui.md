# WebView2 transcription workstation

The optional workstation is Transcriber's sole graphical interface and is the
WebView2 front end to its C++ application state, not a separate sample
application. It uses the same
transcription requests, canonical score revision, editing commands, export
path, live events, and MusicXML accepted by the native application.

## Workspace

- The transport opens an audio file, starts/stops supported ASIO capture,
  transcribes, plays or pauses the score preview, plays the original WAV,
  toggles score looping, navigates measures, undoes/redoes edits, and exports.
- The ASIO source controls enumerate input devices and expose the formats
  reported by each device. During capture, stable-ID provisional and stabilized
  note events update the same notation, tablature, and timeline used by editing.
  Live edits are sent to the capture worker and retained in the document history.
- After a live capture is finalized, **Refine recording** runs the backend's
  offline refinement mode. Proposed changes are reviewed separately from the
  accepted score. Manual-edit conflicts are withheld unless individually
  approved; the original event stream remains in the live document. An applied
  refinement can be undone until a subsequent score edit is exported.
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
  progress, and live event latency when the backend supplies it.

Source and inspector widths can be resized with their separators or keyboard
arrows and collapsed from either the panel or top bar. Layout widths, collapse
states, audio paths, and theme are saved locally in the WebView profile. Dark
and light themes, high-contrast focus indicators, responsive layouts, labels,
and transport/edit keyboard shortcuts are provided.

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
- Live capture, refinement, reconnect behavior, and the end-to-end UI editing
  workflow have not been hardware-validated in this environment. Automated
  smoke coverage verifies message/control wiring, not actual driver behavior.
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
