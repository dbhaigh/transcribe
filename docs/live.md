# Incremental live transcription

`transcribe::live` (`include/transcribe/live.hpp`) is an additive pipeline for
responsive live note events with a separate offline refinement pass. Existing
`RealtimeTranscriber`, `--asio`/`--live` CLI capture, and default CLI output
behavior remain compatible. The optional Windows ASIO/WebView2 workstation
uses this pipeline for live capture and editing. It lives in
`transcriber_core` and adds no dependencies; Basic Pitch requires the existing
optional ONNX build.

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
`PostMessage`). Lifecycle methods are owner-thread operations: hooks/notifiers
must signal the owner, not call `finish()`, `cancel()` or destroy the session.

### Frames and timestamps

* `AudioFrameBlock` carries `AudioFormat{sample_rate, channels, layout}`, interleaved
  float frames, `first_frame` (device frame counter, including frames the
  producer could not deliver), `capture_time` (monotonic) and a
  `discontinuity` flag. Blocks whose format differs from the session format are
  rejected (`InvalidFormat`); non-monotonic `first_frame` is `InvalidTimestamp`.
* By default the recording timeline starts at the first accepted block. ASIO
  capture and the replay runner declare `begin_input(0)` before starting the
  producer and `end_input(end_frame)` after callbacks have stopped. This retains
  leading/trailing refused or null blocks, including an entirely missing input.
  These owner-thread declarations require a quiescent producer. Every
  reported frame is `first_frame - origin`; seconds are frames / sample rate.
  `LiveNote::onset_frame`/`offset_frame` therefore refer to recording time, not
  wall time. Frame gaps produce an `AnalysisGap` event with
  `[gap_first_frame, gap_end_frame)` and zero-filled recording audio, so the
  timeline never compresses.
* Negative/regressing nonzero capture clocks, regressing frame counters and
  frame-counter addition overflow are rejected. A zero clock means unavailable.
* Layout is explicit: `Mono` requires one channel, `Stereo` two; the compatible
  default `DiscreteInterleaved` supports 1-64 channels without an inferred
  speaker mapping. The ingest worker averages channels once, outside capture.
  Layout cannot change within a session.
* New producers set `sequence` and `stream_epoch`. Sequences strictly increase
  for accepted blocks; retry `QueueFull` with the **same** sequence. Once
  sequencing starts, missing/duplicate/regressing sequences are rejected.
  Skipped sequence numbers increment `sequence_gaps` and start a new analysis
  segment even when sample timestamps are contiguous. Known missing sample
  counts still come from absolute timestamps, not sequence numbers.
  A wrong epoch is rejected. Legacy unsequenced producers remain supported.
* Each session is one take, not a reusable stream. Begin a fresh session on
  restart with a distinct `stream_id`/epoch. IDs are `<stream_id>/live-N`
  (legacy `live-N` when empty). Engine instances automatically number their
  successive takes; callers combining **different engine instances** in one
  document must provide globally distinct stream IDs/source identities.

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
| `NoteFinalized` | closed, stabilized note will receive no further live revisions |
| `AnalysisGap` | frames not analysed live (input gap or skip-ahead) |
| `Status` | lifecycle/input/model diagnostic, ending with `finished`, `cancelled` or failure |

Every note keeps a stable `id` for its whole life and an
incrementing `revision`. Events carry `previous` for revisions, a strictly
increasing `sequence`, and `EventTiming{onset_capture_time, emitted_time,
evidence_end_frame}`. `raw_strength` is the backend's raw activation or YIN
heuristic and is never a probability of correctness.

`NoteOff` is **not** final: overlapping windows can still adjust/reopen it.
`LiveNote::finalized` is independent of its stabilized lifecycle. YIN finalizes
at release; fixed-window adapters finalize after the note leaves the mutable
reconciliation horizon, or when a gap/end-of-stream closes that segment.
Finalization has its own increasing revision and `previous` evidence.
The consumer document ignores stale/equal revisions and refuses subsequent live
mutation of finalized notes. Manual edits and explicitly reviewed offline
refinement remain possible. Cancellation is not a successful EOS and does not
promise finalized partial notes.

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
  from that batch's `resync_snapshot` (`TranscriptionDocument::resync`). The
  snapshot and drain share one lock; even a partial poll discards superseded
  queued revisions. Do not obtain a later, separate snapshot for this resync.
* Discontinuity markers: at most 1,024 pending entries. A discontinuity storm
  coalesces tail markers into one analysis gap; `input_markers_coalesced` and
  `skipped_frames` expose the additional skipped analysis. Recording frames
  remain at their original positions.
* Rolling audio, tracked notes (`max_tracked_notes`), snapshot notes
  (`max_snapshot_notes`) and the recording (`max_recording_seconds`) are all
  bounded; overflow is counted in `LiveMetrics`.
  The consumer's score/history necessarily grows with accepted musical output;
  it is **not** part of the bounded audio/analysis working set. Poll throughout
  long processing: a final bounded snapshot alone cannot recover previously
  evicted notes.

### Shared engine input and progressive files

`AudioInputSink` is shared by `LiveTranscriptionSession` and the optional
`IncrementalEngine`. `capture_asio_stream` accepts either sink. The owner begins
the engine/session before capture at frame zero, polls from the control-thread
update hook, then finishes at `AsioLiveCaptureReport::frames_delivered` after
callbacks stop. Device-loss notification and sequences use the same path.
The existing `capture_asio_live` wrapper retains its public contract.

`transcribe_wav_progressive` composes `inspect_wav`, `read_wav_blocks` and one
initialized/configured engine. It never loads the entire WAV, starts one session,
backpressures losslessly, feeds source-frame timestamps and sequences, polls
owned updates, and flushes the exact final frame. Each decoded block fits a
preallocated input slot, avoiding an impossible QueueFull retry when a caller
configures a queue smaller than its requested decode block. Cancellation and
consumer exceptions return explicit errors and cancel the engine.

```cpp
transcribe::EngineRegistry registry;
auto created = registry.create("builtin:yin");
if (auto* error = std::get_if<transcribe::InferenceError>(&created))
    throw std::runtime_error(error->message);
auto engine = std::move(std::get<std::unique_ptr<transcribe::TranscriptionEngine>>(created));
auto initialized = engine->initialize();
if (auto* error = std::get_if<transcribe::InferenceError>(&initialized))
    throw std::runtime_error(error->message);
transcribe::live::TranscriptionDocument document(sample_rate);
transcribe::ProgressiveFileOptions file;
file.live.threaded = false; // Same persistent state machine, pumped by the helper.
file.live.max_recording_seconds = 0; // No duplicate retained audio.
file.live.stream_id = "recording-42";
file.update = [&](const transcribe::EngineUpdate& update) {
    if (update.live_changes) {
        document.apply(*update.live_changes);
        if (update.live_changes->resync_required)
            document.resync(update.live_changes->resync_snapshot);
    }
};
auto result = transcribe::transcribe_wav_progressive(*engine, path, file);
if (auto* error = std::get_if<transcribe::InferenceError>(&result))
    throw std::runtime_error(error->message);
engine->shutdown();
```

The helper inspects and reopens the source with identical format/frame-count
checks, not a content hash; do not replace a file while processing it.
FFmpeg/non-WAV CLI input still uses full-buffer decoding. Fixed-window spectral
and neural paths retain only bounded rolling audio, but **rerun** the model/DSP
over overlapping context; they do not cache hidden neural state or implement
truly incremental FFT inference.

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
* Exceptions escaping ingestion, analysis hooks or notification hooks become
  `AnalysisFailed` with `last_error()`, rather than terminating the process.
  Partial thread startup rolls back and joins. Stop/cancel close admission and
  wait for an already-admitted producer operation before draining/joining.
  The producer must still stop before the session/transport is destroyed:
  this handshake does not make use after destruction safe.

### Windows ASIO transport and lifecycle

ASIO remains an optional RtAudio integration. Both callbacks delegate to the
same hardware-independent transports exercised by `[asio]` tests:

* Legacy capture checks **both** fixed-capacity recording and analysis rings
  before committing a block. Overflow, driver overflow or null input aborts
  explicitly; it never silently returns a partial successful recording.
* Live capture never retries or waits for the worker. Refused/null blocks
  advance the timeline; the next accepted block carries a discontinuity.
  Known missing frames are counted/zero-filled, not compressed out. A driver
  overflow reports an unknown lost length: its flag is counted but exact
  missing hardware frames cannot be reconstructed.
* Sample rings require exactly one producer and one consumer each. Their
  acquire/release totals count occupancy; independent physical positions wrap
  at arbitrary capacity without relying on integer-total modulo capacity.
  Producer `writable()` and consumer `readable()` are role-specific snapshots,
  not third-observer synchronization. Empty reads return zero; short reads and
  writes report the actual count. Capture admission prevents partial writes.
* Callback work is bounded copies, timestamp acquisition, lock-free atomics and
  preallocated queue admission. No application allocation, mutex acquisition,
  inference, file I/O or notification hook runs there. Lock-free atomics are
  checked at compile time. This is source verification, not allocator tracing
  or a hard-real-time guarantee about Windows, RtAudio or its driver.
* A non-callback process-wide try-lock rejects overlapping enumeration/capture
  ownership. Stream guards close the driver before callback state is destroyed,
  including exception paths; analysis threads are joined. Open/start/stop errors
  are checked. Stream cleanup relies on RtAudio close quiescing callbacks.
* Unexpected stream stop or two seconds without frame progress triggers a live
  reconnect (at most five attempts, 500 ms waits checked for cancellation every
  10 ms). Legacy capture fails instead. Reconnect gaps use elapsed host time:
  they are estimates, not a recovered hardware sample clock. Repeated sessions
  construct fresh state; a completed live session is not restartable.
* The live report separates timeline end (`frames_delivered`, historical name)
  from `frames_accepted`, `null_input_frames`, refused blocks, driver overflows
  and reconnects. Negotiated `buffer_frames` and RtAudio
  `driver_latency_frames` are metadata, not loopback measurements.
  `AsioCaptureOptions::stream_opened` reports these on the owner/control thread
  after each successful start, never on the audio callback.
* Legacy `RealtimeTranscriber::take_snapshot()` pairs its notes and drained
  events under one mutex. Live consumers similarly use paired event/resync
  publication; no callback exposes a partially mutated vector.

## Latency definitions (`LiveMetrics`)

| Metric | Definition |
| --- | --- |
| `capture_latency` | block `capture_time` → ingested by ingest worker (queue wait) |
| `analysis_latency` | wall time per analysis step (inference + decode + reconcile) |
| `end_to_end_latency` | capture time of the onset's block → `NoteOn` queued |
| `stabilization_delay` | capture time of the onset's block → `NoteStabilized` queued |
| `algorithmic_latency` | recording-time distance from onset to analysed audio end at `NoteOn` |
| `analysis_queue_latency` | newest evidence block ingestion → start of analysis step |
| `confirmation_latency` | first provisional `NoteOn` → `NoteStabilized` |
| `publication_queue_latency` | event queued → consumer poll |

Summaries now expose nearest-rank **p50, p95 and p99**, plus mean, maximum and
sample count. `input_queue_depth`, `event_queue_depth` and
`current_lag_frames` are current observational snapshots (not synchronization
barriers); high-water marks remain separate. `sequence_gaps`, `gap_frames`,
`frames_refused`, `skipped_frames` and overload counts distinguish sequence loss,
known sample loss, admission pressure and deliberate analysis skipping.

### Explicit latency budget and targets

`latency_budget(options, capture_block_frames)` returns a **planning estimate**,
not a guaranteed bound or an empirical latency:

| Stage | Planned allowance |
|---|---|
| Capture accumulation | callback block frames / source rate |
| Ingest wait | configured ingest poll interval |
| YIN context | configured `RealtimeOptions::latency_ms` |
| YIN onset confirmation | `onset_stability_frames / update_rate_hz` |
| Fixed-window future context | right-edge guard |
| Fixed-window update wait | hop duration |
| Stabilization | YIN `stabilization_seconds`, or `(confirmations - 1) * hop` |
| Compute / scheduling | **not assumed zero**; measure analysis/queue summaries |
| Consumer/UI dispatch / paint | measure event queue and native UI dispatch; browser paint remains unmeasured |

The provisional estimate sums capture, ingest, context and update allowances;
the stabilized estimate adds stabilization. Inference startup, pitch/attack
ambiguity, host scheduling and delayed block delivery can exceed either.
Finalized-note latency is deliberately separate: window reconciliation can
remain mutable until the note leaves the rolling horizon or EOS.

`LiveOptions::target_provisional_seconds` / `target_stabilized_seconds`
(defaults 0.2 / 0.3) only count capture-relative target misses. They never
shorten pitch windows, suppress quiet notes, change thresholds, or skip audio.
The CLI/config equivalents are `--live-target-ms` and
`--live-stabilized-target-ms`; `--live-hop-ms` explicitly changes the
fixed-window update/recompute rate (default 250). Targets must be positive and
ordered; hop supports 10-1800 ms with the default two-second context.

Choose responsiveness **and** decoder independently: YIN remains monophonic;
spectral/Basic Pitch retain polyphonic scope. Basic Pitch `--neural-decoder
speed|quality` uses the measured legacy/multi-head strategy, not an automatic
GPU choice. The replay evaluator records both timing and event/frame quality
when comparing hop/decoder settings. A relaxed 500/800 ms windowed target is an
explicit application budget, not a promise that every event meets it.
`--require-latency-target --check` gates observed paced events; summaries label
zero-observation clips **unmeasured**, never successful. Silence naturally has
no note-on latency samples. Miss counts include every measured event, not just
those matched to reference annotations.

Device/driver buffering before the callback is outside these numbers. In
unpaced replay, the runner supplies no capture timestamp, so capture-relative
latency samples are unavailable; use `--pace realtime`.
These summaries overlap and must not be added. `capture_latency` is the
historical label for input queue wait, **not** physical capture latency.
Percentiles use the most recent 4,096 observations per summary; count, mean and
maximum cover its lifetime.

The WebView workstation separately displays the reported driver latency (unknown when
unavailable), negotiated callback period, analysis-window delay, input/analysis
wait, analysis execution, confirmation, event wait and native UI dispatch delay.
Dispatch measures worker post → native message handler, not WebView IPC, browser
rendering or display/paint. `OVERLOAD` indicates current queue/worker pressure
and clears after pressure subsides; its loss counters remain cumulative.
`INPUT GAP` separately indicates known missing input. Both expose loss counts
rather than substituting successful-looking output.

### Workstation audio preview

During ASIO capture, the WebView2 workstation refreshes the waveform and
spectrogram with a rolling three-second preview about every 150 ms. The
downsampled waveform carries signed minimum and maximum values per time column;
the spectrogram carries relative energy in log-frequency bands. Preview
timestamps remain recording-relative, but this bounded display is not a
full-recording history and does not alter the audio supplied to analysis.

### Hardware-independent workstation replay

`ui_live_callback_replay` replays `samples/guitar_scale.wav` at real-time pace
through `LiveCaptureTransport`, `LiveTranscriptionSession`, the production
bridge coalescer and the browser mock host. It checks provisional, stabilized
and finalized delivery on the same stable ID for each expected sample note.
The 500 ms analysis-delay probe checks that the UI displays OVERLOAD during
worker lag and clears it after recovery; skipped frames remain visible as
losses. This is a callback/bridge/UI integration check, not a test of a physical
ASIO driver or native browser paint latency.

The recorded realtime synthetic-v1 callback evaluations matched their prior
live note-event F1 baselines: YIN **0.696** and Spectral **0.851**. Both
`--check` runs passed with zero refused input blocks, skipped frames, dropped
events, or known missing input. These synthetic results do not qualify
hardware latency or real-recording accuracy. See the
[ASIO manual acceptance checklist](live-manual-acceptance.md); hardware results
are not yet validated.

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
WAV input now uses metadata inspection and incremental decoding rather than
preloading audio. Without `--refine`, duplicate recording retention is disabled;
file refinement retains at most 600 seconds and rejects longer requests with an
offline-refinement recommendation. Complete-input non-WAV decoding remains a
limitation. Main-score/history storage still scales with musical output.

```powershell
.\build\transcriber.exe take.wav --live-pipeline --backend builtin:spectral `
  --live-hop-ms 100 --live-target-ms 200 --live-stabilized-target-ms 300 `
  --live-events revisions.jsonl --json notes.json
.\build\transcriber.exe take.wav --live-pipeline --backend onnx:basic-pitch-v1 `
  --model .\python\models\basic_pitch.onnx --polyphonic --neural-decoder quality `
  --live-target-ms 500 --live-stabilized-target-ms 800
```

The second example also works with `--asio <seconds>` in an ASIO build.
Inference remains on the analysis worker, never the audio callback.

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

### Historical ASIO-hardening verification and callback-replay measurements

The hardening fixes source-confirmed dual-ring partial publication, callback
state lifetime during exception cleanup, ignored RtAudio return codes, lost
null-input timeline frames, uncaught worker exceptions and non-atomic
event/snapshot resynchronization. It also separates physical ring positions from
wrapping occupancy counters. Public entry points and default backends remain.

Hardware-independent coverage includes:

* concurrent SPSC transfer of 250,000 samples through capacities 1, 7 and 257,
  empty reads, partial final callbacks and dual-ring overflow admission;
* a simulated two-hour/57.6-million-frame callback timeline at 8 kHz through
  1,027-sample fixed rings (not two hours of wall time or device capture);
* null input, driver overflow flags, queue refusal, exact leading/trailing
  zero-filled gaps, timestamp/frame overflow rejection;
* 100 fresh sessions racing producer admission with owner finish/cancel;
* hook/notifier failure containment, coherent overflow resync, exact
  simulated-clock latency assertions and 1,100 discontinuities exercising the
  1,024-marker bound.

Validation completed on Windows/LLVM-MinGW Clang 23.1.1:

| Configuration/check | Result |
| --- | --- |
| Model-free complete CTest | 157 passed, 2 skipped / 159 registered |
| ONNX + ASIO complete CTest | 164 passed, 1 skipped / 165 registered |
| Focused `[asio],[live]` | 5,549 assertions in 28 cases passed |
| GUI + ONNX + ASIO build | passed |
| Final replay-harness follow-up | model-free live evaluations 2/2; ONNX live evaluations 3/3 passed |
| WebView browser mock host | overload/input-gap warnings and separated diagnostics verified; no page errors |
| UI asset smoke / whitespace | passed; Node-only syntax check unavailable |

The ONNX-specific bounded inference test is skipped in model-free builds; the
optional real-audio smoke requires an external fixture. Complete suites preceded
the final runner-only trailing-gap correction; the affected live evaluations
were rerun afterwards. Existing compiler warnings remain.

Reproduce without ASIO hardware or ONNX (use the repository's documented toolchain
setup; omit `-G` when reusing an already-configured build):

```powershell
cmake -S . -B build-live -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-live -j 4
ctest --test-dir build-live --output-on-failure

python python\live_evaluation.py --runner build-live\transcriber-live.exe `
  --build-info build-live\evaluation-build-Release.json `
  --backend yin --refine-backend none --pace realtime --policy skip `
  --callback-replay --output-json build-live\yin-callback.json `
  --output-text build-live\yin-callback.md --check
```

Repeat with `--backend spectral`. For controlled worker overload add
`--analysis-delay-ms 500`; for deliberate input-queue overflow also use
`--pace none`. Delay injection is test-only. `--callback-replay` requires `skip`:
it calls the actual extracted callback transport once per block and never
retries a refused block. Its duration measurement storage is reserved before
replay and populated outside the transport call. The runner declares both ends
of the input timeline, so dropped final blocks are not hidden.

Final local reports are in
`build-evaluation-quality/evaluation/asio-hardening/{yin,spectral,overload,queue-overflow}.{json,md}`.
Each JSON includes source hashes, dirty revision identity, runner hash,
compiler/flags/features, environment, corpus identity, configuration and per-clip
metrics. Source and binary identities were verified after collection. This
synthetic regression corpus is not evidence of real-device or real-recording
accuracy; the reports preserve separate quality scores.

Protocol: existing synthetic corpus v1, 16 clips/41.4 seconds, **22,050 Hz mono**,
256-frame blocks, model-free Release `-O3 -DNDEBUG`, no neural provider,
two-second spectral windows/250 ms hops. Host scheduling and polling contribute
to the observations. Each backend receives 3,577 callbacks. Values below are
**worst per-clip p50 / worst per-clip p95 / maximum**, not pooled quantiles;
low note-event counts can make a clip's p95 equal its maximum.

| Software interval | YIN | Spectral |
| --- | --- | --- |
| Callback transport execution (microseconds) | 2.1 / 6.2 / 211.9 | 3.3 / 6.4 / 40.8 |
| Input queue wait (ms) | 7.4 / 7.5 / 14.1 | 7.2 / 8.1 / 15.4 |
| Analysis queue wait (ms) | 0.2 / 7.2 / 14.1 | 0.1 / 7.4 / 7.5 |
| Analysis execution (ms) | 0.1 / 0.1 / 0.5 | 14.2 / 29.2 / 29.2 |
| Callback-to-provisional onset (ms) | 63.1 / 63.1 / 63.1 | 316.5 / 329.6 / 329.6 |
| Audio-time window/evidence delay (ms) | 61.0 / 67.7 / 67.7 | 300.5 / 303.9 / 303.9 |
| Provisional-to-stabilized (ms) | 20.7 / 20.9 / 20.9 | 264.8 / 270.4 / 270.4 |
| Event queue wait (ms) | 7.3 / 14.4 / 14.4 | 9.6 / 14.4 / 14.4 |

Both ordinary paced runs had zero refused blocks, missing/skipped frames and
dropped events; input high-water was 2/256. Note-event F1 remained .696 (YIN)
and .851 (spectral), matching the historical corpus baseline.

The paced 500 ms delay probe reported overload on 13/16 clips and coalesced
67 windows, with input high-water 3/256 and no input/event loss. Worst-clip p95
analysis wait was 768.8 ms, execution 534.9 ms and onset publication 1,086.5 ms.
F1 remained .851; this does not make the extra latency acceptable for interaction.
The unpaced queue-overflow probe refused 1,166 blocks, reported **298,289 missing
input frames**, skipped 564,337 analysis frames and held input high-water to
256/256, with no event loss. Its note-event F1 was **0**, deliberately not hidden
by successful structural checks. `--check` here checks lifecycle/queue integrity,
not an accuracy threshold or absence of overload under `skip`.

**Not measured:** physical ADC/loopback latency, an actual ASIO callback,
device hot-unplug/reconnect behavior, vendor-driver callback quiescence, native
UI dispatch distributions or browser paint latency. Software replay excludes
RtAudio/driver work, and its maximum durations are observations, not bounds.
Integer-total rollover is handled in source but was not forced through
2^64 frames in the stress run. No hard-real-time guarantee is claimed.

### Historical measured results (before ASIO hardening)

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

## Reproducing persistent-stream measurements

After a documented Release build with tests, run the existing evaluator (no
dataset downloads are needed):

```powershell
python python\live_evaluation.py `
  --runner build-live\transcriber-live.exe `
  --build-info build-live\evaluation-build-Release.json `
  --backend yin --refine-backend none --corpus-version 1 --repetitions 2 `
  --pace realtime --policy skip --callback-replay `
  --target-ms 200 --stabilized-target-ms 300 `
  --output-json build-live\incremental\yin.json `
  --output-text build-live\incremental\yin.md --check
```

Repeat serially with `--backend spectral --hop-seconds 0.25` and `0.1`.
For an optional ONNX build, explicitly acquire the pinned licensed model as in
[Neural strategy](neural-strategy.md), then use `--backend onnx --model
python\models\basic_pitch.onnx --neural-decoder speed` and `quality`. Keep
two-second context/other pitch parameters unchanged. To reproduce the controlled
overload, use spectral/250 ms hop with `--analysis-delay-ms 500`.
Do not run builds/other benchmarks concurrently. The evaluator verifies source
and binary identities before/after collection, records each repetition, and
retains separate quality metrics beside latency; it does not pool latency
quantiles or turn unmeasured events into successes.

Deterministic tests compare progressive file processing at 37/512/4096-frame
blocks, bounded one-slot queues, exact final sample positions, and identical
final pitches/onsets/offsets. They also compare the final single-tone output to
canonical offline YIN/spectral within explicit onset/offset tolerances. Legacy
offline YIN can fragment the single tone differently; that output contract is
not silently changed. This is a tested
fixture contract, **not** universal equivalence between full-context inference
and windowed models. Separate tests cover repeated notes, releases, overlapping
pitches, window seams, gaps, EOS, input epochs/sequences/layout, p99 and target
misses with simulated clocks, exactly-once finalization, stale revisions,
restart IDs, stereo ASIO-sink replay, cancellation/consumer failure, and manual
edit/deletion protection during refinement.

### Persistent-stream paced results

Windows/LLVM-MinGW Clang 23.1.1 Release, ONNX Runtime 1.20.1 CPU where applicable,
pinned Basic Pitch weights; synthetic v1, 22,050 Hz mono, 256-frame callbacks,
two-second context and unchanged pitch/segmentation thresholds. Normal cases
repeat all 16 clips twice (82.8 seconds audio); controlled overload runs once
(41.4 seconds), injecting 500 ms **per analysis step**, not in capture.
Runs were serial and source/binary hashes remained stable during each run.
The reports retain the measured executable hash; subsequent validation relinked
the runner, so its current file hash differs. Recorded live/DSP source hashes
still match. The later adapter-only change marks completed offline results
finalized; final complete suites were rerun afterwards. Do not treat a rebuild
as a byte-identical copy of the measured executable.
Reports are under `build-onnx-asio-mingw/evaluation/incremental/`, named
`yin`, `spectral-250`, `spectral-100`, `basic-pitch-speed`,
`basic-pitch-quality`, and `spectral-overload` (`.json` and `.md`).

All latency triples below are **worst per-clip p50 / p95 / p99, milliseconds**,
not pooled distribution percentiles. Each report retains every clip's summary,
sample count and maximum. Sparse note counts make many p95/p99 values identical;
these short synthetic replays are not sufficient for hardware/SLA guarantees.

| Engine / hop | Provisional latency | Stabilization latency | Event F1 | Note-derived frame F1 | Targets: provisional/stabilized |
|---|---|---|---|---|---|
| True incremental YIN | 71.2 / 71.2 / 71.2 | 71.3 / 83.4 / 83.4 | .696 | .806 | **Met observed** 200/300 ms |
| Spectral / 250 ms | 322.1 / 327.5 / 327.5 | 561.1 / 574.3 / 574.3 | .851 | .978 | **Met observed** 500/800 ms; not 200/300 |
| Spectral / 100 ms | 210.5 / 210.5 / 210.5 | 303.4 / 303.4 / 303.4 | .851 | .979 | **Missed** 200/300 ms |
| Basic Pitch speed / 250 ms | 348.4 / 412.7 / 412.7 | 600.1 / 664.3 / 664.3 | .894 | .985 | **Met observed** 500/800 ms; not 200/300 |
| Basic Pitch quality / 250 ms | 355.0 / 414.9 / 414.9 | 591.4 / 657.4 / 657.4 | .894 | .985 | **Met observed** 500/800 ms; not 200/300 |
| Spectral / 250 ms, +500 ms worker delay | 1080.1 / 1104.2 / 1104.2 | 1612.4 / 1614.1 / 1614.1 | .851 | .978 | **Missed** 500/800 ms |

YIN observed 44 provisional/44 stabilized events. Spectral/250 observed 48/46;
spectral/100 observed 56/46 with **8/56 provisional and 2/46 stabilization
target misses**. Each neural mode observed 46/46. Normal runs have eight
zero-observation clips (including silence) per stage, explicitly unmeasured,
not successful. Relaxed-target modes have zero observed misses; that does not
establish that undetected/quiet reference notes meet a target—the separate
recall/F1 exposes those omissions.

Default planning estimates at this block size are 72.6/132.6 ms for YIN,
362.6/612.6 ms for 250 ms windows, and 212.6/312.6 ms for 100 ms windows.
Actual inference and scheduling account for additional delay. All normal runs
had zero refused blocks, skipped samples, dropped events and overloads. Input
high-water was at most 4/256 blocks; event high-water at most 11/4096.
Normal worst-clip analysis p99: YIN 0.4 ms, spectral/250 28.9 ms,
spectral/100 39.7 ms, neural speed 101.5 ms, neural quality 113.3 ms.
Callback transport worst-clip p99 ranged 34.8-65.8 microseconds; the largest
normal callback sample was 285.5 microseconds. These measure application
transport replay, not the complete hardware callback/driver.

Controlled overload exposed 14 overloaded clips and 69 coalesced windows, with
no input refusal or sample/event loss. All 24 provisional and 23 stabilized
events missed 500/800 ms. Event F1 did not change in this probe, but latency
degraded sharply and final draining extended wall time to 54.35 seconds for
41.4 seconds audio. Consumer event-queue p99 reached 1054.3 ms (including final
drain); do not hide this behind good pitch accuracy. Deliberate skip-with-gap
behavior remains covered by deterministic stress tests, not this probe.

The faster spectral hop retained final event F1 on this corpus, without
shortening pitch context or suppressing quiet notes. Both neural modes produced
the same quality here: the default window session already uses canonical
event decoding. **No live-quality improvement is claimed** for changing that
label on these fixtures, nor are offline neural-strategy improvements
extrapolated to live audio.

### Verification of the persistent-input additions

| Configuration/check | Executed result |
|---|---|
| Dependency-light Release complete CTest | **192 passed, 2 skipped / 194** |
| ONNX CPU + ASIO Release complete CTest | **200 passed, 1 skipped / 201** |
| Model-free ASan/UBSan `[live],[engine],[asio]` | **9,494 assertions / 42 cases passed**, leak detection disabled |
| WebView2 GUI + ONNX + ASIO strict build | passed |
| Production clang-tidy / warnings-as-errors build | passed |
| Serial paced callback-replay evaluations | six configurations passed lifecycle/queue/duplicate checks; timing misses reported above, not gated away |

Complete suites include CLI timing validation, finalized JSON events, Unicode/
space paths, export semantics, Python scoring/evaluation and the existing
browser workflow tests. The model-free ONNX-only bounded-window test is skipped;
both builds skip the optional externally supplied real-audio smoke fixture.
No physical ASIO capture, GPU, Linux/MSVC, FFmpeg development integration or
hard-real-time/physical loopback latency was validated in this Windows
environment. No production pitch threshold was changed for these results.

## Limitations

* No bundled model is truly incremental. Basic Pitch and the spectral tracker
  use overlapping windows. Guard context, hop phase, confirmation and inference
  time all affect latency; their sum is a planning allowance, not a universal
  lower bound or a hard upper bound.
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
