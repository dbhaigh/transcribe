# Model inference contract

The [engine layer](engines.md) composes this interface with versioned engine
configuration, capability selection, common result updates and optional live
input. `ModelBackend` remains the lower-level inference adapter; its existing
public contracts remain backward-compatible.

For explicit neural speed/quality selection, per-head geometry, pinned acquisition,
validation-only confidence calibration and measured note-event results, see
[Neural transcription strategy](neural-strategy.md).

For opt-in bounded WAV windows, per-instance result caching, cancellation and
explicit non-streaming limitations, see [Long recording pipelines](streaming.md).

For the current frame-by-pitch representation, explicit objective declarations,
aligned Basic Pitch onset/contour processing, and real-data benchmark tradeoffs,
see [Polyphonic activation and model adapters](polyphonic.md).
The inspection and measurements below describe the original adapter baseline
unless explicitly stated otherwise.

## Existing implementation (inspected before refactoring)

The legacy entry points are `transcribe_monophonic` and `transcribe_with_onnx`
in [transcriber.cpp](../src/transcriber.cpp). They return the existing `Note`
representation and optionally deliver frame evidence. Neither arbitrary ONNX
files nor short neural windows imply a streaming model.

### Bundled Basic Pitch

The bundled file is [basic_pitch.onnx](../python/models/basic_pitch.onnx),
SHA-256 `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`.
Its upstream Basic Pitch attribution and Apache-2.0 license are retained in
[LICENSE-basic-pitch.txt](../python/models/LICENSE-basic-pitch.txt).
See [python/README.md](../python/README.md) for provenance and evaluation limits.

The file actually declares float32 input `serving_default_input_2:0` with
shape `[symbolic_batch,43844,1]`; the adapter executes `[1,43844,1]`.
Audio is linearly resampled to 22050 Hz; batches are one, not arbitrary.
The file's three float32 outputs are `StatefulPartitionedCall:2`
`[symbolic_batch,172,88]`, `StatefulPartitionedCall:1`
`[symbolic_batch,172,88]` and `StatefulPartitionedCall:0`
`[symbolic_batch,172,264]`. The adapter consumes the note head named
`StatefulPartitionedCall:1`, which must return `[1,172,88]` at runtime,
representing MIDI 21-108 at 256-sample frame steps.
The engine adapts only MIDI **28-96**. The baseline did not use onset or contour
heads. The current canonical decoder retains/uses onset and note state, and
`OnnxOptions::aligned_model_heads` additionally enables jointly aligned contour
evidence and corrects multi-window time drift. This still does not supply
instrument identity, velocity or calibrated correctness confidence.

The adapter prepends 3840 zero samples, advances windows by 36164 samples,
discards 15 frames at each edge, retains 142 frames per window and trims the
stitched activation sequence using the existing length formula. The 7680-sample
overlap and 172 output frames are model-specific, not a general ONNX contract.
The 36164-sample window advance differs from 142 * 256; this refactor must not
silently change its established timestamps. The entire audio, padded audio and
stitched activity are retained: this is **windowed offline inference**, not
bounded-memory or stateful incremental inference.

### Existing generic ONNX path

This is a specific historical pitch-class adapter, not an arbitrary-model
loader. It resamples the full input to 16000 Hz, sends float32 `[1,samples]`,
and accepts float32 `[1,frames,69]` or `[1,frames,70]` outputs at an assumed
10 ms frame step. Classes 0-68 map to MIDI 28-96; optional class 69 is silence.
The first output is used for mono; the second may be selected for polyphony
if its shape matches. Mono logits use softmax; polyphonic logits use sigmoid.
`independent_probabilities` instead clamps values to [0,1]; none of these
operations calibrates correctness confidence. The file does not itself prove
the assumed sample rate, labels, frame step or activation semantics.

The pluggable API will therefore require explicit selection of a supported
contract rather than infer all semantics from tensor dimensions. Other tensor
layouts/rates/ranges require another registered adapter, not shape guessing.

### Built-in trackers

YIN normally analyzes 2048-sample windows at 11025 Hz with 256-sample hops;
preview uses 1024 at 8000 Hz with 512-sample hops. It is monophonic.
Its lag search is approximately 65-1000 Hz: although the old estimator's
final guard permits MIDI 28-96, actual normal/preview search output rounds
to MIDI **36-83**. The new metadata reports that narrower capability rather
than advertising the entire guard range.
Spectral tracking uses 4096/256 at 11025 Hz (preview 2048/512), adapts MIDI
28-96 and selects up to six pitches by default. Both preprocess and resample
offline audio; their normalized strengths/voicing are heuristics, not calibrated
probabilities. Existing realtime YIN has its own state/stabilization API and
must not be advertised as identical to incremental use of an offline adapter.

### Providers and lifetime before this change

CPU is the default. CUDA, DirectML and ROCm require both build-time enablement
and a compatible ONNX Runtime distribution/device. The existing session cache
keys path/provider/device, holds sessions indefinitely and constructs sessions
while holding its mutex. Graph execution may partition onto CPU even when an
accelerator is requested. Selecting an accelerator is not evidence that every
node actually runs there. Existing GUI/CLI exception boundaries report model
errors; preserve them and expose actionable errors to new API consumers.

## Versioned pluggable API

[inference.hpp](../include/transcribe/inference.hpp) defines source contract
version **1**. It exposes no ONNX Runtime headers and adds no dependency to the
standard-library-only build. This is a C++20 **source API**, not a stable C ABI
for loading arbitrary binary DLL plugins. Rebuild consumers when public
aggregate layouts change.

`ModelRegistry` owns definitions and factories, not live model sessions:

- `discover()` lists registered IDs, contract versions, paths, provenance and
  license declarations without loading weights. Construct with a model path to
  also register the explicit `onnx:basic-pitch-v1` and
  `onnx:legacy-pitch-v1` adapters. A path alone is not evidence of its semantics.
- `create(id)` returns a backend or an explicit `InferenceError`. Its
  `metadata()` describes capabilities before loading; after loading, ONNX
  metadata includes actual tensor names, element types and declared dimensions
  (`-1` denotes a dynamic/symbolic dimension).
- Metadata distinguishes accepted source rates (mono, any integral rate at
  least 2000 Hz) from the internal analysis rate, supported MIDI range,
  polyphony, input window/advance/overlap, output frame step, batch size,
  activation semantics and offline/incremental support. Capability bounds are
  not an accuracy guarantee. Spectral/ONNX allow up to 69 pitch candidates;
  existing default requests still select at most six.
  Current metadata also declares objective, edge trimming, temporal alignment
  and head semantics. `LegacyPitchV1` no longer advertises polyphony solely from
  tensor dimensions: a definition or request must explicitly declare an
  independent multi-pitch objective before a polyphonic request is accepted.
- `load(options)` selects the provider, device and built-in preview variant.
  Loading replaces/releases the old session first; failure leaves the instance
  unloaded, not secretly using the previous model. Loading is synchronous.
- `infer(audio, request)` returns the existing notes plus optionally retained
  frame tracks, without introducing score, rendering or exporter dependencies.
  Model ID selects the built-in algorithm; YIN explicitly rejects a polyphonic
  request. Preview is selected at load, not silently switched per request.
  ONNX request provider/device must match the loaded session.
  Retained aligned fixed-pitch evidence additionally supplies an optional
  `PitchActivationGrid`; unknown objective semantics are never inferred from
  its number of columns.
- `release()` is idempotent and destroys that instance's session. New instances
  bypass the legacy global session cache; they do not retain sessions by path.
  A file replaced on disk is observed when loaded again. The legacy entry
  point's process-wide cache is preserved for existing callers and is separate.
- `warm_up(request)` really infers two seconds of zero audio (at least one
  complete model window), using the loaded instance and requested settings.
  Warmup counts as a completed inference; it is never secretly included in a
  purported cold run.

```cpp
#include "transcribe/inference.hpp"
#include <stdexcept>

transcribe::ModelRegistry registry;
auto created = registry.create("builtin:spectral");
if (auto* error = std::get_if<transcribe::InferenceError>(&created)) {
    throw std::runtime_error(error->message); // Or display/log at the application boundary.
}
auto model = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
auto loaded = model->load();
if (auto* error = std::get_if<transcribe::InferenceError>(&loaded)) {
    throw std::runtime_error(error->message);
}
transcribe::InferenceRequest request;
request.retain_evidence = true;
auto result = model->infer(audio, request);
if (auto* error = std::get_if<transcribe::InferenceError>(&result)) {
    throw std::runtime_error(error->message);
}
auto output = std::move(std::get<transcribe::InferenceOutput>(result));
// Existing exporters consume output.notes unchanged.
// The separate canonical decoder can consume output.evidence.
model->release();
```

Neural discovery/loading can instead use:

```cpp
transcribe::ModelRegistry registry("python/models/basic_pitch.onnx");
auto created = registry.create("onnx:basic-pitch-v1");
```

For a catalog with audited provenance, call `add_onnx(ModelDefinition)` yourself:
it requires an explicit known contract, nonempty path, provenance and license.
Registration never downloads weights, guesses training data or asserts that a
license declaration has been independently verified. Convenience path
registration explicitly says to verify the supplied model's license.

### Cancellation, errors, diagnostics and ownership

Operations return `std::variant<value, InferenceError>`; errors carry a code and
actionable message. Codes distinguish invalid arguments, unsupported operations,
unloaded state, cancellation, load failure and inference failure. The adapter
boundary converts standard/ONNX exceptions to these explicit errors and retains
the message in diagnostics; errors do not become empty successful notes.
Application callback exceptions also become reported operation failures.
The legacy CLI/GUI retain their existing exception-reporting boundaries.

Instances are single-owner, non-reentrant and not safe for concurrent
load/infer/release. Registry registration is likewise serialized by its owner.
Independent instances may be scheduled by an application, but ONNX sessions
use one intra-op thread to avoid automatic nested oversubscription. The existing
built-in parallel option is preserved. None of these APIs belong on a capture
callback or implicitly dispatch UI updates.

`InferenceRequest.cancellation` is the authoritative `std::stop_token`.
YIN checks between analysis frames, spectral tracking between salience frames,
and Basic Pitch between windows. During an ONNX `Run`, a stop callback requests
termination on that run's private `RunOptions`; it does not poison the session
for the next inference. Preprocessing, resampling, spectrogram construction and
session loading are not fully interruptible. Cancellation is cooperative, not
a hard realtime latency guarantee.

The top-level progress callback is synchronous and monotonic, from zero to one
on success, with coarse backend-specific stages. Existing analysis/ONNX
callbacks remain available; retain/copy borrowed evidence if needed. Callbacks
may already have observed evidence before cancellation; a failed operation
does not return a successful partial transcription. Marshal callbacks to a UI
thread at the application boundary.

`resources()` reports loaded state, owned live session count, model-file bytes,
last successful input bytes/evidence-frame count, completed inference count,
load wall time and last successful inference wall time. `resident_bytes` is
explicitly absent: file size is **not** model memory usage, and portable exact
per-session RSS is unavailable. The benchmark separately measures process
peak RSS and CPU time. Shared DSP caches, ONNX arenas, allocator retention and
output vectors can outlive a single call; release is not a promise that process
RSS returns immediately to its initial value.

### Provider policy

`inference_providers()` distinguishes build enablement from providers advertised
by the **loaded** ONNX Runtime. Availability is not proof that a usable GPU
device exists; session construction must still succeed. Built-ins require CPU.
Diagnostics expose requested provider, active provider only after successful
load, device ID, runtime version and whether CPU fallback was disabled.

Accelerator session options explicitly set
`session.disable_cpu_ep_fallback=1`. This applies to the shared legacy loading
path too: a graph requiring unsupported GPU operations may now fail instead of
silently assigning them to CPU. Select CPU explicitly if that is intended.
No retry changes providers or backends. This intentional stricter failure
policy is not a transcription-quality or provider-speed claim.

### Extending the catalog

For a file with the existing supported tensor/semantic contract, register its
path, unique model ID, version, provenance and license using `add_onnx`.
For another rate, layout, label range, activation interpretation or genuine
incremental protocol, implement `ModelBackend` in a new adapter module and
register its factory with `ModelRegistry::add`. Compose shipped registrations
in [inference.cpp](../src/inference.cpp), not in rendering or command-line
parsing. Factories must return a valid backend; null/throwing factories produce
explicit errors. New incompatible semantics require a new contract/version,
not reinterpretation of an existing ID.

The opt-in CLI route is generic:

```powershell
.\build-release-mingw\transcriber.exe .\samples\single_a4.wav `
  --backend builtin:spectral --json notes.json

.\build-onnx-asio-mingw\transcriber.exe .\samples\single_a4.wav `
  --backend onnx:basic-pitch-v1 --model .\python\models\basic_pitch.onnx `
  --polyphonic --provider cpu --json notes.json
```

The `backend` config-file key is also supported. A newly composed registry
factory is selectable through the same `--backend <id>` branch, without new
CLI/exporter/rendering logic. Existing invocations **without** `--backend`
retain their current streaming-WAV/ONNX paths and output shapes. The opt-in
backend route is full-buffer offline processing, including for WAV; it is not
a replacement for the default bounded-audio-buffer WAV path.

### Incremental inference is deliberately unsupported here

All three shipped adapters report `incremental=false` and return an explicit
unsupported error from `infer_incremental`. Offline YIN/spectral frames depend
on full-clip preprocessing/onsets, Basic Pitch stitches window edges, and the
legacy generic model consumes the full input. Pretending independent window
calls are equivalent incremental inference would change boundary behavior.
The virtual incremental operation exists for future adapters that actually
implement state ownership, continuity, finalization and sample-time guarantees.
The separate existing `RealtimeTranscriber` remains available and unchanged.

## Reproducible quality and performance evaluation

Use [the existing framework](evaluation.md) and add `--mode full
--model-interface`. For ONNX explicitly select `--model-contract
basic-pitch-v1` or `legacy-pitch-v1`; unlike the legacy entry point, this route
does not guess the model contract from shape.

```powershell
python .\python\evaluate.py `
  --runner .\build-onnx-asio-mingw\transcriber-evaluate.exe `
  --build-info .\build-onnx-asio-mingw\evaluation-build-Release.json `
  --backend onnx --model .\python\models\basic_pitch.onnx --provider cpu `
  --model-interface --model-contract basic-pitch-v1 --mode full `
  --output-json .\build-onnx-asio-mingw\evaluation\interface-onnx.json

python .\python\performance.py `
  --runner .\build-onnx-asio-mingw\transcriber-performance.exe `
  --build-info .\build-onnx-asio-mingw\evaluation-build-Release.json `
  --model-interface --onnx-model .\python\models\basic_pitch.onnx `
  --provider cpu --verify-legacy `
  --output .\build-onnx-asio-mingw\evaluation\model-interface-performance.json
```

Both use the same deterministic ground-truth corpus. The performance runner
adds low notes, dense chords, 90-second silence and a 90-second chord: 20 clips,
60 backend workloads, 233.410 audio seconds per backend. Private recordings
are not needed or included. Four in-process runs per workload separate the
first inference after load from three warm runs; session load time is recorded
separately. All four runs must produce identical notes. `--verify-legacy`
additionally compares every workload with a separate legacy full-buffer
invocation, including pitches, timestamps and heuristic strengths.

For a GPU-enabled build/runtime, rerun both commands with the explicit provider
and compare against the corresponding CPU corpus report. Do not change the
provider in metadata after a failure. No GPU package is installed automatically.

### Neural execution choices investigated

- The ONNX file declares a symbolic batch axis, but this adapter's stitching
  and runtime output validation support batch **one** only. Larger batches are
  neither exposed nor benchmarked as if supported. Tests reject fixed batch-two
  model contracts. A batching experiment needs its own alignment/quality tests.
- Basic Pitch keeps its established 7680-sample overlap, zero padding, 36164
  window advance and 15-frame edge discard. No guessed overlap or timing
  "correction" changes existing note output in this refactor.
- The Basic Pitch waveform vector and float input `Ort::Value` wrapper are
  reused across windows within one inference. Sessions persist across calls;
  per-input waveform/activation/output buffers do not. The old redundant
  full-input 16 kHz conversion is no longer performed before the Basic Pitch
  22.05 kHz path. Output tensors are still allocated by ONNX Runtime.
- Full-input generic inference and full-result Basic Pitch memory remain
  explicit limitations, not hidden by a misleading streaming label.

### Measured compatibility

The final Windows Clang 23.1.1 Release runs use the unchanged synthetic-v1
corpus: 16 clips, 24 reference notes, seed 2026, PCM16 mono at 22050 Hz.
Strict matching requires exact MIDI, onset within 50 ms and offset within
the larger of 50 ms or 20% of reference duration. Frame scoring uses 10 ms
steps and is reported separately. Each quality record has one discarded
fresh-process warmup and three measured fresh-process runs; these are not
persistent-session warm runs.

| Adapter | TP / FP / FN | Event precision | Event recall | Strict event F1 | Frame F1 |
|---|---|---|---|---|---|
| [YIN](../python/evaluation/synthetic-v1-windows-clang23-yin-interface-full.json) | 13 / 17 / 11 | 43.33% | 54.17% | 48.15% | 78.43% |
| [Spectral](../python/evaluation/synthetic-v1-windows-clang23-spectral-interface-full.json) | 16 / 80 / 8 | 16.67% | 66.67% | 26.67% | 81.01% |
| [Basic Pitch CPU](../python/evaluation/synthetic-v1-windows-clang23-onnx-interface-full.json) | 17 / 23 / 7 | 42.50% | 70.83% | 53.13% | 95.57% |

These preserve the legacy full-buffer results, including their limitations;
they are not the improved opt-in [temporal-decoder](decoding.md) results.
Regression tests compare the entire legacy and adapter note vectors and
metrics on the same corpus, not just rounded aggregate F1. Tests also compare
legacy and registry CLI ONNX exports. The separate temporal mode is unchanged;
the evaluator currently rejects combining its temporal and model-interface
experiments rather than silently selecting one.

Synthetic signals do not establish independent real-recording accuracy.
The historical GuitarSet result was not rerun; possible training-data overlap
remains unresolved. The supplied unlabeled recording passed existing smoke
tests, but has no ground truth and is not included in the public snapshots.

### Persistent-session CPU measurements

The [machine-readable performance record](../python/evaluation/model-interface-windows-clang23-cpu.json)
contains all 60 workloads, per-call timing/CPU/peak-RSS, source and binary
digests, compiler/build flags, model hash, actual provider/runtime, rates,
window geometry and note-event/frame metrics. The source is uncommitted;
digests identify it more precisely than the recorded base commit.

| Adapter | Cold inference total (s) | Warm inference total (s) | Cold CPU total (s) | Warm CPU total (s) | Max process peak RSS (MiB) | Warm audio throughput |
|---|---|---|---|---|---|---|
| YIN | 0.8160 | 0.8259 | 2.0313 | 1.9219 | 25.37 | 282.63x realtime |
| Spectral | 0.7982 | 0.4516 | 1.7656 | 0.4063 | 96.50 | 516.86x realtime |
| Basic Pitch CPU | 6.5808 | 6.5616 | 6.5781 | 6.5313 | 83.33 | 35.57x realtime |

Cold totals sum each clip's first invocation after load; warm totals sum
per-clip medians of the remaining three calls, not all three calls.
CPU totals use the corresponding first-call/per-clip-median convention.
Throughput divides 233.410 audio seconds by the warm inference total.
Full-buffer WAV decoding and session loading are excluded from inference.
Twenty independent Basic Pitch session loads totalled **1.2433 s**, reported
separately. ONNX Runtime was **1.20.1**, with one intra-op thread and batch one.
Every workload's emitted notes matched the legacy path exactly; repeated
calls were deterministic and owned live-session counts were zero after
release. Empty ONNX input was explicitly skipped, not inferred.

These are descriptive measurements, **not a before/after interface speedup
claim**. Spectral warm calls benefit from the existing content-keyed
spectrogram cache. OS caches were uncontrolled, Windows CPU accounting is
coarse for short calls, and process peak RSS includes caches and allocator
retention rather than isolated model memory.

CUDA, DirectML and ROCm were disabled in these builds and unavailable in the
loaded runtime; only CPU could execute. Unavailable-provider failure paths
were tested. No GPU inference, GPU speed comparison, batch-size scaling or
physical-device absence is claimed.

### Final validation

- Default dependency-light Release: **59/59 CTests passed**.
- Optional ONNX/ASIO Release: **63/63 CTests passed**.
- Both suites included the supplied external-recording smoke test; neither
  suite skipped that test in this final run.
- Full WebView2/ONNX/ASIO Release CLI and GUI rebuilt successfully. Interactive
  GUI use and physical ASIO hardware were not exercised.
- Tests cover discovery, versioning, lifecycle, resource accounting, warmup,
  evidence retention, provider mismatch/unavailability, invalid rates/options,
  nonfinite audio, cancellation and session reuse, exact legacy compatibility,
  and malformed ONNX tensor ranks, batches, types, classes, lengths and values.
- Python syntax checks and `git diff --check` passed. Existing Catch2
  `__COUNTER__` extension warnings remain; builds are not warning-free.
- Linux/macOS, alternate toolchains and GPU execution were not run here.

## Live and windowed use

The live pipeline drives any `ModelBackend` through overlapping windows; no
bundled model is treated as incremental. Whole-recording Basic Pitch inference
measured worse than its native 2 s windows on the synthetic corpus, so live
refinement defaults to windowed mode for `onnx:*` backends. See
[Live transcription](live.md).

## Dependencies and licensing

No new package, runtime, model, license or downloaded dataset is introduced.
The public interface uses only C++20; ONNX Runtime remains optional and is
MIT-licensed (the configured SDK/runtime version must match the deployed DLL).
CUDA/DirectML/ROCm SDKs and their own redistribution terms apply only when
explicitly enabled. The bundled Basic Pitch model is Apache-2.0 with Spotify
attribution and pinned upstream commit
`fa5997af0a8210982619003269994a1be25eddf3`; retain its adjacent license.
Possible training/evaluation-data overlap is unresolved.

There is no root repository license file in the inspected checkout; built-in
model definitions say **"No repository license declared"**, rather than invent
a license. Model registration is not a license grant. Verify repository and
model redistribution rights before packaging externally.

Tests use Catch2 as already configured and stdlib Python only. Tiny local
ONNX validation fixtures are encoded from standard protobuf field definitions
in [onnx_fixtures.py](../tests/onnx_fixtures.py); no ONNX Python package or model
download is required to test ranks, batch, types, pitch counts, lengths or
nonfinite outputs.
