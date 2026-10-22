# Pluggable transcription engines (contract v1)

## Architecture and incremental migration

The source already contained a versioned `ModelRegistry`/`ModelBackend`, the
`PitchActivationGrid` and frame-evidence representations, canonical temporal
segmentation, and `LiveTranscriptionSession`. The engine layer composes those
components; it does not add competing DSP, ONNX, ring-buffer, or note-stitching
implementations.

```text
WAV / FFmpeg / ASIO acquisition (outside engines)
             |
       mono AudioBuffer / timestamped live AudioFrameBlock
             |
       EngineRegistry -> TranscriptionEngine
             |                 |
       ModelBackend       optional IncrementalEngine
       YIN / spectral     incremental YIN / bounded window session
       / ONNX                   |
             +---- EngineUpdate + diagnostics ----+
                              |
               interpretation / canonical score / UI
                              |
                        exports / playback
```

- **Acquisition:** existing decoders choose and report the source sample rate.
  Engines accept decoded mono buffers or explicitly laid-out interleaved live
  blocks (downmixed by ingestion); they do not open files or run inference on ASIO
  callbacks. Adapter analysis/model-rate resampling remains inside the existing
  DSP/model contract. Nothing silently changes the source clock.
- **Pitch activity/inference:** existing model adapters retain their tensor,
  provider, cache, activation-semantics and temporal-alignment validation.
- **Segmentation:** by default the adapter's current `Note` vector is preserved
  exactly. Opting into `EngineConfiguration::segmentation` invokes the existing
  canonical decoder over retained frame evidence. Configuration validation uses
  the existing temporal-decoder validator.
- **Musical interpretation/export:** remain separate. Engines never assign
  tempo, voices, spelling, strings/frets, UI selection or playback state.
- **UI:** receives owned updates, not internal mutable inference buffers.
  The GUI's full-buffer built-in backend uses the engine layer. Its existing
  bounded WAV preview path and automatic legacy ONNX model detection remain
  compatibility paths, not duplicate engine algorithms.

The CLI's full-buffer explicit `--backend` path now uses engine instances.
All existing flags and export formats remain. The default free-function paths,
`--model` automatic adapter detection, ASIO capture, `--live-pipeline`, and
`--bounded-inference` retain their existing behavior. In particular, bounded
file decoding still uses `infer_wav_file` and is **not** disguised as stateful
engine input. This is an incremental migration, not removal of public APIs.

## Interfaces and capability metadata

[`engine.hpp`](../include/transcribe/engine.hpp) defines:

- `TranscriptionEngine`: `initialize`, versioned `configure`, offline
  `transcribe`, `cancel`, `diagnostics`, and idempotent `shutdown`.
- `IncrementalEngine`: an optional `live::AudioInputSink` interface returned by `incremental()`, with
  `begin`, bounded single-producer `push`, manual `pump`, `poll`, and `finish`.
  Implementations without streaming return null. Window processing is a
  different advertised mode from true sample-incremental processing.
- `transcribe_wav_progressive`: bounded WAV decoding into one persistent
  incremental engine, with cancellation, lossless backpressure, owned update
  callbacks and exact EOS flushing. `capture_asio_stream` accepts the same sink
  on the existing bounded ASIO transport. See [live input contracts and latency
  budgets](live.md#shared-engine-input-and-progressive-files).
- `EngineRegistry`: factories, discovery, explicit creation, and
  capability-aware selection with per-candidate rejection reasons.

| Adapter | Objective/range (MIDI) | Incremental mode | Available evidence |
|---|---|---|---|
| `builtin:yin` | Single-pitch, 36-83 | True sample-incremental YIN | Offline selected-pitch/voicing evidence; live note revisions |
| `builtin:spectral` | Independent multi-pitch, 28-96 | Overlapping offline windows | Offline fixed-pitch salience/activation grid; live note reconciliation |
| `onnx:basic-pitch-v1` | Independent multi-pitch, 28-96 | Overlapping offline windows, **not stateful neural streaming** | Note/onset/contour heads when requested/configured through the existing adapter |
| `onnx:legacy-pitch-v1` | 28-96, training objective initially unspecified | None | Explicit legacy tensor contract; optional selected-pitch or independent-pitch evidence |

Model definitions are registered when an ONNX path is supplied. Discovery lists
configured factories, not every conceivable downloadable model. A missing
requested ID is reported as unregistered; missing model files and unavailable
providers appear in candidate reasons. A model with many pitch columns is
**not** inferred to have a polyphonic training objective. Generic multi-pitch
use requires an explicit objective in model metadata or configuration.

Capabilities describe:

- Declared instruments/scope, monophonic operation, polyphony and pitch range.
  Built-in instrument values mean available **priors**, not recognition or
  accuracy guarantees. Neural adapters declare only unconstrained input
  (`InstrumentPrior::None`) unless a separate plugin provides richer metadata.
  A requested instrument-specific neural guarantee is rejected rather than
  silently ignored. Spectral can analyze monophonic material but selecting it
  does not promise exactly one output pitch.
- Input minimum sample rate, optional exact-rate list, and the model's distinct
  analysis rate. Empty rate lists mean any input rate above the minimum; they
  do not remove Nyquist/bandwidth limitations.
- Streaming mode, offline versus live activation availability, heuristic versus
  model-head onset/offset support, and provider choices.
- The existing complete `ModelMetadata`: window/hop/overlap, tensor heads,
  alignment, trimming, activation semantics, objective and provenance.
  After loading, onset-head capability is refined from actual tensor names;
  a note-head-only compatible model does not advertise a nonexistent onset head.

Selection is deterministic: the first registered candidate satisfying the
request wins; other candidates remain in the response with explanations. It
does not benchmark/rank quality, infer an instrument, download a model, silently
choose another provider, or load a session. “Available” is a **preflight**:
`initialize` must still validate tensor compatibility and allocate runtime
resources. Built-ins always require CPU, irrespective of optional ONNX builds.

```cpp
transcribe::EngineRegistry engines(model_path); // {} for model-free operation
transcribe::EngineRequirements request;
request.polyphonic = true;
request.sample_rate = audio.sample_rate;
request.provider = transcribe::OnnxExecutionProvider::Cpu;
auto selection = engines.select(request);
// Check selected_id; present selection.reasons and each candidate.reasons/explanation.
// To require true streaming, set request.streaming = EngineStreaming::Incremental.
// No spectral/ONNX candidate will then be mislabeled as incremental inference.
```

## Results and authoritative units

`EngineUpdate` is owned by its recipient. It has a monotonically increasing
sequence per initialized engine, provenance, source sample rate, and a complete
current note snapshot. Offline `notes` preserves the existing export-compatible
projection; live open notes are represented only in `events`.

`EngineNoteEvent` uses:

- Fractional MIDI pitch in semitones.
- Integer `onset_frame` and optional **exclusive** `offset_frame` in the source
  sample-rate clock. Missing offset means still sounding, never a fabricated
  duration. `onset_seconds()` and `offset_seconds()` divide by that declared
  rate and reject unknown sample rates. Legacy note times are rounded to the
  nearest source frame (at most half a frame of projection error).
- Explicit provisional/stabilized/rejected lifecycle, revision, `finalized` and truncated
  onset, where supplied by the live backend.
  `NoteOff` remains revisable; `NoteFinalized` ends live revisions, not manual
  edits or reviewed refinement. Input blocks carry explicit layout, sequence,
  source sample timestamps and epoch. Distinct takes must not reuse IDs in a
  shared consumer document.
- Engine/model ID, model-contract version, source/license, algorithm/method and
  audio-source identity. Heuristic strength stays separate from optional
  calibrated confidence. Current adapters do not invent calibrated confidence.

Retained `FrameEvidenceTrack` timestamps and `PitchActivationGrid` timestamps,
window and hop are **seconds relative to recording sample zero**, not beats,
milliseconds, device-clock time, or window-centered times. Existing model
alignment metadata remains attached to the engine. Rich heads/raw values stay
available without forcing every engine to produce them.

For YIN and monophonic legacy output, a requested dense grid is explicitly a
**selected-pitch projection**, labeled in `method`, with monophonic objective
and heuristic strength semantics. Unselected bins are zero in that projection;
they are not independent negative model evidence. Spectral and polyphonic model
grids retain their original semantics. Optional segmentation retains accepted
and rejected events, with event evidence.

Live updates additionally retain the existing `EventBatch`: revisions,
retractions, analysis gaps and overflow/resynchronization instructions.
`poll_events_with_snapshot` takes events and snapshot under the same publication
lock; the common event snapshot is built from that batch, not a later competing
view. Streaming activation grids are not currently implemented: capability
selection rejects them, and starting a stream with retained evidence/callbacks
fails explicitly.

## Lifecycle, cancellation and errors

Operations use the existing typed `InferenceResult<T>` /
`ModelOperation` and `InferenceErrorCode`; no success-shaped empty error result
or heavyweight plugin framework is introduced.

```text
Created / Shutdown --initialize--> Ready --configure--> Ready
Ready --transcribe--> Processing --success--> Ready
Ready --begin--> Streaming --finish--> Finished
active operation --cancel--> Cancelled
load/execution failure --> Failed
any state --shutdown--> Shutdown
```

Invalid/unsupported configuration and incorrect-state calls return errors
without destroying a ready engine. Failed/cancelled/finished streams require
explicit shutdown/reinitialize; completed offline calls retain the initialized
model for subsequent requests. `configure` changes options only in Ready state.
Provider/device/preview/session-cache settings belong to initialization and
require shutdown/reinitialization to change.

`EngineConfiguration::version` and `EngineMetadata::contract_version` are v1;
unsupported versions fail explicitly. The embedded `ModelDefinition` retains
the existing `model_contract_version == 1`. These are C++ API schema contracts,
not a new competing config-file parser; existing CLI/config-file parsing is
unchanged. Reconfiguration validates ratios, polyphony, requested capabilities
and optional temporal parameters before accepting the new configuration.

Instances have **one owner**. Initialization, configuration, offline inference,
polling, manual pumping, finishing, diagnostics and destruction are
non-reentrant owner-thread operations. `cancel()` can be called concurrently
with an active operation; it requests cooperative cancellation and never joins
its own inference worker. In live mode a nonjoining session cancellation
request stops ingestion/inference. The owner subsequently polls, finishes or
shuts down to join workers and release resources. Do not race initialization
or destruction with external cancellation calls.

`push` alone is the existing bounded single-producer callback interface:
no inference, locks, disk I/O or new allocations. Never invoke initialization,
poll, pump, finish or shutdown from capture callbacks. Quiesce the producer
before finish/destruction. Cooperative cancellation does not promise immediate
interruption inside an arbitrary external model/runtime call.

For streams, pass `LiveOptions`, including bounded input/event/note capacities,
overflow policy, notification hook and manual/threaded choice. `begin(options,
first_frame)` explicitly declares the device-frame origin; output frame zero
is that origin, while input gaps remain visible. `finish(end_frame)` seals the
known device-frame end, including dropped trailing frames. Do not double-add
the device origin to output events.

`LiveOptions::yin` controls live YIN confidence, latency and stabilization;
offline segmentation, instrument-prior and preview settings are not silently
applied to live YIN. Unsupported prior/preview/segmentation requests are
rejected. Window mode uses configured inference/segmentation options and the
existing reconciler. `LiveOptions::max_recording_seconds` remains a bounded
retention option; set it to zero when no recording is needed.

Diagnostics preserve lifecycle/error, backend/provider/session information
and live latency/queue/overload counters. When ownership of a model moves into
the live window session, model resource counters are explicitly an
initialization snapshot; evolving window counters are in `live_metrics`.
Resource/diagnostic reads are not capture-callback operations.

## Adding an engine

The runnable [reference plugin](../examples/engine_plugin.cpp) registers
`example:yin`. It delegates DSP to the existing backend and changes identity/
provenance only, demonstrating registration rather than inventing a toy
transcription algorithm.

```powershell
cmake --build build-tests --target transcriber-engine-example --config Release
ctest --test-dir build-tests -C Release --output-on-failure -R engine_plugin_example
```

There are two extension levels:

1. **Existing offline/model contract:** implement `ModelBackend` (metadata,
   load/release, infer, resources/diagnostics), then use
   `EngineRegistry::add_model(definition, factory)`. The shared engine provides
   lifecycle, canonical/legacy event projection, optional segmentation and
   cancellation. Factories must report the registered identity/version.
   The reference custom YIN ID is deliberately offline-only; registration must
   not secretly substitute the stock live YIN algorithm for a new plugin.
2. **Different/richer algorithm or true incremental state:** implement
   `TranscriptionEngine`, optionally implement `IncrementalEngine`, return that
   interface from `incremental()`, and register complete metadata plus a factory
   with `EngineRegistry::add`. Preserve owned result/lifecycle/error contracts,
   declare unsupported outputs honestly, and implement your incremental
   algorithm rather than inheriting an offline-window fiction.

No global service, shared mutable singleton registry, dynamic library loader,
virtual audio decoder, dependency injection container or runtime download is
required. Plugin discovery here is in-process factory registration; binary
plugin ABI compatibility is not promised. Future specialized outputs can extend
optional interfaces without changing acquisition, score or UI ownership.

## Validation

`tests/engine_tests.cpp` checks selector explanations, provider/range/rate/
instrument/streaming rejection, version/factory errors, state transitions,
repeated inference, exact legacy-note equivalence, optional canonical
segmentation, sample units/provenance, live origins/snapshots and cancellation
from an inference callback. ONNX builds additionally compare Basic Pitch notes
against direct inference and retain aligned onset/activation heads. The
reference executable is also a CTest.

```powershell
ctest --test-dir build-tests -C Release --output-on-failure -R "engine|cli_export_validation"
```

This validates adapter contracts, not improved transcription accuracy, equivalent
offline/live segmentations, GPU performance or hardware ASIO latency. Existing
evaluation scripts and baseline gates remain authoritative for quality.

### Measured validation

The final implementation was built on Windows with LLVM-MinGW Clang 23.1.1,
Python 3.12.10 and, for optional model tests, ONNX Runtime 1.20.1 CPU.

| Configuration/check | Executed result |
|---|---|
| Dependency-light Release, warnings as errors, full CTest | 185 passed, 2 skipped out of 187; 121.14 seconds |
| ONNX CPU + ASIO Release, warnings as errors, full CTest | 193 passed, 1 skipped out of 194; 207.08 seconds |
| Debug ASan/UBSan, engine and live selections | 4,398 assertions passed in 28 cases; leak detection disabled |
| Production-only dependency-light Release | Build passed |
| Windows WebView2 GUI + ONNX + ASIO, warnings as errors | Build passed |
| Production clang-tidy/static-analysis build | Passed without reported warnings |

Both full suites include the reference plugin, CLI/export validation, existing
evaluation gates and headless UI workflow. The model-free suite skips the
ONNX-only bounded Basic Pitch case; both suites skip the optional real-audio
smoke test because its external fixture is unavailable. The ASIO-enabled build
does not establish hardware capture correctness. Linux, MSVC, GPU providers,
hardware ASIO and native GUI startup were not exercised in this environment.
FFmpeg integration remains unvalidated locally because its development
configuration requires unavailable `pkg-config`.
