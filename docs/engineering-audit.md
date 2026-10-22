# Engineering audit

## Current audit: 0.1.5 baseline

Audit date: 2026-10-21. Source baseline: `1528f91` (Transcriber 0.1.5).
The worktree was clean before this documentation update. No application
implementation was modified. This current audit supersedes the historical
0.1.4 findings preserved below.

This is a source-level engineering and correctness audit, not a security
penetration test, a hardware qualification, or a claim of commercial-grade
automatic music transcription. Severity ranks user impact and remediation
priority. Evidence labels distinguish **Reproduced** behavior,
**Source-confirmed** behavior not exercised at runtime, **Risk** paths
supported by source but not reproduced, and **Design gap** items that are
limitations or assurance/documentation gaps rather than established defects.

### Execution paths and subsystem boundaries

| Surface | Current execution path and boundary |
|---|---|
| WAV input | The CLI uses `transcribe_wav_streaming` for ordinary WAV requests; it calls bounded `read_wav_blocks` and processes overlapping chunks. The library `read_wav` adapter instead accumulates those blocks into a full `AudioBuffer`. See [audio_file.cpp](../src/audio_file.cpp#L99) and [transcriber.cpp](../src/transcriber.cpp#L1977). |
| Compressed audio | With `TRANSCRIBE_ENABLE_FFMPEG`, `read_audio` dispatches non-WAV input to `read_audio_ffmpeg`, which decodes/resamples to a full mono `AudioBuffer` at 44.1 kHz. FFmpeg is an optional development-library dependency, not merely a command-line executable. See [ffmpeg_audio.cpp](../src/ffmpeg_audio.cpp#L69) and [README optional integrations](../README.md#L315). |
| YIN / spectral DSP | `transcribe_monophonic` preprocesses audio, detects onsets, resamples to the analysis rate, then uses multi-resolution YIN or the STFT/harmonic-salience pitch tracker. WAV streaming applies that analysis per chunk and merges notes onto the source-time axis. See [transcriber.cpp](../src/transcriber.cpp#L1855) and [StreamingOptions processing](../src/transcriber.cpp#L1977). |
| ONNX inference | The path-based compatibility API loads/reuses an `Ort::Session`. Basic Pitch is recognized by its fixed waveform input dimensions, resampled to 22.05 kHz and run in overlapping fixed windows; stitched frame probabilities and decoded notes are retained across the recording. The generic legacy contract resamples to 16 kHz and submits one full waveform tensor. The separate versioned [ModelBackend interface](../src/inference.cpp) makes model contracts and ownership more explicit. See [transcriber.cpp](../src/transcriber.cpp#L687) and [ONNX dispatch](../src/transcriber.cpp#L2164). |
| ASIO acquisition | The legacy `capture_asio` path has an RtAudio callback feeding separate recording and analysis SPSC rings; a worker runs realtime YIN and the caller receives the recording. `capture_asio_live` instead pushes timestamped blocks into `LiveTranscriptionSession`, which owns bounded input/event queues, ingest and analysis workers, gap reporting and optional refinement. See [asio_capture.cpp](../src/asio_capture.cpp#L35), [live capture](../src/asio_capture.cpp#L68), and [live pipeline contract](live.md#architecture). |
| Note events and time | Detectors produce `Note` vectors with measured seconds. `decoding.cpp` can turn aligned frame evidence into sample-referenced canonical events; the score/editor model preserves measured timing separately from musical positions. WAV chunk merging adds each chunk's source-frame offset; the live API expresses event times as recording frames divided by sample rate. These timestamp differences are intentional (analysis/event latency is distinct from onset time). See [decoding.cpp](../src/decoding.cpp#L128), [live.md](live.md#frames-and-timestamps), and [score-model.md](score-model.md#time-and-pitch). |
| Musical interpretation | CLI and shared GUI backend pass note results to `interpret_notes`, which ranks tempo, meter, key and rhythmic hypotheses; `make_score_document` selects a hypothesis and constructs the canonical editable score. The older public `Note`-vector MusicXML API still uses `infer_score_metadata`. See [interpretation.cpp](../src/interpretation.cpp#L967), [main.cpp](../src/main.cpp#L1109), and [score.cpp](../src/score.cpp#L1582). |
| Exports and projects | CLI writes MusicXML, guitar tablature, MIDI and JSON; optional score-project JSON serializes the canonical score. GUI exports accepted edits through the same score writers. MusicXML and tab use score fingering assignments; MIDI and JSON retain their own time/event representations. See [main.cpp](../src/main.cpp#L1115), [musicxml.cpp](../src/musicxml.cpp#L1049), [guitar_tab.cpp](../src/guitar_tab.cpp), and [score.cpp](../src/score.cpp#L1913). |
| GUI and UI state | There is one graphical variant in this revision: the Windows WebView2 host, shared transcription backend, and local `ui/` assets. The host owns worker/score state and communicates over WebView messages; `app.js` and `editor.js` render and edit the accepted score. There is no native `src/gui.cpp` target in the current CMake project. See [CMakeLists.txt](../CMakeLists.txt#L139), [gui_webview.cpp](../src/gui_webview.cpp), [gui_transcription.cpp](../src/gui_transcription.cpp), and [ui/](../ui/). |

The central boundaries are therefore audio decoding/acquisition → DSP or
model inference → note/event construction → musical interpretation → canonical
score editing → exporters, with GUI state as a separate presentation and
editing layer. The opt-in canonical event/live/review features add provenance
and revisioning without making every older `Note`-vector API sample-based.

### Build-time and optional dependencies

CMake 3.20+ and a C++20 compiler are required. The default WAV/YIN core uses
the C++ standard library without FFmpeg, ONNX, Python, or WebView2. Tests are
opt-in and fetch Catch2 3.7.1 through CMake FetchContent; when Python is
available, CMake registers the integration/evaluation tests. Optional
features add RtAudio plus a Windows ASIO driver, FFmpeg development libraries
through pkg-config, ONNX Runtime and provider-specific SDK/runtime files, or
the WebView2 SDK/runtime. Verovio assets are separately optional. These
requirements are documented across [README build/integrations](../README.md#build),
[release checklist](release-checklist.md), and the named feature guides.

### Build and test baseline

The documented commands were run from the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Release

cmake -S . -B build-tests -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

Both configurations configured and built successfully. CTest registered 129
tests: **128 passed, 1 skipped, 0 failed** (exit code 0). The skipped case was
`real_audio_smoke`; no real-recording/device test was thereby passed.

Configuration caveat: this machine used the `MinGW Makefiles` single-config
generator for both build trees. `build/CMakeCache.txt` has an empty
`CMAKE_BUILD_TYPE`, so `--config Release` does not turn that standard build
into an optimized Release build. `build-tests/CMakeCache.txt` has
`CMAKE_BUILD_TYPE=Release`, so the test build did use Release. The standard
build succeeded, but must not be reported as a Release performance baseline.
The optional ASIO, ONNX, FFmpeg and WebView2 builds were not enabled here;
no physical ASIO device, interactive WebView2 session, GPU provider, or
FFmpeg error-injection test was run.

### Existing coverage and measured results

The C++ Catch2 suite is split across transcription, decoding, inference,
live capture/session, interpretation, fingering, score, review and project
tests. Python CTest integrations cover dependency-manifest checks, evaluation
metrics, synthetic full/spectral/realtime evaluation, model-interface/live
evaluation, CLI exports, UI assets and notation fixtures. The current suite
also exercises chunk-boundary/held-note behavior, score transactions,
serialization, timing constraints and generated synthetic cases. It does not
exercise real hardware or a full interactive GUI lifecycle.

The following measurements are from the synthetic reports emitted by this
CTest run, using 16 clips / 41.41 seconds of generated audio, one run per clip
and no warmup. They are a reproducible local baseline, not a multi-run
benchmark or a generalization claim:

| CTest mode | Strict note-event F1 (TP/FP/FN) | 10 ms frame F1 | Chord-note recall | Mean analysis / corpus RTF |
|---|---:|---:|---:|---:|
| Full YIN | 48.15% (13/17/11) | 78.33% | 20% (1/5) | 16.39 ms / 0.00633 |
| Full spectral | 26.67% (16/80/8) | 81.01% | 100% (5/5) | 13.00 ms / 0.00530 |
| Realtime YIN | 69.57% (16/6/8) | 80.65% | 20% (1/5) | 3.11 ms / 0.00149 |

The small corpus makes the substantial synthetic spectral false-positive
count visible; synthetic scores should not be extrapolated to recordings.
The README's GuitarSet v1.1.0 figures (Basic Pitch 14.58% strict
note-event F1 / 72.97% frame F1) are historical stored results, not rerun in
this audit. Likewise, the older before/after DSP timings in
[performance.md](performance.md) are not this run's benchmark. Dataset
identity, metric definitions and known GuitarSet overlap limitations are
described in [evaluation.md](evaluation.md) and [python/README.md](../python/README.md).

### Current findings

#### C01 — Legacy ASIO exception teardown can race callback-state lifetime

- **Severity / evidence:** High impact, **Risk**; not reproduced on hardware.
- **Source:** [capture_asio](../src/asio_capture.cpp#L223) constructs `RtAudio`
  before stack-owned `CaptureState`, starts the callback, and explicitly stops
  and closes the stream only on the normal path. User callbacks and preview
  allocations execute while capture is active and can throw. During unwinding,
  the later-declared state is destroyed before the earlier-declared RtAudio
  object has completed teardown, while the callback still holds its pointer.
- **Reproduction:** ASIO-enabled build with a throwing progress/audio-update
  callback while the stream is active; run under ASan or a lifetime-focused
  stress harness. Not run here because ASIO was disabled.
- **Remediation:** Use an exception-safe stream guard that stops/closes and
  joins callbacks before callback state is destroyed; test callback exceptions
  and cancellation.

#### C02 — Batch output names collide for same-stem inputs

- **Severity / evidence:** High conditional data-loss impact,
  **Source-confirmed**; FFmpeg batch not built here.
- **Source:** [process_batch](../src/main.cpp#L1229) constructs every
  destination from `file.stem()` and writes fixed `.musicxml`, `.tab.txt`,
  `.mid`, `.json` and `.score.json` names. With FFmpeg enabled, both WAV and
  compressed formats are accepted, so `take.wav` and `take.flac` target the
  same output paths. The later successful file can silently replace the
  earlier file's results.
- **Reproduction:** In an FFmpeg-enabled build, batch a directory containing
  two supported, successfully decodable files with the same stem but different
  extensions; inspect the shared output names.
- **Remediation:** Preflight output collisions and assign deterministic unique
  names (or reject ambiguous batches) before processing starts. Add a
  same-stem batch test. Keep the current continue-after-per-file-failure
  behavior for non-colliding inputs.

#### C03 — CLI/config numeric parsing accepts partial and non-finite values

- **Severity / evidence:** Medium, **Reproduced** for partial numeric tokens;
  NaN-to-size risk is source-confirmed but not exercised through ASIO.
- **Source:** `apply_config` and `parse_options` in
  [main.cpp](../src/main.cpp#L217) use `stoi`/`stod`/`stoul` without checking
  full-string consumption consistently. `capture_asio` validates only
  `seconds <= 0` and `seconds > 300` before converting duration to
  `size_t`; NaN satisfies neither comparison. The WebView request parser has
  stricter finite/full-consumption checks, so validation differs by surface.
- **Reproduction:** `transcriber tone.wav --tempo 96junk ...` completed
  successfully; separate `--help` probes accepted `--device-id 1.5` and
  `--asio nan`. ASIO itself was disabled here, so the conversion path was not
  run.
- **Remediation:** Centralize strict integer/finite-number parsing for CLI and
  config input; validate again at public capture entry before any conversion.
  Reject trailing characters, NaN/infinities, and fractional integers with
  option-specific errors.

#### C04 — Linear downsampling aliases out-of-band energy into valid pitches

- **Severity / evidence:** Medium accuracy defect, **Reproduced**.
- **Source:** [resample_linear](../src/transcriber.cpp#L201) interpolates but
  does not low-pass before the YIN analysis-rate downsample at
  [transcribe_monophonic](../src/transcriber.cpp#L1855).
- **Reproduction:** A 2-second 44.1 kHz, 10,585 Hz mono PCM sine, above the
  11.025 kHz analysis Nyquist frequency, was transcribed by the default CLI
  as MIDI 69 (A4). The source contains no 440 Hz fundamental; it aliases
  into the supported pitch range during downsampling.
- **Remediation:** Use a documented anti-alias low-pass/polyphase resampler
  (or an equivalent band-limited method) for decimation, and add
  above-Nyquist rejection plus in-band pitch/timing fixtures.

#### C05 — YIN lag search contradicts its advertised low MIDI bound

- **Severity / evidence:** Medium pitch-accuracy defect, **Reproduced**.
- **Source:** `yin_with_workspace` caps `max_lag` at `sample_rate / 65`
  ([transcriber.cpp](../src/transcriber.cpp#L72)) but later permits MIDI
  28–96. At the 11.025 kHz analysis rate, the search cannot represent
  fundamentals below approximately 65 Hz (near MIDI 36); the lower MIDI
  guard is not evidence of detector support.
- **Reproduction:** A sustained 58.2705 Hz tone (MIDI 34) produced MIDI 36;
  a 32.7032 Hz tone produced no stable pitches. A 65.4064 Hz control
  produced MIDI 36.
- **Remediation:** Make supported pitch limits consistent with lag/window
  sizes and documentation. Extend the offline search only with adequate
  windows and octave-error tests; do not claim MIDI 28 for YIN until
  boundary fixtures pass.

#### C06 — Several full-buffer routes have no duration/resource budget

- **Severity / evidence:** Medium resource-exhaustion risk,
  **Source-confirmed**, not stress-tested with extreme files.
- **Source:** `read_audio`/`read_audio_ffmpeg` and generic ONNX hold complete
  audio buffers ([audio_file.cpp](../src/audio_file.cpp#L99),
  [ffmpeg_audio.cpp](../src/ffmpeg_audio.cpp#L69),
  [transcriber.cpp](../src/transcriber.cpp#L2200)). Spectral analysis
  materializes `frame_count * bin_count` doubles and pitch-frame collections;
  ONNX Basic Pitch retains stitched per-frame probabilities. The spectrogram
  cache's 64 MiB retention threshold does not bound the temporary computed
  spectrogram or the other duration-dependent allocations.
- **Reproduction:** Run long compressed-audio, generic-ONNX or full spectral
  inputs while measuring peak memory; there is no configured maximum duration
  or preflight memory budget in these paths.
- **Remediation:** Add checked resource estimates and clear maximum/budget
  behavior first. Where the model contract permits, process bounded windows
  while preserving overlap/timestamps; do not arbitrarily chunk generic
  models that require full context.

#### C07 — GUI stop requests do not cancel file decoding or offline analysis

- **Severity / evidence:** Medium liveness issue, **Source-confirmed**;
  interactive lifecycle not run here.
- **Source:** [run_transcription](../src/gui_transcription.cpp#L309) checks
  the stop token at entry and passes it to ASIO capture, but does not pass it
  to `transcribe_wav_streaming`, `read_audio` or offline `transcribe_*`
  analysis. The WebView close path requests stop and joins the worker
  ([gui_webview.cpp](../src/gui_webview.cpp#L4539)); file work can therefore
  continue until completion before close resolves.
- **Reproduction:** Request close during a long WAV or offline model analysis
  and observe that the stop token is not observed inside the active file/DSP
  path. No WebView GUI was run in this environment.
- **Remediation:** Thread cancellation through bounded decode/chunk/DSP/model
  stages and check it between units of work. Add close-during-decode,
  analysis, export and ASIO tests with a declared response bound.

#### C08 — Path-based ONNX session cache cannot observe model replacement

- **Severity / evidence:** Medium stale-model risk, **Source-confirmed**;
  ONNX was disabled here.
- **Source:** [load_onnx_session](../src/transcriber.cpp#L687) keys a static
  process-lifetime map by normalized path, provider and device only. It does
  not include file identity/content or provide invalidation/eviction.
  Replacing a model at the same path in a long-lived process can therefore
  reuse the old loaded session; loading many paths retains sessions until
  process exit. This finding is scoped to the path-based compatibility API,
  not every explicit `ModelBackend` instance.
- **Reproduction:** In an ONNX-enabled process, infer with model A, replace
  the file at the same path with compatible model B, and infer again through
  the path-based wrapper; compare the output/session identity.
- **Remediation:** Define reload semantics explicitly; use a versioned file
  identity or explicit cache invalidation and bounded ownership. Add a
  same-path replacement regression test.

#### C09 — FFmpeg resampler flush can convert errors into successful EOF

- **Severity / evidence:** Medium error-reporting defect,
  **Source-confirmed**, not exercised with FFmpeg enabled.
- **Source:** The regular decode loop checks negative `swr_convert` results,
  but the flush loop in [ffmpeg_audio.cpp](../src/ffmpeg_audio.cpp#L190)
  breaks on `converted_samples <= 0`, treating a negative conversion error
  as ordinary drain completion.
- **Reproduction:** Inject a negative `swr_convert` result during flush in an
  FFmpeg-enabled test build; current code exits the loop and can return
  already-decoded samples as success.
- **Remediation:** Break only on zero; route negative values through
  `throw_ffmpeg_error`. Test both normal drain and injected flush failure.

#### C10 — Legacy vector-based MusicXML can label a one-tick duration incorrectly

- **Severity / evidence:** Medium public-API export defect,
  **Source-confirmed**. The normal CLI now exports the canonical score path;
  this finding applies to the legacy `std::vector<Note>` overload.
- **Source:** [rhythm_values](../src/musicxml.cpp#L70) maps one tick to
  `"256th"`, while [ticks_for_time](../src/musicxml.cpp#L106) and the legacy
  writer use 48 divisions per quarter ([write_musicxml(notes)](../src/musicxml.cpp#L518)).
  One tick is 1/48 of a quarter, not the 1/64 quarter represented by a
  256th note; no tuplet ratio is attached to that table entry.
- **Reproduction:** Call the public vector overload with a 120 BPM score and
  a 0.01-second note: the timing grid rounds the duration to one tick, whose
  emitted note type does not equal the XML duration. The separate
  `ScoreDocument` export path should be tested independently and is not
  asserted to share this exact defect.
- **Remediation:** Make duration/divisions/type/dots/tuplet arithmetic agree
  exactly, or reject/round to a representable lattice with a declared timing
  tolerance. Add semantic XML tests, not only well-formedness checks.

#### C11 — Legacy ASIO path ignores driver status and stream-operation results

- **Severity / evidence:** Medium capture-diagnostics/data-integrity gap,
  **Source-confirmed**; no device test.
- **Source:** The legacy [capture_callback](../src/asio_capture.cpp#L35)
  does not use `RtAudioStreamStatus`; its ring-overflow flag only covers the
  application's own rings. The legacy `capture_asio` call sites ignore
  `openStream`, `startStream` and `stopStream` results
  ([asio_capture.cpp](../src/asio_capture.cpp#L256)). In contrast, the
  newer live-ASIO path checks open/start errors and records driver overflow.
- **Reproduction:** Hardware or a test double that reports
  `RTAUDIO_INPUT_OVERFLOW` or fails a legacy open/start operation; currently
  no driver-overflow diagnostic is surfaced by that path.
- **Remediation:** Propagate driver overflow as a gap/error and check all
  RtAudio results. Preserve the newer live path's explicit status handling;
  add a fake-driver test before hardware qualification.

#### C12 — Compatibility and documentation paths still describe different architectures

- **Severity / evidence:** Low design/documentation gap,
  **Source-confirmed**; not a correctness failure by itself.
- **Source:** New CLI/GUI score paths use `interpret_notes` and the canonical
  `ScoreDocument`, while the legacy vector MusicXML overload calls the older
  `infer_score_metadata` ([transcriber.cpp](../src/transcriber.cpp#L1592),
  [interpretation.cpp](../src/interpretation.cpp#L967),
  [musicxml.cpp](../src/musicxml.cpp#L518)). This preserves compatibility
  but leaves two metadata/notation routes that can evolve differently.
  Separately, the opening of [live.md](live.md#L5) says “both GUIs,” while
  the current [README](../README.md#L35) and CMake target list describe only
  the WebView2 workstation.
- **Reproduction:** Compare the legacy vector writer's metadata path with the
  CLI's canonical score path; inspect the cited `live.md` and README
  statements against the current CMake GUI targets.
- **Remediation:** Keep legacy APIs stable, but route shared inference through
  a single implementation where behavior permits. Correct the stale GUI
  wording and document deliberate compatibility differences.

#### C13 — Documented standard build does not select Release with a single-config generator

- **Severity / evidence:** Low build-reproducibility gap,
  **Reproduced** in the documented configuration.
- **Source:** The standard commands in [README build instructions](../README.md#build)
  configure without `CMAKE_BUILD_TYPE`, then pass `--config Release` at build
  time. The local generator is `MinGW Makefiles`, which is single-config, and
  its `CMAKE_BUILD_TYPE` remains empty; `--config Release` is ignored by this
  generator. The separately documented Release test configuration did set
  `CMAKE_BUILD_TYPE=Release`.
- **Reproduction:** Run the documented standard commands with a single-config
  Makefile generator and inspect `CMakeCache.txt`; the standard build here
  succeeded but did not build Release.
- **Remediation:** Document/configure `CMAKE_BUILD_TYPE=Release` for
  single-config Release builds and retain `--config Release` for
  multi-config generators; prefer the existing Release presets for measured
  performance results.

### Prioritized implementation plan

1. **Correctness baselines first:** add regression fixtures for anti-alias
   rejection, YIN lower/upper register behavior, and legacy MusicXML duration
   semantics. Fix those defects without changing public interfaces; compare
   note events and timestamps against the current synthetic baseline.
2. **Input and capture safety:** introduce strict shared numeric parsing,
   reject non-finite/out-of-range values before conversions, and make legacy
   ASIO teardown exception-safe with explicit driver/open/start/stop handling.
   Validate with fake-driver/callback-exception tests before physical devices.
3. **Prevent destructive batch outcomes:** preflight same-stem output
   collisions and add deterministic batch tests; then stage outputs or report
   exact partial completion for multi-file exports.
4. **Bound work and honor cancellation:** establish documented per-mode
   resource budgets, add early estimates for full-buffer routes, then add
   cooperative cancellation at decode/chunk/DSP/inference boundaries.
5. **Harden optional model/decoder paths:** define ONNX cache invalidation and
   reload behavior; distinguish FFmpeg flush EOF from errors and test injected
   failures in the optional build.
6. **Make build measurements comparable:** set Release at configure time for
   single-config generators and record the effective build configuration.
7. **Reduce compatibility drift:** consolidate shared metadata/notation
   policy only after comparison fixtures show legacy output changes are
   understood; align GUI/live documentation and expand lifecycle/export
   coverage. Do not perform a broad refactor as part of this audit.

### Findings deliberately not promoted to defects

- No race was established in the documented single-producer/single-consumer
  ring usage. The live API explicitly documents the SPSC contract; concurrent
  multiple-producer use is not promised.
- WAV chunk offsets are explicitly added to note/evidence times, canonical
  events retain sample positions, and live events use frame/sample-rate
  timestamps. No inconsistent timestamp defect was reproduced in the paths
  audited; event detection latency is intentionally distinct from onset time.
- Basic Pitch fixed input/output dimensions and the generic ONNX contract are
  validated in source. Shape-based Basic Pitch dispatch in the compatibility
  API remains a fragile assumption, but no invalid tensor access was found.
- Heuristic confidence, approximate musical interpretation, the limited
  synthetic corpus and the lack of physical-device verification are disclosed
  limitations, not hidden failures. The optional model/provider and
  compressed-audio paths need their own enabled-build validation.

## Archived audit (0.1.4 baseline; retained for history)

The following sections and F01–F29 anchors are preserved from the audit dated
2026-10-20 at commit `0561397`. They are historical evidence only; do not
interpret their severity, reproduction status or inventory as a current
0.1.5 finding. The current source-verified findings and test results above
take precedence. The roadmap's historical finding links continue to resolve.

## 1. Historical scope, baseline, and interpretation

Audit date: 2026-10-20. Source baseline:
`0561397b3705aeb8763726ea4b73fd83b4012aac` (Transcriber 0.1.4).
The worktree was clean at the start. This phase changes documentation only;
it does not implement fixes, replace the architecture, or retune the model.
The implementation sequence is in [the roadmap](roadmap.md).

This is a point-in-time audit of that baseline, not a current inventory of
later implementation or CI changes. Release and workflow status is summarized
in [the release checklist](release-checklist.md) and the current
[README](../README.md); audit findings remain evidence about the named source
baseline unless explicitly updated in a later change.
The project MIT license and WebView2-only graphical frontend were added after
this baseline audit.

This is an engineering/correctness audit, not a claim of commercial AMT
quality, a penetration test, or a legal opinion.

Evidence labels used below:

- **Confirmed:** a behavior follows directly from the inspected source.
- **Reproduced:** also exercised by a bounded local check during this audit.
- **Risk:** a plausible failure path supported by source, not reproduced.
- **Limitation/gap:** an intentional restriction or missing assurance, not
  necessarily a bug or a contradiction of the documentation.

Severity: **High** means potential lost/corrupted results, unsafe lifetime,
or a material blocker to reliable transcription/distribution. **Medium**
means incorrect conditional behavior, significant accuracy/resource limits,
or an important validation gap. **Low** means maintainability or
reproducibility work with less immediate user impact. Priority is not a claim
that every documented limitation must be eliminated.

### Inspection inventory

The following text sources were read, including both Windows front ends:

| Area | Inspected files |
|---|---|
| Build and CI | [CMakeLists.txt](../CMakeLists.txt), [CI workflow](../.github/workflows/ci.yml), [.gitignore](../.gitignore) |
| All public headers | [audio.hpp](../include/transcribe/audio.hpp), [asio_capture.hpp](../include/transcribe/asio_capture.hpp), [transcriber.hpp](../include/transcribe/transcriber.hpp), [musicxml.hpp](../include/transcribe/musicxml.hpp), [guitar_tab.hpp](../include/transcribe/guitar_tab.hpp) |
| Engine and decoders | [transcriber.cpp](../src/transcriber.cpp), [audio_file.cpp](../src/audio_file.cpp), [ffmpeg_audio.cpp](../src/ffmpeg_audio.cpp) |
| Capture/concurrency | [asio_capture.cpp](../src/asio_capture.cpp), [audio_ring_buffer.cpp](../src/audio_ring_buffer.cpp), realtime implementation in [transcriber.cpp](../src/transcriber.cpp) |
| CLI and exporters | [main.cpp](../src/main.cpp), [musicxml.cpp](../src/musicxml.cpp), [guitar_tab.cpp](../src/guitar_tab.cpp) |
| Windows GUIs | [gui.cpp](../src/gui.cpp), [gui_webview.cpp](../src/gui_webview.cpp), [gui_transcription.hpp](../src/gui_transcription.hpp), [gui_transcription.cpp](../src/gui_transcription.cpp) |
| Web UI | [index.html](../ui/index.html), [styles.css](../ui/styles.css), [app.js](../ui/app.js), [editor.js](../ui/editor.js) |
| Tests and fixtures | [Catch2 tests](../tests/transcribe_tests.cpp), [CLI export smoke](../tests/cli_export_smoke.py), [UI smoke](../tests/ui_smoke.py), [real-audio smoke](../tests/real_audio_smoke.py), [sample generator](../samples/generate_samples.py), [sample provenance](../samples/README.md), both expected-pitch fixture files |
| Evaluation/provenance | [project README](../README.md), [Python README](../python/README.md), [evaluator](../python/evaluate_guitarset.py), [model README](../python/models/README.md), [model license](../python/models/LICENSE-basic-pitch.txt), structured summaries and per-recording count aggregation from [stored GuitarSet results](../python/evaluation/guitarset-v1.1.0-results.json) |

The bundled model was hashed, not reverse-engineered or re-exported. The
large evaluation JSON was inspected structurally and aggregated, not treated
as evidence that the evaluation was rerun. External dataset archives,
upstream license histories, and current GitHub CI run results were not
downloaded or independently validated.

## 2. Dependency and execution map

### Build-time and runtime dependencies

```text
CMake (>=3.20), C++20 compiler
|
+-- transcriber_core [STATIC, public include directory, C++20]
|   +-- audio_file / audio_ring_buffer / transcriber / musicxml / guitar_tab
|   +-- asio_capture stub by default
|   +-- optional RtAudio ASIO + Windows driver; MinGW winpthread
|   +-- optional ONNX Runtime + model
|   |   +-- optional CUDA / DirectML / ROCm providers and matching SDK/runtime
|   +-- optional FFmpeg avformat/avcodec/avutil/swresample via pkg-config
|
+-- transcriber [CLI] -> transcriber_core
+-- Windows transcriber_gui_backend -> transcriber_core
|   +-- transcriber-gui [native Win32] -> Win32 system libraries
|   +-- OR transcriber-gui [WebView2] -> loader + Evergreen runtime + local ui/
|
+-- optional transcriber-tests -> core + fetched Catch2 3.7.1
    +-- optional Python CTest integrations
        +-- UI syntax checks additionally use Node when available
```

See [target definitions/options](../CMakeLists.txt#L4) and
[optional dependency linkage](../CMakeLists.txt#L158).
All feature switches, GPU providers, and tests default to OFF. The default
WAV/YIN core has no FFmpeg, ONNX, Python, Node, or WebView dependency.
Windows' default GUI uses operating-system Win32 APIs.

C++20 is propagated with `target_compile_features`. Production targets and
tests receive `/W4 /permissive-` on MSVC or
`-Wall -Wextra -Wpedantic` otherwise. There is no project-wide sanitizer,
warning-as-error, LTO, or benchmark policy. Those omissions alone are not
correctness defects. Release optimization comes from the selected generator
and configuration, not a project-specific optimization flag.

### Audio-to-output dependencies

```text
CLI parse_options / config                         GUI Request / message parsing
             |                                         |
             +--------------------+--------------------+
                                  |
WAV -> read_wav_blocks -> mono float blocks -> transcribe_wav_streaming
 |                            |                   |
 +-> read_wav -> AudioBuffer   |              overlapped chunk/core merge
Other audio -> FFmpeg -> mono 44.1 kHz AudioBuffer       |
ASIO callback -> recording SPSC ring -> full recording  |
             -> analysis SPSC ring -> realtime YIN     |
                         -> stable Onset/Offset events |
                                  |                    |
                                  +--------------------+
                                                       |
built-in offline: preprocess -> onsets/FFT -> resample -> YIN
                         OR cached spectrogram -> harmonic salience/greedy pitches
ONNX generic: full input -> 16 kHz -> one Run -> generic v1 pitch frames
ONNX Basic Pitch: full input -> 22.05 kHz -> overlapped fixed-window Runs
                                             -> stitched frame probabilities
                                                       |
                           frame grouping -> vector<Note> in seconds
                                                       |
                 infer_score_metadata (tempo/key/meter/swing heuristics)
                                                       |
         +--------------------+-------------------+----------------------+
         |                    |                   |                      |
   MusicXML voices,      guitar fingering,     MIDI events,         CLI JSON,
   quantization/ties     ASCII tab/alternates  480 PPQ              GUI messages
         |
   notation + tab parts share selected positions

WebView completion/live messages -> editor note/position state -> canvas/Web Audio
WebView edited export -> parsed notes/positions -> same C++ writers
```

There is no common persistent quantized score/voice graph: public `Note`
objects hold performance times, while MusicXML constructs its own private
voice/token graph. MIDI uses continuous-time-derived ticks; the canvas
positions notes in seconds. These are different representations, not
automatically equivalent round trips.

### Ownership and synchronization

- [The ring](../src/audio_ring_buffer.cpp#L13) uses monotonic atomic indices
  and release/acquire publication for one producer and one consumer. Its
  operations do not allocate after construction. It is not an MPMC queue.
- [Capture](../src/asio_capture.cpp#L23) has separate recording and analysis
  rings. The RtAudio callback only writes samples/atomics; the capture caller
  drains recording and delivers callbacks; a dedicated `jthread` drains
  analysis. Analysis exceptions are transferred after joining.
- [RealtimeTranscriber](../src/transcriber.cpp#L1166) locks its public
  operations and copies incoming samples. Its public type is distinct from
  a streaming ONNX session.
- [Offline FFT workers](../src/transcriber.cpp#L291) own reusable workspaces;
  the single-entry spectrogram cache and ONNX session map use mutexes.
- Both GUIs marshal worker results via posted window messages and normally
  join before deferred destruction. Successfully posted heap payloads pass
  to the window procedure; failed posting retains `unique_ptr` ownership.
  Startup COM callbacks have a separate lifetime concern (F19).

### Build variants and CI evidence

| Variant | Repository CI configuration | Local audit evidence |
|---|---|---|
| Default/native Windows | Windows runner configures tests, builds Release, runs CTest | LLVM-MinGW Release: build and default suite run |
| WebView2 Windows | Separate WebView2 build and ZIP; tests run against the default build | WebView2 + ONNX + ASIO Release build succeeded; no interactive GUI check |
| Linux GCC/Clang, FFmpeg OFF/ON | Four matrix combinations, CTest, compressed FLAC smoke, TGZ | Not run on this Windows host |
| CPU ONNX | Linux ONNX Runtime 1.20.1, tests | Existing Windows ONNX + ASIO Release build rebuilt; two selected tests run |
| ASIO hardware | No ASIO job | Compiled only; device/driver capture not exercised |
| CUDA/DirectML/ROCm | No provider job | Not built or executed |

Local build directories are caches, not authoritative feature presets. Some
have old `transcribe*` artifacts alongside current `transcriber*` targets;
the incomplete NMake release directory and unoptimized older directories do
not establish a working Release variant.

### Licensing and provenance inventory

| Component | Evidence and qualification |
|---|---|
| Repository code | No tracked top-level license file was present. Do not infer a license from public GitHub availability. |
| Basic Pitch model | Pinned upstream commit `fa5997af0a8210982619003269994a1be25eddf3`; Apache-2.0 license included. Current 230,444-byte model SHA-256 is `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`, matching the stored evaluation identity. Exact export reconstruction is not documented. |
| GuitarSet | Documentation identifies v1.1.0, DOI, CC BY 4.0, archive MD5s, and possible model-training overlap. No recordings are redistributed in the repository. Archive checksums were not rerun here. |
| Synthetic WAV fixtures | Deterministic generator and expected pitches are present; these are not recorded-audio accuracy evidence. A project license still needs to establish repository-wide reuse terms. |
| WebView2 | SDK version 1.0.4258.31 in CI; loader license/notice and Evergreen bootstrapper are copied/installed. Bootstrapper URL is mutable; download validates TLS/status/size, not a pinned digest. |
| ONNX Runtime | CI downloads 1.20.1; local configured SDK is 1.20.1. Runtime/provider binaries and their notices need a release-payload audit; the model's Apache license is not the runtime's license. |
| RtAudio / ASIO | CMake finds an externally installed package without a version constraint. Inspected local header identifies RtAudio 6.0.1. Verify RtAudio and the specific ASIO SDK distribution terms separately; no license violation is asserted. |
| FFmpeg | External development libraries, optional only. License/redistribution requirements depend on the actual configured FFmpeg build and linked codecs; the repository does not record those choices. |
| Catch2 | Test-only FetchContent dependency pinned to tag v3.7.1; not a mandatory production dependency. |

## 3. Validation actually executed

Host: Windows x64; CMake 4.4.4, Python 3.12.10, LLVM-MinGW Clang 23.1.1.
Node was not available on the command PATH.

| Check | Outcome | What it does not prove |
|---|---|---|
| VS Code unit-test runner for the Catch2 source | No tests discovered | Not a test pass; CTest was used instead |
| `cmake --build .\build-release-mingw --parallel` and `ctest --test-dir .\build-release-mingw -C Release --output-on-failure` | Build succeeded; **30 passed, 1 skipped**, 31 registered; CTest reported 1.23 s | External real-audio smoke skipped; no hardware, GUI interaction, or wall-clock performance qualification |
| Rebuild optional ONNX/ASIO directory; CTest selector `Basic Pitch ONNX\|cli_export_validation` | Build succeeded; **2 selected tests passed**, 1.25 s | Not the entire optional suite or an ASIO/GPU run |
| `cmake --build .\build-webview-onnx-asio-mingw --parallel` | CLI and modern GUI targets succeeded | No visual/browser/COM-shutdown behavior exercised |
| Install full-featured build to an isolated prefix; inspect PE imports with `llvm-readobj --coff-imports` | Only WebView2 loader DLL installed; imports include missing ONNX and LLVM-MinGW DLLs (F02) | No clean-machine launch or redistribution test |
| CLI malformed numeric probes with `--help` | `--asio nan`, `--tempo 96junk`, `--device-id 1.5` all exited 0 (F04) | No invalid ASIO hardware invocation attempted |
| Isolated C++20 harness linked to current default Release core | Reproduced YIN range/aliasing, MusicXML duration mismatch, invalid tab alternatives, and lost explicit override (F07/F08/F13/F15) | Synthetic probes, not a real-audio benchmark |

The optional rebuild emitted 59 Catch2 macro `__COUNTER__` extension warnings
under this newer compiler. It succeeded; this is not described as a
warning-free build or a production-source failure.

The UI smoke prints that JS syntax checking was skipped when Node is absent.
A passing asset test therefore did not validate JavaScript execution here.
The GuitarSet dataset evaluation, Linux jobs, packaged-app clean-machine
execution, sanitizer runs, capture hardware, GPU inference, and interactive
GUI tests were **not run**.

Temporary probes and the isolated install were kept outside the repository
and removed after their results were recorded.

## 4. Accuracy investigation: frame F1 is not note-event F1

Evidence: [event matching](../python/evaluate_guitarset.py#L80),
[frame occupancy scoring](../python/evaluate_guitarset.py#L129), and
[stored evaluation](../python/evaluation/guitarset-v1.1.0-results.json).
The 360 per-recording count sums were checked against the stored aggregate
counts. Percentages below were calculated from those counts, not from a new
transcription run.

| System / metric | TP | FP | FN | Precision | Recall | F1 |
|---|---:|---:|---:|---:|---:|---:|
| Basic Pitch onset + pitch | 28,748 | 82,292 | 33,728 | 25.89% | 46.01% | 33.14% |
| Basic Pitch strict note event | 12,646 | 98,394 | 49,830 | 11.39% | 20.24% | **14.58%** |
| Basic Pitch 10 ms frame | 1,288,384 | 349,271 | 605,393 | 78.67% | 68.03% | **72.97%** |
| YIN onset + pitch | 10,124 | 18,143 | 52,352 | 35.82% | 16.20% | 22.31% |
| YIN strict note event | 1,931 | 26,336 | 60,545 | 6.83% | 3.09% | 4.26% |
| YIN 10 ms frame | 263,918 | 170,208 | 1,629,859 | 60.79% | 13.94% | 22.67% |

Strict matching requires exact MIDI pitch, an onset within 50 ms, and an
offset within `max(50 ms, 20% of reference duration)`. Matches are
maximum-cardinality one-to-one using augmenting paths, not naive greedy
pairing. Onset-only matching removes the offset constraint.

Frame scoring compares Boolean active-pitch occupancy on a rounded 10 ms
grid. It ignores the identity of repeated same-pitch events. A fragmented
long note or incorrectly merged repetition can occupy largely correct
frames while failing event matches. The final partial-duration grid cell
also counts as one frame rather than a fraction.

There are **111,040 predicted Basic Pitch events for 62,476 references**.
Adding offset eligibility reduces matched events from 28,748 to 12,646.
These facts support substantial unmatched events and an additional
boundary/duration problem; they do not prove a unique cause such as a
particular threshold, resampler, or adapter window seam. Both onset and
offset behavior need investigation; blaming only offsets would be wrong.

The 180 comp recordings have Basic Pitch strict F1 14.00% / frame F1 73.23%;
the 180 solo recordings have 15.93% / 72.29%. The issue is not limited to
the chordal subset. YIN is a monophonic baseline on material that includes
polyphony, not a competing general polyphonic model.

This is a diagnostic dataset result, not an independent held-out
generalization result: possible GuitarSet overlap with Basic Pitch training
is explicitly acknowledged. Future decoder changes need frozen validation
protocols, recording-level splits, onset/offset diagnostics, repeated-note
tests, and a separate real-world holdout before making quality claims.

## 5. Findings

### F01 - Release selection differs by generator

- **Severity / impact:** Medium; confirmed build/documentation gap. Linux CI
  and single-config users can believe an unoptimized build is Release.
- **Source / evidence:** [README build instructions](../README.md#L35) and
  [Linux/ONNX workflow](../.github/workflows/ci.yml#L66) configure without
  `CMAKE_BUILD_TYPE` and then use `--config Release`. Makefile generators
  choose optimization at configure time; that build flag does not select it.
  Existing local default/native/test caches have an empty build type.
- **Proposed solution:** Specify Release when configuring single-config
  release/benchmark jobs; document both generator forms. Keep Debug usable.
- **Dependencies / regression risks:** None; optimization can expose latent
  UB and changes benchmark comparability. Do not change every developer
  build to Release implicitly.
- **Acceptance:** Single-config release CI records Release and optimized
  flags; multi-config Release still builds; benchmark reports identify both.

### F02 - Installed full-featured Windows payload omits required DLLs

- **Severity / impact:** High; reproduced incomplete payload. Clean-machine
  startup is not assured and PATH can supply a mismatched ONNX runtime.
- **Source / evidence:** [ONNX linkage](../CMakeLists.txt#L175) links a raw
  library path; [install rules](../CMakeLists.txt#L274) install applications,
  WebView2 assets, and model, but not ONNX or compiler runtime DLLs.
  The audited GUI imports `onnxruntime.dll`, `libc++.dll`, `libunwind.dll`,
  and `libwinpthread-1.dll`; none appeared in the isolated install.
- **Proposed solution:** Explicitly stage/install supported redistributable
  dependency closures per toolchain/feature set, with matching notices.
  Validate installed artifacts, not just the build tree.
- **Dependencies / regression risks:** F03 licensing inventory; provider-
  specific dependencies and MSVC versus MinGW rules differ. Do not copy
  arbitrary system DLLs or resolve releases through a developer PATH.
- **Acceptance:** Default and ONNX/WebView2 packages launch and transcribe
  in a clean Windows environment without development-tool paths; enabled
  providers use the intended runtime version or fail with a clear message.

### F03 - Distribution licensing and reproducible provenance are incomplete

- **Severity / impact:** High release-readiness gap, not an asserted legal
  violation. Users cannot determine repository reuse terms or the complete
  dependency notice set from a release.
- **Source / evidence:** No top-level license in the tracked inventory.
  [Installation](../CMakeLists.txt#L274) includes WebView2 and model license
  files, not an inventory for RtAudio/ASIO, ONNX, compiler runtimes or FFmpeg.
  [Model provenance](../python/models/README.md) pins upstream but gives no
  exporter environment/opset/command to reproduce the ONNX bytes.
- **Proposed solution:** Obtain the owner's project-license decision; record
  actual dependency versions/build options/licenses; bundle required
  notices. Document the model's artifact origin or reproducible export.
- **Dependencies / regression risks:** Owner/legal decisions and actual
  packaged dependencies; do not invent licenses or require optional SDKs
  in default builds. Notice obligations vary by distribution.
- **Acceptance:** Every distributed non-system binary/model maps to version,
  origin, digest and license/notice; project licensing is explicit; a pinned
  model artifact is verifiable or its reproducible conversion is documented.

### F04 - Numeric parsing accepts trailing junk and NaN capture duration

- **Severity / impact:** Medium; reproduced validation defect. Invalid
  input is accepted, and NaN can reach floating-to-size conversion in ASIO.
- **Source / evidence:** [parse_options/apply_config](../src/main.cpp#L210)
  use `stoi/stod/stoul` without checking full consumption.
  [ASIO checks](../src/main.cpp#L461) and
  [capture_asio](../src/asio_capture.cpp#L67) test `<= 0` / `> 300`, not
  finiteness. The three malformed `--help` probes in section 3 exited 0.
- **Proposed solution:** Reuse strict numeric parsers with finite/range
  validation in CLI/config and enforce the same invariant at capture entry.
- **Dependencies / regression risks:** None; malformed strings previously
  accepted will intentionally become errors. Preserve valid option names,
  units and CLI-over-config precedence.
- **Acceptance:** CLI/config reject `nan`, infinities, trailing text and
  fractional integers with nonzero exit and an option-specific error;
  `capture_asio(NaN)` rejects before device access.

### F05 - ASIO exception teardown can outlive callback state

- **Severity / impact:** High; source-backed lifetime risk, not hardware-
  reproduced. Exceptions from user callbacks/allocation can leave an active
  callback referring to destroyed stack state.
- **Source / evidence:** [capture_asio](../src/asio_capture.cpp#L73) constructs
  `RtAudio audio` before `CaptureState`; explicit stop/close is reached only
  on the normal path at lines 163-166. Progress/live/audio callbacks and
  audio-preview allocation occur while streaming. During unwinding, state is
  destroyed before the earlier-constructed RtAudio object.
- **Proposed solution:** Establish an exception-safe stream guard whose
  stop/close happens before worker and callback-state teardown. Preserve
  the original exception and make cleanup errors observable.
- **Dependencies / regression risks:** F06 clarifies RtAudio return semantics.
  Stop/join ordering must not deadlock the analysis worker or discard normal
  final events. No change to the callback's real-time allocation policy.
- **Acceptance:** Fault-inject every external callback and preview allocation;
  a fake stream verifies stop/close before state destruction on every exit.
  Hardware/ASan-assisted validation shows no callback after teardown.

### F06 - RtAudio errors and device overflow status are not propagated

- **Severity / impact:** High for undiagnosed recording failure/data gaps;
  confirmed unchecked paths, hardware behavior not exercised.
- **Source / evidence:** [capture_callback](../src/asio_capture.cpp#L35)
  ignores `RtAudioStreamStatus`; its overflow flag covers application rings
  only. [Stream operations](../src/asio_capture.cpp#L93) ignore return values
  from open/start/stop. The installed RtAudio 6.0.1 header returns
  `RtAudioErrorType` and exposes `getErrorText()`; it does not require these
  failures to throw. Capture also hardcodes 44.1 kHz/default channel.
- **Proposed solution:** Define/test a supported RtAudio API version,
  check operation results, and carry device overflow/discontinuity status
  out of the callback through atomics and explicit diagnostics.
- **Dependencies / regression risks:** Coordinate with F05 and a fake-stream
  seam; do not log, allocate or invoke UI handlers from the audio callback.
  Device/rate selection is separate optional work, not a required rewrite.
- **Acceptance:** Inject open/start/stop errors and input overflow; caller
  receives the original cause or explicit incomplete-recording status, never
  an apparently complete uninterrupted success.

### F07 - Linear downsampling aliases high-frequency energy into pitches

- **Severity / impact:** High transcription correctness; reproduced.
- **Source / evidence:** [resample_linear](../src/transcriber.cpp#L171)
  interpolates samples without a target-Nyquist antialias filter; it serves
  YIN and both ONNX adapters. A 1 s, 44.1 kHz, 10,585 Hz sine transcribed as
  MIDI 69 at confidence 0.9999, identical to a 440 Hz control.
- **Proposed solution:** Introduce a small dependency-free band-limited
  downsampling path (or adequate prefilter), shared by the analysis rates.
- **Dependencies / regression risks:** First pin fixtures and current
  outputs. Filtering changes model activations, boundaries, delay and CPU;
  do not attribute the stored event F1 entirely to this defect.
- **Acceptance:** At 44.1 -> 11.025 kHz, specified stopband test tones are
  attenuated at least 40 dB, 440 Hz passband gain stays within 0.1 dB,
  pitch error within 1 cent where estimated, and the alias fixture no
  longer produces a confident A4. Report delay/CPU and model metric deltas.

### F08 - YIN's effective lag range is narrower than its accepted MIDI range

- **Severity / impact:** Medium; reproduced register limitation and
  high-confidence subharmonic error.
- **Source / evidence:** [yin_with_workspace](../src/transcriber.cpp#L73)
  bounds lags around 65-1000 Hz, then accepts MIDI 28-96 at line 134.
  With adequate-length 11.025 kHz synthetic frames, MIDI 28 returned no
  pitch and MIDI 96 returned 77/confidence 0.972156; 40/69/83 were correct.
- **Proposed solution:** Make the supported frequency bounds explicit and
  consistent with lag/window choices; extend offline range only with
  adequate windows. Expose shorter realtime-window limits honestly.
- **Dependencies / regression risks:** F07 and register fixtures. Wider
  searches can increase CPU and octave confusion; a 20 ms window cannot
  promise the same low-register behavior as long offline frames.
- **Acceptance:** Published per-mode bounds match tested behavior;
  representative boundary pitches are within one semitone inside those
  bounds, and unsupported registers are not advertised as supported.

### F09 - Event accuracy is inadequate; its exact failure decomposition is missing

- **Severity / impact:** High quality limitation, not a new claim that the
  documented diagnostic result was hidden.
- **Source / evidence:** Section 4 and
  [Basic Pitch decoding](../src/transcriber.cpp#L777): 14.58% strict F1,
  33.14% onset F1, 72.97% frame F1 and 111,040 predictions versus 62,476
  references. Current synthetic chord test only requires each expected
  pitch to appear, not correct boundaries or few false positives.
- **Proposed solution:** Add event-boundary/repetition diagnostics and
  validation splits before isolated decoder experiments. Investigate
  activation hysteresis, gaps, onsets/offsets and window seams separately;
  do not replace the model or assert an unmeasured root cause.
- **Dependencies / regression risks:** Frozen provenance/protocol (F23),
  F07 and meaningful tests (F25). Avoid tuning on the reported evaluation
  set and advertising it as independent generalization.
- **Acceptance:** Recomputed baseline counts agree exactly; diagnostics
  identify FP/FN/duration/repetition categories on a fixed validation set.
  Promote a decoder change only with declared event-F1 improvement and
  bounded frame-F1/precision regression on recording-separated data.

### F10 - ONNX remains full-recording in memory; Basic Pitch has avoidable work

- **Severity / impact:** Medium; confirmed resource limitation plus a
  specific redundant operation.
- **Source / evidence:** [transcribe_with_onnx](../src/transcriber.cpp#L1978)
  resamples all input to 16 kHz before Basic Pitch dispatch; Basic Pitch
  independently resamples to 22.05 kHz at line 713. The generic adapter
  runs one full `[1, samples]` tensor (lines 2006-2017). Basic Pitch uses
  43,844-sample windows but retains stitched 69-pitch double probabilities
  and decoded frame data (lines 720-810). Audio decode is also full-buffer.
  One hour of 16 kHz float input alone occupies 230.4 MB (decimal).
- **Proposed solution:** First dispatch before the unused 16 kHz resample.
  Then incrementally decode bounded Basic Pitch windows with overlap state.
  Keep generic full-input semantics unless a model explicitly permits
  chunking; add preflight/resource diagnostics.
- **Dependencies / regression risks:** F09 seam/boundary fixtures and memory
  measurement. Arbitrary generic-model chunking can invalidate context and
  output timing; reducing inference window size alone does not bound decode.
- **Acceptance:** Removing the redundant resample leaves note outputs
  unchanged. For a later bounded Basic Pitch mode, peak working memory
  excluding returned notes meets a documented duration-independent budget
  and seam event times match the reference within one model frame.

### F11 - Confidence values have incompatible, uncalibrated meanings

- **Severity / impact:** Medium API/UX limitation; already acknowledged in
  the README, but thresholds/percentages remain easy to misinterpret.
- **Source / evidence:** [YIN confidence](../src/transcriber.cpp#L138) is
  `1 - best_value`; [spectral confidence](../src/transcriber.cpp#L452)
  is relative salience (strongest candidate can be 1.0); ONNX uses model
  activations. [CLI JSON](../src/main.cpp#L502) and
  [MIDI velocity](../src/musicxml.cpp#L507) consume the same `confidence`
  concept, although it is not a cross-model probability.
- **Proposed solution:** Document score semantics and source; retain legacy
  fields while adding optional provenance/calibration metadata. Evaluate
  per-backend threshold curves before any calibration claim.
- **Dependencies / regression risks:** F09/F23 for labeled evaluation;
  consumers may rely on current thresholds or velocity mapping.
- **Acceptance:** Output/UI label heuristic scores correctly; each backend
  has precision/recall-versus-threshold evidence. No percentage is described
  as probability without a held-out calibration result.

### F12 - Rhythm inference and quantization lack a common musical-time model

- **Severity / impact:** Medium documented feature limitation; not a demand
  for a replacement score architecture.
- **Source / evidence:** [infer_score_metadata](../src/transcriber.cpp#L1412)
  infers global tempo from intervals, meter from interval/phrase grouping,
  swing from ratios and key from pitch profiles.
  [ticks_for_time](../src/musicxml.cpp#L77) snaps independent absolute
  starts/durations to nearby grids, then assigns greedy overlap voices.
  No beat/downbeat map, tempo changes or persistent voice identity exists.
- **Proposed solution:** First characterize pickup, syncopation, swing,
  tuplets and crossing/overlapping voices with fixtures. Add explicit
  musical-time annotations locally where needed, preserving seconds-based
  APIs and the current default unless an improvement is validated.
- **Dependencies / regression risks:** F13 must first ensure internally
  consistent notation. New snapping/voice choices change visible scores
  and must not silently alter raw MIDI/JSON performance times.
- **Acceptance:** Each supported rhythmic fixture has expected measures,
  per-voice duration sums, ties and bounded quantization displacement.
  Unsupported meter/tempo structures remain clearly labeled, not guessed
  with a new unsupported confidence claim.

### F13 - MusicXML one-tick tokens disagree with their written note type

- **Severity / impact:** Medium; reproduced semantic export defect.
- **Source / evidence:** [rhythm_values](../src/musicxml.cpp#L40) assigns one
  tick to `256th` with divisions 48. A 0.01 s note at 120 BPM exported
  `<duration>1</duration><type>256th</type>` without time modification.
  `1/48` of a quarter is not a 256th note (`1/64` of a quarter).
  Arbitrary residual ticks can also appear in rests/decomposed durations.
- **Proposed solution:** Use a representable timing lattice or exact
  type/dot/time-modification arithmetic for every token, then test semantic
  duration equality rather than only XML parsing.
- **Dependencies / regression risks:** Preserve measure totals and ties;
  any grid adjustment must bound/export its timing displacement.
- **Acceptance:** For every note/rest, notated type, dots and time
  modification equal `duration/divisions` exactly; all voices sum to the
  measure duration in adversarial off-grid and cross-bar fixtures.

### F14 - Public score/API invariants are weaker than CLI validation

- **Severity / impact:** Medium API robustness risk; source-based, not
  invalid-floating-point crash-tested.
- **Source / evidence:** [public score/export APIs](../include/transcribe/musicxml.hpp)
  accept arbitrary notes/metadata. [write_musicxml](../src/musicxml.cpp#L355)
  clamps tempo but does not reject NaN and multiplies unrestricted positive
  meter numerators; [write_midi](../src/musicxml.cpp#L467) rounds times/
  confidence and clamps pitches/ticks. Public analysis thresholds also
  have inconsistent finite/range checks. CLI checks do not protect library
  users or all edited inputs.
- **Proposed solution:** Define small shared validators for finite
  timestamps/durations/confidence/tempo, MIDI ranges and bounded meters.
  Reject invalid values explicitly; document any deliberate clamping.
- **Dependencies / regression risks:** Preserve public overloads and valid
  outputs. Invalid inputs previously normalized may now throw; specify
  behavior rather than hiding it in a new silent fallback.
- **Acceptance:** Table-driven library tests cover NaN/infinities, negative
  times/durations, out-of-range pitches and overflow-size meters. Invalid
  input fails before creating/truncating destination files.

### F15 - Tab alternatives omit constraints; fallback can discard explicit edits

- **Severity / impact:** Medium; reproduced fingering/export defects.
- **Source / evidence:** [primary assignment](../src/guitar_tab.cpp#L274)
  passes held strings and fixed positions to `assign_group`;
  [alternative generation](../src/guitar_tab.cpp#L329) does not.
  With E4 held on string 2/fret 5, the next G4's first alternative used
  string 2/fret 8 during that hold. In a simultaneous B3/E4/unplayable
  MIDI-20 group, fallback assigned B3 to string 2 and dropped an explicit
  E4 override on string 2/fret 5, returning unassigned without an error.
- **Proposed solution:** Reuse one constraint-aware solution set for
  primary/alternatives; reserve explicit positions before partial fallback
  and report unplayable/dropped notes distinctly.
- **Dependencies / regression risks:** Preserve valid existing fingerings;
  heuristic rankings may change. Partial playability is useful, but must
  not silently overrule a validated explicit edit.
- **Acceptance:** Every alternative obeys held-string/override constraints;
  valid explicit positions survive unrelated unplayable notes or cause a
  precise error. Property tests verify pitch/tuning/capo/string consistency.

### F16 - Saved unavailable model settings can disable usable GUI workflows

- **Severity / impact:** Medium; confirmed conditional capability bug.
- **Source / evidence:** [native preferences/validation](../src/gui.cpp#L231)
  restores a model path, disables its controls without ONNX, then rejects
  the nonempty path. [WebView preferences](../ui/app.js#L68) similarly
  restore disabled model/polyphony values, which
  [native message validation](../src/gui_webview.cpp#L731) rejects.
- **Proposed solution:** Make submitted effective settings capability-aware;
  ignore/clear unavailable active choices while preserving preferences for
  a later capable build.
- **Dependencies / regression risks:** Test migration both directions;
  do not permanently erase useful model settings merely by opening the
  lightweight build.
- **Acceptance:** Both GUIs with saved ONNX settings can transcribe a WAV
  in a non-ONNX build without editing preference files; ONNX builds retain
  their compatible preferences.

### F17 - Edited export uses stale tuning/capo and other output options

- **Severity / impact:** Medium; confirmed request-state mismatch.
- **Source / evidence:** [editor export payload](../ui/editor.js#L842)
  sends notes/positions/metadata/paths, not current tuning, capo,
  alternative count or expressive timing. Controls remain active and
  tuning/capo can change positions. [export-edits](../src/gui_webview.cpp#L916)
  copies `last_request` and writers use those old options at lines 983-995.
- **Proposed solution:** Snapshot and validate current export settings in
  the message, or explicitly freeze unsupported changes. Prefer one
  authoritative request snapshot per export.
- **Dependencies / regression risks:** F14/F15 validation and UI state
  tests; stale tuning can turn a previously valid position into a mismatch.
- **Acceptance:** Change each export option after transcription, export,
  and verify current values in MusicXML/tab; unchanged-state output
  remains compatible. Reject invalid combinations before writing anything.

### F18 - Editing history and live-update fingering can become stale

- **Severity / impact:** Medium; confirmed conditional editor bugs.
- **Source / evidence:** [live render updates](../ui/editor.js#L692)
  recalculate positions only when note count changes. Same-count changed
  pitches/order can retain old positions. Alternative-pitch edits at
  lines 450-455 and keyboard edits at 932-945 push undo snapshots but do
  not clear redo history, unlike other edit paths.
- **Proposed solution:** Recompute affected positions for incoming
  transcription batches; centralize edit commits so every divergent edit
  invalidates redo. Do not overwrite deliberate post-transcription edits.
- **Dependencies / regression risks:** Separate live state from committed
  user state; F17 exports rely on coherent note/position snapshots.
- **Acceptance:** Same-count changed-pitch/order updates yield valid
  positions; Undo -> alternate/keyboard edit always disables Redo and
  cannot restore an abandoned future.

### F19 - GUI cancellation/liveness and asynchronous startup need explicit contracts

- **Severity / impact:** Medium confirmed liveness limitations; High
  potential lifetime impact for the unverified startup race.
- **Source / evidence:** [run_transcription](../src/gui_transcription.cpp#L29)
  checks stop at entry and passes it to capture, but not file decode/
  streaming/offline analysis. Close defers until completion.
  [WebView startup callbacks](../src/gui_webview.cpp#L125) retain
  `AppState&`, while `WinMain` owns state on its stack (line 1412).
  If completion posting fails, ownership is safe but `busy`/deferred close
  have no completion transition.
- **Proposed solution:** Add bounded cancellation points without changing
  successful outputs; give async startup an independently safe lifetime/
  cancellation mechanism; handle failed completion delivery explicitly.
- **Dependencies / regression risks:** Shutdown ordering, COM thread
  affinity, F05 capture cleanup. Do not infer UAF from normal posted
  worker completion: that path joins correctly.
- **Acceptance:** Automated close during startup, decode, analysis,
  capture and export terminates without late state access, indefinite busy
  state or lost error; cancellation response budget is declared/measured.

### F20 - Native polyphonic checkbox can silently run monophonic analysis

- **Severity / impact:** Medium; confirmed native/WebView validation drift.
- **Source / evidence:** [native checkbox submission](../src/gui.cpp#L371)
  permits checked "Polyphonic ONNX" without a model in an ONNX build.
  [run_transcription](../src/gui_transcription.cpp#L99) selects built-in
  analysis without propagating `AnalysisOptions.polyphonic`. WebView
  explicitly rejects polyphony without a model at lines 737-739.
- **Proposed solution:** Add common model-required validation for this
  ONNX-labeled control. Exposing built-in spectral polyphony is a separate,
  intentional UI feature, not a silent reinterpretation of the checkbox.
- **Dependencies / regression risks:** F16 and capability tests. Both
  GUIs intentionally label their current control as model-only; the README
  documents model-free polyphony for the CLI, not GUI parity.
- **Acceptance:** Both GUIs reject checked/no-model consistently or offer
  an explicitly selected built-in mode; valid model requests still reach
  ONNX with the requested polyphony.

### F21 - Custom canvas notation and exported notation are not equivalent

- **Severity / impact:** Medium documented limitation and UX assurance gap.
- **Source / evidence:** [noteX](../ui/editor.js#L113) positions in
  seconds. [drawStaff/glyph selection](../ui/editor.js#L137) draws a fixed
  staff, heuristic noteheads/flags and sharp-only spelling, without
  persistent voices, ties, tuplets or key-aware accidental carry.
  MusicXML performs separate quantization, ties and voice construction.
- **Proposed solution:** Keep the lightweight editor, label performance-
  time versus notation views, and add small rendering/semantic fixtures
  for the supported subset. Share only necessary spelling/timing metadata.
- **Dependencies / regression risks:** F12/F13 and DOM/canvas behavior tests.
  Do not replace the UI with an engraving framework in this phase.
- **Acceptance:** Supported fixtures render/edit with stable note identity
  and pitch; unsupported ties/tuplets/key spelling are disclosed. Export
  differences are testable and do not masquerade as an exact score preview.

### F22 - Multiple output files can leave partial or overwritten results

- **Severity / impact:** Medium for partial sets; High for conditional
  batch data loss. Confirmed write ordering/naming, not fault-injected here.
- **Source / evidence:** [CLI writes](../src/main.cpp#L696),
  [shared GUI writes](../src/gui_transcription.cpp#L131), and
  [edited writes](../src/gui_webview.cpp#L983) write sequentially without a
  transaction. [process_batch](../src/main.cpp#L809) uses only `stem()`
  for every output: `take.wav` and `take.flac` collide with FFmpeg enabled.
- **Proposed solution:** Preflight collisions before processing; preserve
  legacy names when unique. Stage writes to sibling temporary files and
  report exact partial completion; use a tested promotion/rollback policy.
- **Dependencies / regression risks:** F14 validators and filesystem
  semantics on Windows; a group of renames is not inherently atomic.
  Preserve existing successful-file recovery in batch mode.
- **Acceptance:** Same-stem inputs never overwrite each other silently;
  injected failure on each output leaves prior destinations unchanged or
  explicitly reports the precise partial set. No false overall success.

### F23 - Evaluation identity can misattribute a supplied model

- **Severity / impact:** Medium reproducibility defect; confirmed.
- **Source / evidence:** [evaluator model metadata](../python/evaluate_guitarset.py#L398)
  always records Spotify Basic Pitch and its pinned commit, while
  `--model` accepts another file. SHA-256 is recorded, but binary version/
  hash/source revision for the transcriber is not.
- **Proposed solution:** Restrict the named benchmark to the bundled
  digest or accept explicit model identity; record executable identity,
  effective flags, build configuration and evaluator revision.
- **Dependencies / regression risks:** Preserve/version result JSON
  schema; do not relabel an arbitrary replacement as the pinned model.
- **Acceptance:** Bundled runs retain verified attribution; different
  models fail clearly or have supplied/unknown provenance. Every new
  report identifies the binary and can reproduce its effective settings.

### F24 - FFmpeg flush errors can become success; decode allocation is repetitive

- **Severity / impact:** Medium error-propagation defect; Low performance
  opportunity. Not locally exercised with an FFmpeg-enabled build.
- **Source / evidence:** [resampler flushing](../src/ffmpeg_audio.cpp#L197)
  breaks on `converted_samples <= 0`, treating a negative FFmpeg error
  like normal completion, unlike the checked conversion in the decode loop.
  Conversion vectors are allocated per received frame and flush iteration
  (lines 153/193); the final decoded buffer remains full-input.
- **Proposed solution:** Distinguish negative error from zero drain result;
  then reuse conversion capacity after measuring allocations. Preserve
  packet/frame RAII and complete decoder/resampler flushing.
- **Dependencies / regression risks:** FFmpeg-only fixtures/error injection
  and actual library-version coverage. Do not change default WAV behavior.
- **Acceptance:** Injected flush errors yield the FFmpeg error text and
  failure; supported compressed fixtures have expected final sample count.
  Buffer reuse measurably reduces allocation count without output changes.

### F25 - Existing tests do not gate the most important accuracy/UX contracts

- **Severity / impact:** Medium assurance gap; tests passing is insufficient
  evidence of event accuracy, rendering, concurrency or release readiness.
- **Source / evidence:** [unit tests](../tests/transcribe_tests.cpp#L74)
  emphasize synthetic presence/substring checks; MIDI checks only `MThd`.
  [CLI smoke](../tests/cli_export_smoke.py#L94) parses XML/compares pitch
  sets but not complete durations/voices or MIDI events.
  [UI smoke](../tests/ui_smoke.py) checks assets/wiring and optional syntax,
  not behavior. [real-audio smoke](../tests/real_audio_smoke.py) is optional,
  merely requires notes, and has no annotation-based quality threshold.
  CMake silently omits Python integrations when Python is missing.
- **Proposed solution:** Add focused semantic, event, editor, decoder and
  concurrency regression tests before fixes; make CI's expected test
  inventory explicit. Keep labeled accuracy/performance evaluation optional
  for ordinary dependency-light development, but gate quality claims on it.
- **Dependencies / regression risks:** F01 fixes optimization comparability.
  Avoid hardware/internet requirements or fragile wall-clock limits in the
  default unit suite.
- **Acceptance:** CI checks expected test registration and skip reasons;
  deterministic fixtures cover each confirmed defect. MIDI parsing checks
  note order/times/metadata; threaded SPSC stress and sanitizer jobs pass;
  real-audio reports include event and frame metrics with provenance.

### F26 - Manual metadata overrides are labeled as inferred at confidence 1

- **Severity / impact:** Medium semantic/API ambiguity; confirmed.
- **Source / evidence:** [CLI overrides](../src/main.cpp#L674) set
  `*_inferred=true` and confidence 1.0 for user tempo/key/meter choices.
  JSON has only the boolean/confidence, so consumers cannot distinguish
  user assertions from successful heuristic inference.
- **Proposed solution:** Add optional source fields (`default`, `inferred`,
  `user`) and document legacy booleans rather than breaking existing JSON.
  Apply the same provenance policy to GUI exports.
- **Dependencies / regression risks:** F11/F14; consumers/tests may rely on
  the existing booleans and override precedence.
- **Acceptance:** User overrides, inferred values and defaults are
  distinguishable in output; old keys/values remain compatible unless a
  separately versioned schema change is explicitly approved.

### F27 - ONNX session caching does not reload changed model files

- **Severity / impact:** Medium long-lived GUI correctness/resource risk;
  confirmed cache behavior, not a demonstrated current-user failure.
- **Source / evidence:** [load_onnx_session](../src/transcriber.cpp#L583)
  caches sessions by normalized path/provider/device in a static map with
  no eviction or content/mtime identity. Replacing a file at the same path
  does not replace the cached session; selecting many models retains them.
- **Proposed solution:** Define cache identity/lifetime explicitly; add
  bounded retention and a reload/invalidation path when model identity
  changes. Keep reuse for unchanged models.
- **Dependencies / regression risks:** Concurrent in-flight inference must
  retain its own session safely. File timestamps alone may not establish
  identical content; hashing large files on every inference is also costly.
- **Acceptance:** Same model reuses a session; replacing it at the same
  path reloads deterministically; retained sessions meet a documented limit
  without invalidating any active inference.

### F28 - Duplicated policies and unmeasured DSP costs invite drift

- **Severity / impact:** Low maintainability/performance gap; no broad
  rewrite or unmeasured speedup is justified.
- **Source / evidence:** [CLI processing](../src/main.cpp#L543) and
  [GUI processing](../src/gui_transcription.cpp#L29) separately construct
  analysis/model options, override metadata and sequence outputs.
  Fingering is recomputed for notation/tab/alternatives (F15).
  [FFT](../src/transcriber.cpp#L198) recomputes stage roots; uncached
  [spectrogram calls](../src/transcriber.cpp#L291) and YIN calls launch
  per-call workers. Cache keys scan samples; FFT/YIN workspaces are already
  reused per worker, and the spectrogram cache is already bounded at 64 MiB.
- **Proposed solution:** Extract only small demonstrated shared policies/
  validators; profile allocation, hashing, FFT and worker overhead before
  changing scheduling. Do not add an unneeded framework, thread pool or
  third-party FFT dependency.
- **Dependencies / regression risks:** F25 baselines; callback order and
  progress semantics differ between front ends. Caching twiddles or changing
  parallelism can alter memory, rounding and interactive latency.
- **Acceptance:** Each extraction preserves CLI outputs/errors; any
  optimization reports representative Release timings, peak memory and
  output equality, with a predeclared improvement threshold.

### F29 - Public buffer and callback contracts are undocumented

- **Severity / impact:** Medium integration/concurrency risk, not evidence
  that current internal SPSC use is wrong.
- **Source / evidence:** [audio.hpp](../include/transcribe/audio.hpp) does
  not describe SPSC ownership or the lifetime of `AudioBlockView.samples`;
  [transcriber.hpp](../include/transcribe/transcriber.hpp) and
  [capture callbacks](../include/transcribe/asio_capture.hpp) do not state
  callback threads, borrowing, exception/cancellation behavior or safe
  concurrent usage. The block span points at a per-callback decoded vector.
- **Proposed solution:** Document ownership/lifetime/threading and supported
  parameter ranges beside existing APIs; use small validators/tests rather
  than changing the public representation.
- **Dependencies / regression risks:** F05/F14 must make promised error
  behavior true. Do not promise concurrent destruction, MPMC access or
  callbacks retaining borrowed spans.
- **Acceptance:** Every public span/callback has an explicit lifetime/
  thread contract; SPSC stress preserves exact sequence across wrap/full/
  empty transitions, and examples copy data when retaining it.

## 6. Documentation alignment and preserved strengths

| Documentation statement | Implementation alignment / action |
|---|---|
| Default build is lightweight; optional integrations explicit | Accurate for WAV/YIN; preserve it. |
| Release examples use `--config Release` | Generator-dependent; fix F01. |
| ONNX consumes full input; Basic Pitch uses overlapping windows | Accurate but easy to conflate. Windowed `Run` is not bounded end-to-end memory; explain F10. |
| Two-second GUI and 15-second CLI WAV chunks preserve final offline behavior | They preserve the analysis/export pipeline, not proven event-for-event equivalence to full-buffer or each other. Seam merge uses 50 ms matching and can discard unmatched overlap candidates; add boundary tests before asserting equivalence. |
| Built-in spectral polyphony works without ONNX | Accurate for CLI. Both GUI toggles are explicitly model-only; do not imply GUI mode parity. |
| Pitch alternatives/confidences and metadata are heuristic | Accurately disclosed. Retain warnings; add explicit source semantics (F11/F26). |
| MusicXML supports basic tuplets/voices; renderer is not engraving | Accurately qualified; F13 is a genuine arithmetic defect, while F12/F21 are broader limits. |
| Capture ring overflow is explicit | Application-ring overflow is explicit; device-reported overflow is not yet propagated (F06). |
| CI creates ZIP/TGZ packages | True as workflow configuration, not proof of a self-contained full-featured distribution (F02). |
| Do not commit model weights | A bundled licensed model already exists. Clarify the intentional reviewed exception rather than pretending weights are absent. |

Important correct behavior to retain:

- WAV reads bound block allocations, validate truncation/alignment, support
  stated PCM widths/float32, downmix channels, and sanitize nonfinite float
  samples. Extensible WAV is explicitly rejected, not silently decoded.
- FFmpeg packet/frame/codec/resampler resources use RAII and detailed errors
  on most paths; decoder and resampler are flushed.
- Real-time callback work avoids FFT/YIN/GUI operations and dynamic sample
  allocation. Recording/analysis consumers are separated.
- Realtime methods are synchronized; normal capture transfers analysis
  exceptions after joining. Normal GUI worker-close paths join safely.
- ONNX output shape/type checks, Basic Pitch output-head validation and
  unavailable-provider errors are explicit. No unguarded output tensor
  indexing defect was established by this audit.
- Spectrogram workspaces are reused and the cache has a 64 MiB retention cap.
  Session reuse already exists; proposing a session cache from scratch would
  duplicate existing functionality.
- MusicXML and primary ASCII tab share selected fingering; existing overloads
  preserve simple callers. MIDI sorts same-tick note-offs before note-ons
  and guards four-byte variable-length delta limits.
- Batch mode continues after individual failures and returns failure for a
  partial batch. Preserve successful per-file results while fixing collisions.
- The README already discloses low event F1, heuristic metadata/confidence,
  approximate playback, rendering limits, full-buffer ONNX and possible
  training overlap. Do not report these disclosures as concealment.

## 7. Remaining verification limits

Prioritize concrete defects over speculative rewrites. GUI startup lifetime,
device error/overflow paths, stream exceptions, generic-model resource
limits, actual FFmpeg failure injection, concurrency sanitizers and
clean-machine packages still need targeted validation. No GPU capability,
hardware latency guarantee, calibration accuracy, independent real-world
AMT quality, export-schema conformance, or visual correctness is certified
by the tests run for this document.
