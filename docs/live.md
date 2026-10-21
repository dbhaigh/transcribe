# Incremental live transcription

`transcribe::live` (`include/transcribe/live.hpp`) is an opt-in, additive pipeline
for responsive live note events with a separate offline refinement pass. The
existing `RealtimeTranscriber`, `--asio`/`--live` CLI capture, both GUIs and all
default CLI outputs are unchanged. It lives in `transcriber_core` and adds no
dependencies; Basic Pitch requires the existing optional ONNX build.

## Architecture

```text
audio callback ──push()──▶ SPSC input queue (bounded, lock-free, preallocated)
                                │ ingest worker
                                ▼
                rolling mono window (bounded) + bounded recording
                                │ analysis worker
                                ▼
   IncrementalYin  |  OverlappingWindow(ModelBackend::infer + decoder + reconcile)
                                │
                                ▼
          bounded event queue ──poll_events()──▶ consumer (UI/CLI) ── TranscriptionDocument
                                                         │
                      finish() ─▶ recording() ─▶ refine_recording() ─▶ ScoreProposal (review/apply)
```

| Thread | Work | Never does |
| --- | --- | --- |
| Audio callback (producer) | `push()` copies one block into a preallocated slot; `report_input_lost()` sets a flag | lock, allocate, block, analyse, touch disk/UI |
| Ingest worker | drains the input queue, downmixes, fills the rolling window and recording, records gaps | inference |
| Analysis worker | YIN steps or windowed inference, decoding, reconciliation, event emission | wait on the producer |
| Consumer | `poll_events()`, `snapshot()`, `metrics()` | — |

`LiveOptions::threaded = false` runs the same ingest/analysis code synchronously
from `pump()`; deterministic tests and offline windowed refinement use it.
`events_available` is invoked on the analysis worker and must not block (e.g. a
`PostMessage`).

### Frames and timestamps

* `AudioFrameBlock` carries `AudioFormat{sample_rate, channels}`, interleaved
  float frames, `first_frame` (device frame counter, including frames the
  producer could not deliver), `capture_time` (monotonic) and a
  `discontinuity` flag. Blocks whose format differs from the session format are
  rejected (`InvalidFormat`); non-monotonic `first_frame` is `InvalidTimestamp`.
* The recording timeline starts at the first accepted block. Every
  reported frame is `first_frame - origin`; seconds are frames / sample rate.
  `LiveNote::onset_frame`/`offset_frame` therefore refer to recording time, not
  wall time. Frame gaps produce an `AnalysisGap` event with
  `[gap_first_frame, gap_end_frame)` and zero-filled recording audio, so the
  timeline never compresses.

### Backends

| `LiveBackendKind` | Backends | Behaviour |
| --- | --- | --- |
| `IncrementalYin` | existing `RealtimeTranscriber` | true sample-incremental state machine; provisional note after onset, stabilized after `stabilization_seconds` of continued evidence |
| `OverlappingWindow` | any `ModelBackend` (`builtin:spectral`, `onnx:basic-pitch-v1`, ...) | repeated offline inference over `window_seconds`, advanced by `hop_seconds` |

Basic Pitch and the spectral tracker are **not** incremental models: they are
run on overlapping windows and the latency/accuracy trade-off is explicit in
`WindowOptions`. Windows grow from one hop at the start of a segment, so there
is no initial full-window wait. Within a window, onsets inside
`left_guard_seconds` of the left edge (except at a segment head) are treated as
possibly truncated, and notes ending within `right_guard_seconds` of the right
edge are treated as still sounding.

Reconciliation matches window candidates against tracked notes by pitch and
onset (`onset_match_seconds`), extends notes across seams
(`seam_merge_seconds`) and recognises continuations whose true attack preceded
the window (the decoder reports a later onset in that case). This prevents
duplicates and artificial gaps across windows. A note is stabilized after
`confirmations` windows observe it. Unconfirmed notes shorter than
`minimum_duration_seconds` are retracted. Onset/offset changes larger than
`correction_seconds` are emitted as corrections.

## Events

| `LiveEventKind` | Meaning |
| --- | --- |
| `NoteOn` | provisional note (`EventLifecycle::Provisional`) |
| `NoteStabilized` | confirmed by additional evidence |
| `NoteOff` | offset determined; may later be corrected or reopened |
| `NoteCorrected` | pitch/onset/offset revised, or a closed note reopened; same ID |
| `NoteRetracted` | provisional note withdrawn (`Rejected`); consumers remove it |
| `AnalysisGap` | frames not analysed live (input gap or skip-ahead) |
| `Status` | lifecycle/input/model diagnostic, ending with `finished`, `cancelled` or failure |

Every note keeps a stable `id` (`live-N`) for its whole life and an
incrementing `revision`. Events carry `previous` for revisions, a strictly
increasing `sequence`, and `EventTiming{onset_capture_time, emitted_time,
evidence_end_frame}`. `raw_strength` is the backend's raw activation or YIN
heuristic and is never a probability of correctness.

## Backpressure and bounded memory

* Input: `input_queue_blocks` preallocated slots of `max_block_frames`. A full
  queue returns `QueueFull`. Real-time producers drop the block and keep
  advancing their frame counter, so the loss appears as a gap (`blocks_refused`,
  `gap_frames`).
* `LagPolicy::SkipAhead` (live capture): when analysis lags more than
  `max_analysis_lag_seconds`, windows are first coalesced. If it is still
  behind, unanalysed audio is skipped with an `AnalysisGap` event and
  `skipped_frames` is counted. Audio remains in the recording for refinement.
* `LagPolicy::Lossless` (file replay only): ingestion stops draining and the
  producer observes `QueueFull` and retries. Nothing is skipped.
* Events: `event_queue_capacity`. On overflow, events are dropped and the next
  `poll_events()` returns `resync_required` and `dropped_events`; consumers rebuild
  from `snapshot()` (`TranscriptionDocument::resync`).
* Rolling audio, tracked notes (`max_tracked_notes`), snapshot notes
  (`max_snapshot_notes`) and the recording (`max_recording_seconds`) are all
  bounded; overflow is counted in `LiveMetrics`.

## Failure, cancellation and recovery

* `cancel()` stops workers promptly; destructors join. `finish()` drains queued
  audio, closes open notes and stops.
* `report_input_lost()` (e.g. device disconnection) moves the session to
  `InputInterrupted`. A later block resumes it, and the frame gap is reported
  explicitly. `capture_asio_live` reconnects the default device up to 5 times,
  500 ms apart, advancing the frame counter by wall time.
* Model errors: each failure triggers a model reload, up to
  `max_model_recoveries` in a row; after that the session enters
  `AnalysisFailed` with `last_error()`. Audio is retained for refinement. The
  backend is never silently switched.

## Latency definitions (`LiveMetrics`)

| Metric | Definition |
| --- | --- |
| `capture_latency` | block `capture_time` → ingested by ingest worker (queue wait) |
| `analysis_latency` | wall time per analysis step (inference + decode + reconcile) |
| `end_to_end_latency` | capture time of the onset's block → `NoteOn` queued |
| `stabilization_delay` | capture time of the onset's block → `NoteStabilized` queued |
| `algorithmic_latency` | recording-time distance from onset to analysed audio end at `NoteOn` |

Device/driver buffering before the callback is outside these numbers. In
unpaced replay, capture and end-to-end latency include artificial replay
queueing and are **not** device latency; use `--pace realtime`.

## Refinement and the editable document

`refine_recording(session.recording(), RefinementOptions)` is blocking (call it
from a worker) and returns a separate `ScoreProposal`; it never changes the live
score.

* Full context (default): one inference over the whole recording followed by
  the canonical temporal decoder. `ScoreProposal::events` holds raw evidence.
* Windowed (`RefinementOptions::windowed`): the recording is replayed offline,
  without latency limits, through the same overlapping-window reconciler.
  `events` is empty in this mode.
  This exists because whole-recording Basic Pitch inference measured worse than
  its native short windows (see below). The CLI/runner `auto` mode uses
  windowed refinement for `onnx:*` backends and full context otherwise.

`TranscriptionDocument` (owned by one consumer thread) applies live events and
keeps every note's revision history (`RevisionOrigin::Live/Resync/Refinement/Manual`)
and the original event stream (`original_events()`). It supports
`add_manual_note`, `edit_manual_note` and `delete_manual_note`. Live updates to a
manually edited note are recorded in history (`superseded_by_manual`) but not
applied. `review(proposal)` is read-only and produces add/modify/remove
changes against the current revision, flagging any change that touches a
manually edited note. `apply(review, approved_conflicts)` applies
non-conflicting changes and only those conflicting changes whose `change_id`
is explicitly approved. It throws `std::logic_error` if the document changed
after the review. Deleted and rejected notes remain in history.

## CLI

```text
transcriber take.wav --live-pipeline [--backend <registry-id>] [--live-events events.jsonl]
                     [--refine [--refine-backend <id>] [--refine-mode auto|full|windowed]
                      --proposal-json proposal.json]
transcriber --asio 30 --live-pipeline ...   (ASIO build; SkipAhead policy)
```

`--live-pipeline` replays WAV input losslessly in 1024-frame blocks, or captures
ASIO (`capture_asio_live`, 44.1 kHz mono). The default is incremental YIN; `--backend` selects a
windowed model (`--model` requires an explicit `--backend`). Existing outputs
(`--musicxml`, `--tab`, `--midi`, `--json`) are written from the live
document. `--live-events` writes the original event stream as JSON lines.
Refinement writes only `--proposal-json` (`"applied": false`); the live score
is left unchanged. The new options are rejected unless `--live-pipeline` is
given, and `--live-pipeline` is incompatible with `--batch` and `--fast-preview`.

## Evaluation

`transcriber-live` (built with tests) replays one WAV and writes JSON metrics.
`python/live_evaluation.py` runs it over the deterministic synthetic corpus
(16 clips, 41.4 s; no downloads). It scores live and refined notes separately
with the same event matcher as [Evaluation](evaluation.md) (50 ms onset,
max(50 ms, 20 %) offset). It gates on:
* clean finish;
* bounded queues;
* no dropped events or duplicate live notes;
* no skipped frames under `lossless`;
* `--max-refinement-f1-drop`.

```powershell
python python/live_evaluation.py --runner build/transcriber-live.exe `
  --build-info build/evaluation-build-Release.json --backend spectral `
  --pace realtime --policy skip --output-json live.json --output-text live.txt
```

CTest registers `live_evaluation_yin` and `live_evaluation_spectral`, plus
`live_evaluation_onnx` in ONNX builds with a model (labels `evaluation;live`,
`--max-refinement-f1-drop 0 --check`). The `[live]` Catch2 tests cover:
* prerecorded chunks with simulated clocks;
* dropped frames, discontinuities, device loss and resumption, and forced worker delays and overload;
* model failure and recovery, cancellation, and chunk-size invariance;
* hidden-onset reconciliation, format/timestamp validation, channel downmix and non-finite sample sanitizing;
* bounded event queues with snapshot resync, document revision safety, and full and windowed refinement;
* sustained threaded stress (`[stress]`).

### Measured results

Synthetic corpus v1, Windows, LLVM-MinGW Clang 23.1.1, Release `-O3`,
44.1 kHz input, 256-frame blocks, window 2.0 s / hop 0.25 s, ONNX Runtime
1.20.1 CPU provider. Note-event F1 uses the strict onset+offset matcher.

| Live backend | Live P/R/F1 | Refinement | Refined P/R/F1 |
| --- | --- | --- | --- |
| incremental YIN | .727/.667/.696 | spectral, full | .870/.833/.851 |
| incremental YIN | .727/.667/.696 | Basic Pitch, windowed | .913/.875/.894 |
| spectral window | .870/.833/.851 | spectral, full | .870/.833/.851 |
| spectral window | .870/.833/.851 | spectral, windowed | .870/.833/.851 |
| Basic Pitch window | .913/.875/.894 | Basic Pitch, full | .870/.833/.851 (regression; gate failed) |
| Basic Pitch window | .913/.875/.894 | Basic Pitch, windowed | .913/.875/.894 |

Basic Pitch live note-event F1 by window length: 2 s .894, 4 s .894, 30 s
(effectively whole-clip) .851. This is why `auto` refinement is windowed for
ONNX models.

Real-time paced replay, `--policy skip` (worst clip p95, ms):

| Live backend | capture | analysis step | end-to-end | stabilization | algorithmic | skipped frames |
| --- | --- | --- | --- | --- | --- | --- |
| incremental YIN | 13.6 | 0.2 | 69.6 | 76.4 | 68.8 | 0 |
| spectral window | 10.8 | 29.4 | 342.4 | 593.1 | 301.7 | 0 |
| Basic Pitch window | 9.3 | 100.1 | 411.0 | 662.6 | 350.4 | 0 |

Queue high-water was 3/256 input blocks and 3 events in every paced run, with no
dropped events. Unpaced lossless replay is faster than real time and fills the
input queue (256/256) by design.

Overload probe: Basic Pitch with `--hop-seconds 0.02`, paced, `skip` policy.
* The analysis worker was saturated (43.9 s busy for 41.4 s of audio).
* `SkipAhead` coalesced 1,339 windows into 745 analysis steps and held
  maximum lag to 62 ms.
* No frames were skipped, and note-event F1 stayed at .894.
* Worst-clip p95 end-to-end latency was 217 ms. Larger hops wait longer for
  evidence, so a finer hop with coalescing was faster here.

The skip-with-gap path is exercised deterministically by the `[live]` lag and
`[stress]` overload tests, not by this probe.

## Limitations

* No bundled model is truly incremental. Basic Pitch and the spectral tracker
  use overlapping windows, and their latency is bounded below by hop size plus
  right-guard plus inference time.
* `capture_asio_live` can enumerate and open the selected ASIO device, sample
  rate, and input-channel count, and reconnects the selected device after
  interruption. Device-loss handling has not been exercised on ASIO hardware.
  Device buffering latency is not included in the metrics.
* The synthetic corpus is small and does not establish real-recording accuracy.
  Measured differences between modes on it are a few notes. Evaluate on
  licensed real audio (see [Evaluation](evaluation.md)) before generalising.
* Windowed refinement does not carry canonical raw evidence events.
* The optional WebView2 workstation consumes the live API for ASIO capture,
  live event display, canonical score updates, and in-capture edits. It offers
  offline refinement as a separate reviewed proposal; non-conflicting changes
  may be applied directly, while changes against manual edits require an
  individual approval. The WebView2 workstation is the only graphical
  interface; CLI workflows remain available separately.
  End-to-end capture and refinement have not been verified on physical ASIO
  hardware in this environment.
