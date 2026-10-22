# Long recordings, bounded windows and cancellation

## Status and compatibility

The existing full-buffer APIs and default CLI/model behavior are unchanged.
`infer_wav_file` is an **opt-in, overlapping offline-window pipeline**, not a
stateful or sample-exact streaming neural adapter. `ModelMetadata::incremental`
remains false for Basic Pitch. Its fixed-window training objective does not
establish equivalence between arbitrary independent chunks and a complete file.

| Path | Acquisition / working buffers | Retained output |
|---|---|---|
| `read_wav` / `read_audio` | Complete mono waveform; WAV reserves declared size once | Complete waveform |
| `read_wav_blocks` | Incremental, 4096-frame blocks in the new file pipeline | Caller-owned |
| Existing `transcribe_wav_streaming` | Bounded overlapping WAV chunks, built-in YIN/spectral only | Complete note vector |
| New `infer_wav_file` | Bounded WAV chunks; built-in trackers or an explicitly loaded BasicPitchV1 backend | Capped complete notes, or finalized callback batches only |
| FFmpeg `read_audio` | Packet decoding/conversion is incremental internally, but the public result still materializes the entire 44100 Hz mono waveform | Complete waveform |
| Existing Basic Pitch `infer` / free function | Complete source, resampled and padded copies, stitched activity; more arrays when evidence is requested | Complete notes, optional complete evidence |
| LegacyPitchV1 | Full-buffer contract only; new bounded path returns `Unsupported` before decoding | Complete notes |
| ASIO / live adapters | Existing capture ring and live-window machinery; unchanged | Recording retention depends on live/refinement options |

No new path silently changes the source sample rate. WAV remains at its declared
rate until each backend's documented analysis-rate conversion. FFmpeg's existing
44100 Hz output policy is unchanged. RF64 and files exceeding RIFF's 32-bit sizes
are not newly supported.

## Reproducing bounded transcription

Use a documented Release build, with ONNX enabled only if using Basic Pitch:

```powershell
cmake --build build-onnx-asio-mingw --config Release -j 4
.\build-onnx-asio-mingw\transcriber.exe input.wav --backend onnx:basic-pitch-v1 `
  --model python\models\basic_pitch.onnx --bounded-inference --polyphonic `
  --provider cpu --benchmark
```

The new switch requires WAV file input and an explicit backend. Built-ins also
work, for example `--backend builtin:yin --bounded-inference`. Generic models,
compressed input, capture and `--live-pipeline` are rejected rather than silently
falling back to a whole-file allocation. Existing exports are unchanged.

In the WebView workstation, **Bounded neural windows** selects the same pipeline
for a WAV file and a Basic Pitch model. It defaults off and reports incompatible
input/model contracts explicitly. Review evidence is delivered to the existing
bounded review collector, with absolute timestamps; full activation grids are
not retained by the file pipeline.

For embedding, load one `ModelBackend`, then call:

```cpp
transcribe::InferenceRequest request;
request.cancellation = stop_source.get_token();
request.onnx.polyphonic = true;
request.onnx.independent_probabilities = true;
transcribe::FileInferenceOptions options;
options.retain_notes = false;
options.notes_callback = consume_finalized_notes;
auto result = transcribe::infer_wav_file(backend, path, request, options);
```

The backend remains caller-owned and loaded after success, cancellation, a
callback exception, or a failed request. Instances remain single-owner and
non-reentrant. No background queue or thread is created by this API.
Errors are returned as `InferenceError`; callback batches delivered before an
error cannot be retracted, and must not be committed as a successful transcription.

### Parameters, ownership and limits

`FileInferenceOptions` defaults:

| Parameter | Default | Meaning |
|---|---:|---|
| `chunk_seconds` | 8 | Source-rate offline window; valid 2-30 s |
| `overlap_seconds` | 1 | Shared context; at least .5 s and less than chunk |
| `maximum_audio_bytes` | 8 MiB | Limit for one source chunk's float payload, checked before reserve |
| `maximum_pending_notes` | 4096 | Maximum unresolved cross-window notes |
| `maximum_retained_notes` | 1,000,000 | Maximum accumulated result notes; use callback-only mode for bounded result memory |
| `retain_notes` | true | Return complete, start/pitch-sorted notes |

These are **payload/count limits, not a process-RSS allocator**. WAV byte/mono
decode blocks, allocator overhead, one backend invocation's resampling/features,
evidence copies, and ONNX Runtime workspace are additional. Their sizes depend
on the bounded window, model and source format rather than recording duration.
This API does not promise that arbitrary ONNX Runtime internal allocations fit
inside `maximum_audio_bytes`. Allocation failures are reported, not retried as
successful empty transcriptions. Source chunks, pending notes and temporary
results are released on return. Completed return values and callback-owned
copies are the caller's responsibility.

Window/overlap sizes are rounded once to integer source samples. Every absolute
timestamp is derived from the integer source origin, not accumulated floating
durations. First/last windows retain their outer edge; interior ownership starts
and ends at half-overlap guards. An onset belongs to one core. Same-pitch
continuations starting in the left guard merge only when their intervals
actually overlap a pending event; there is no added gap-merging threshold or
confidence/quiet-note cutoff. Newly detected core onsets remain distinct.
Completed notes are published once their end precedes the next source origin.
Final short windows flush pending events, and exact-length input does not get
an extra overlap-only inference. Callback batches are completion-ordered, **not
globally onset-ordered**; a long sustained bass note can finish after melody notes.

Evidence callbacks include overlap context (not a unique global activation grid)
and absolute frame timestamps. Consumers doing frame metrics must establish
their own single-frame ownership rather than count overlapping callbacks twice.
Progress is monotonic and source-relative, with cancellation checks before
decoding, between blocks/windows, inside backend analysis/inference and after
callbacks.

Each offline chunk is independently filtered/resampled and Basic Pitch pads its
own windows. Thus context, resampling phase, onset/offset estimates and sometimes
event counts differ from full-buffer inference. This is **not exact neural output
stitching** or a new recurrent-model contract. Synthetic measurements are
descriptive and do not justify a default or accuracy promotion.

## Resource reuse and responsive interaction

* A file run loads no models: it reuses the caller's initialized session across
  all windows and subsequent requests.
* The legacy free-function ONNX session cache now retains at most two entries.
  Its identity includes absolute path, file size, modification time, provider and
  device. Eviction releases resources outside its mutex; outstanding callers
  retain their shared session. Owned backends retain their model until release.
  To force reloading a replacement with identical size and modification time,
  use a new/reloaded owned backend.
* An optional single-result cache on owned backends avoids **all** repeated
  preprocessing, resampling and inference for unchanged audio and configuration.
  Enable a payload budget with `ModelLoadOptions::maximum_result_cache_bytes`
  and opt in per request with `reuse_cached_result`. Both default off.
  Input samples/rate are compared, not merely hashed. The key covers current
  analysis/decoder options. Model/provider/device/preview are fixed by load.
  Reload/release destroys the cache. Inference still validates requests and
  checks cancellation on cache hits.
  Capacity-based accounting includes vector storage and conservative string
  storage. Replacing an entry uses a temporary bounded copy (transiently up to
  two cache entries), then releases the old allocation; a smaller entry does not
  retain a larger previous vector's capacity.
* Evidence retention, evidence callbacks, stage-timing callbacks or backend-local
  progress callbacks disable result reuse, preserving their side effects.
  Top-level request progress still receives start/end updates on hits. Oversize
  results are not cached; an existing bounded entry can remain available.
  `ModelResources` exposes cached payload and last-hit status.
* The existing spectrogram cache remains one entry with a 64 MiB retention limit.
  It is not an allocation limit for full-buffer analysis; use bounded input for
  that. No second FFT or resampling cache was added speculatively.
* File-analysis cancellation now reaches WAV block reading, DSP and ONNX calls,
  and the WebView Cancel button is available during file analysis. ASIO's normal
  **Stop capture** behavior remains distinct from discarding a file analysis.
* WebView file transcription progress, note previews and audio previews are
  latest-value/coalesced, rather than an unbounded accumulation of posted
  payloads. A slow interface may skip intermediate previews; the final score
  remains complete. There is at most one pending payload of each kind.
* Transcription messages carry a job identity, checked before UI/result mutation
  or joining a worker. Busy guards and disabled input/settings already serialize
  normal jobs; these are not evidence of a previously confirmed concurrent-job
  race. Job identities additionally reject late updates after completion. A
  cancelled file worker cannot commit a success after its final cancellation
  check. Project/refinement workers retain their existing revision guards.

FFmpeg cancellation uses an input interrupt callback and packet/frame checks.
Its full-waveform API remains non-bounded. Optional FFmpeg compilation/runtime
validation could not run locally because CMake could not find `pkg-config`;
this is a dependency/build limitation, not a passing test. No ASIO hardware or
interactive WebView cancellation session was exercised.

## Profiling and reproducible measurements

The existing performance runner and `python/performance.py` were extended, not
replaced. Before implementation, a 120 s, 8 kHz PCM16 A3/silence recording measured:

* Basic Pitch CPU: 3.073-3.076 s inference, 114.2-116.1 MiB process peak, model
  initialization .061 s.
* Spectral: .544-.546 s, 118.3 MiB peak. First-run stages: preprocessing .035 s,
  onset FFT .058 s, resampling .009 s, pitch analysis .441 s. This does **not**
  support prioritizing resampling micro-optimizations.

The full-buffer Basic Pitch source confirms complete resampled and padded copies
plus stitched frame-by-pitch arrays; only its fixed 43844-sample neural window is
intrinsically required by the adapter contract. WAV's complete result is required
by `read_wav`, not by WAV decoding. FFmpeg similarly accumulates a complete result
even though packets/converted frames are individually decoded.

Reproduce model-free measurements from a clean checkout using the normal
dependency-light test build:

```powershell
cmake -S . -B build-long -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-long -j 4
python python\performance.py --runner build-long\transcriber-performance.exe `
  --build-info build-long\evaluation-build-Release.json --long-recordings `
  --serial --warmups 1 --repetitions 3 --output build-long\long-pipeline.json
ctest --test-dir build-long --output-on-failure
```

The generator is bounded to one second of samples while writing fixtures. It
creates 6 s and 120 s inputs at 8 kHz, with a three-second A3 each minute.
Use `--long-seconds` to change the latter, `--chunk-seconds` to vary seams, and
`--hours 2` to add a two-hour input. Only bounded mode is run on multi-hour input;
the harness deliberately does not exhaust RAM by trying full-buffer neural
inference. Each mode/backend is a separate process. The existing runner can also
be called directly with `model`, `bounded`, or `cached`, and supports warmups via
`TRANSCRIBE_PERFORMANCE_WARMUPS`.

For Basic Pitch, configure the documented ONNX build and add:

```powershell
python python\performance.py --runner build-onnx-asio-mingw\transcriber-performance.exe `
  --build-info build-onnx-asio-mingw\evaluation-build-Release.json `
  --long-recordings --model-interface --onnx-model python\models\basic_pitch.onnx `
  --provider cpu --serial --warmups 1 --repetitions 3 `
  --output build-onnx-asio-mingw\evaluation\long-pipeline\short-long.json
```

JSON and companion Markdown report median/interpolated p95 timings, startup,
first completed window, real-time factor, throughput, process-lifetime peak RSS,
buffer payload peak, model initialization/provider/runtime, binary/model/source
identities, exact-repeat checks and full-buffer equality. Full/model timing
excludes initial WAV decoding; bounded timing includes it. Startup includes
decode where applicable and model initialization. Cached measurements exclude
stage callbacks and use a 16 MiB payload budget.

Per-input event precision/recall/F1, onset/offset errors and fragmentation remain
in the JSON; exact full-buffer prediction equality is reported separately.
Repeated runs are required to return identical notes. OS caches and other
machine workloads are uncontrolled. Small sample counts are not stable
regression thresholds. These fixtures measure memory scaling/consistency, not
real-world transcription accuracy.

### Measured results

Windows 11, LLVM-MinGW Clang 23.1.1, Release `-O3 -DNDEBUG`, ONNX Runtime 1.20.1,
CPU execution provider; the bundled Basic Pitch SHA-256 is
`2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`.
No GPU was measured. The final short/long run uses one warmup and three measured
invocations per row:

| 120 s input | Full median s | Bounded median s | Full peak MiB | Bounded peak MiB |
|---|---:|---:|---:|---:|
| YIN | .1731 | .1673 | 18.58 | 6.81 |
| Spectral | .5455 | .2573 | 118.36 | 18.13 |
| Basic Pitch | 2.9852 | 3.3845 | 114.39 | 39.83 |

Basic Pitch reduces peak RSS by about **65%**, at about **13% more wall time**
on this sparse fixture. Source audio payload peaks at 256000 bytes per bounded
run. The spectral gain benefits from reusing identical silence-chunk FFTs in
the existing cache; do not extrapolate it to continuously changing music.
Basic Pitch load takes .0584 s in bounded mode; its first completed file window
takes approximately .198-.203 s after load. Full/model startup including decode
and load takes .0640 s.

All six-second bounded predictions equal their respective full-buffer outputs
(one outer window). All 120-second bounded predictions differ from full-buffer
outputs. Repeated invocations within each row are identical. The opt-in cached
mode returns exactly the full-buffer predictions on both durations; 120-second
cache-hit medians are .00106 s YIN, .00123 s spectral and .00115 s Basic Pitch.
Its peak RSS still includes the initial full-buffer computation; it is not a
replacement for bounded decoding.

Accuracy differences are kept separate, using the shared existing scorer:

| 120 s input | Frame F1 full / bounded | Event P full / bounded | Event R full / bounded | Event F1 full / bounded | Extra fragments full / bounded |
|---|---:|---:|---:|---:|---:|
| YIN | .9819 / .9828 | 0 / 0 | 0 / 0 | 0 / 0 | 2 / 2 |
| Spectral | .8654 / .8841 | 0 / 0 | 0 / 0 | 0 / 0 | 2 / 2 |
| Basic Pitch | .9462 / .9916 | .2500 / .6667 | .5000 / 1.0000 | .3333 / .8000 | 2 / 1 |

The built-ins still fail strict event scoring despite high frame accuracy.
Basic Pitch's matched-note mean onset error **worsens** from .0101 to .0186 s,
while mean offset error changes from .0990 to .0615 s; matching populations
differ (one versus two onset matches). All merging counts are zero. None of
these tiny synthetic results establishes real-data accuracy improvement.
An additional diagnostic found existing noise sensitivity on an unquantized
float32 sustained sine: full-buffer YIN also fragmented it. The sustained-tone
seam regression uses PCM16-quantized samples; it does not claim to fix that
separate onset-detector limitation.

The final two-hour run uses two measured invocations and no excluded warmup:

| Backend | Median s | P95 s | RTF | Peak MiB | Retained notes |
|---|---:|---:|---:|---:|---:|
| YIN | 9.964 | 9.995 | .00138 | 7.27 | 240 |
| Spectral | 15.057 | 15.134 | .00209 | 19.76 | 1014 |
| Basic Pitch | 206.565 | 206.912 | .02869 | 39.91 | 155 |

Each run processes 1029 outer windows, with the same 256000-byte source payload
peak. Basic Pitch runs approximately 34.9 times faster than real time, and its
peak remains near the 120-second value. This verifies bounded **working audio**
on a practical multi-hour input, not constant-sized accumulated notes or the
performance of two hours of dense guitar: only 5% of this fixture is sounding.

Machine-readable and companion readable reports are generated locally at:

* [short/long JSON](../build-onnx-asio-mingw/evaluation/long-pipeline/short-long.json)
  and [Markdown](../build-onnx-asio-mingw/evaluation/long-pipeline/short-long.md);
* [two-hour JSON](../build-onnx-asio-mingw/evaluation/long-pipeline/two-hour.json)
  and [Markdown](../build-onnx-asio-mingw/evaluation/long-pipeline/two-hour.md).

These build-directory artifacts are intentionally untracked; the commands above
recreate them. The two reports identify the same final runner binary and check
that relevant source, model and binary hashes do not change during measurement.
Pre-change `before-onnx.json` and `before-spectral.json` in the same directory
are descriptive runner outputs without a preserved source/binary fingerprint,
not independently versioned regression baselines.

### Validation

* Model-free Release: **148 passed, 2 skipped**, 150 registered CTest entries.
  Skips are Basic Pitch without ONNX and the optional external-audio smoke.
* ONNX/ASIO Release: **155 passed, 1 skipped**, 156 registered CTest entries.
  The external-audio smoke is skipped; no hardware capture claim is made.
* WebView2/ONNX/ASIO build succeeds.
* Bounded CLI MusicXML, tablature and JSON exports were generated and parsed;
  missing-backend usage fails explicitly.
* Regressions exercise three source rates, nonintegral overlap rounding, exact
  and short final windows, leading/trailing events, repeated pitches, concurrent
  pitches, sustained continuation, absolute evidence times, callback-only
  output, memory/note budgets, injected callback/model failure, cancellation,
  deterministic replay and session/cache cleanup/reuse.
* WAV physical data size is checked before callbacks advertise/reserve a declared
  whole-file allocation; a truncated-large-declaration regression verifies that
  no blocks are published first.
* Browser bridge smoke exercised the actual UI scripts with a mocked host:
  bounded-option submission, disabled settings during file analysis, usable
  Cancel analysis, cancellation message dispatch, no cancel affordance for
  non-cancellable exports, and disabling bounded mode without ONNX. No page
  errors occurred. This is not a native engine/GUI timing or hardware test.
