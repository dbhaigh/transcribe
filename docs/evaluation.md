# Quality and performance evaluation

The evaluation framework exercises the existing public C++20 engine without
changing production APIs or CLI behavior. Python 3.10+ and the standard
library suffice; synthetic runs do not download models, recordings or Python
packages. Catch2 remains the existing test dependency fetched at configure
time when tests are enabled.

## Build and run

Single-configuration generators (MinGW Makefiles, Ninja, Unix Makefiles):

```powershell
cmake -S . -B build-evaluation -DCMAKE_BUILD_TYPE=Release -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-evaluation --parallel
ctest --test-dir build-evaluation -C Release --output-on-failure
```

For Visual Studio, configure with `-DTRANSCRIBE_BUILD_TESTS=ON`, build with
`--config Release`, and run CTest with `-C Release`. Its executable is in
the `Release` subdirectory. `--config Release` alone does not optimize a
Makefile build.

When tests are enabled, CMake builds `transcriber-evaluate`. CMake generates
`evaluation-build-Release.json` alongside the build files even when tests are
disabled, so the legacy CLI workflow does not require Catch2. This metadata
identifies the compiler/version, build type, flags, generator and enabled
integrations. The runner links the same core library used by the product;
it does not use a separate transcription algorithm.

From the repository root, use the matching runner and metadata:

```powershell
python .\python\evaluate.py `
  --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --backend yin --mode streaming `
  --output-json .\build-evaluation\evaluation\baseline-yin.json `
  --output-text .\build-evaluation\evaluation\baseline-yin.txt
```

On Linux, omit `.exe`. For Visual Studio, use
`.\build-evaluation\Release\transcriber-evaluate.exe`; the metadata remains
at `.\build-evaluation\evaluation-build-Release.json`. Reports reject metadata
whose runner path differs from the requested executable. Rebuild before
evaluating changed source; the report records the binary SHA-256 separately
from the current working tree's revision, dirty state and relevant source
digests. A source checkout identity alone does not establish binary identity.

Other supported combinations:

```powershell
# Built-in spectral polyphony, full-buffer offline analysis
python .\python\evaluate.py --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --backend spectral --mode full --output-json .\build-evaluation\evaluation\spectral.json

# Real-time monophonic analysis fed deterministic 10 ms blocks, without hardware
python .\python\evaluate.py --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --mode realtime --output-json .\build-evaluation\evaluation\realtime.json

# Exercise shorter chunk seams independently of the CLI's 15 s default
python .\python\evaluate.py --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --chunk-seconds 1 --overlap-seconds 0.25 `
  --output-json .\build-evaluation\evaluation\short-chunks.json
```

`--fast` selects the existing built-in preview. `--serial` disables the
offline `AnalysisOptions.parallel` option. Realtime is always the existing
monophonic engine (40 ms latency setting, 100 Hz update rate, confidence
threshold 0.45 unless the engine's public defaults change); offline defaults
remain confidence 0.25 and maximum six pitches. The report records both
offline and realtime settings rather than pretending they are interchangeable.
Backend/mode/provider combinations that cannot be honored fail explicitly.

Optional ONNX evaluation requires an ONNX-enabled build:

```powershell
python .\python\evaluate.py --runner .\build-onnx\transcriber-evaluate.exe `
  --build-info .\build-onnx\evaluation-build-Release.json `
  --backend onnx --mode full --model .\python\models\basic_pitch.onnx `
  --model-identifier spotify-basic-pitch-pinned --provider cpu `
  --output-json .\build-onnx\evaluation\basic-pitch.json
```

ONNX runs use polyphonic independent-probability decoding. The model content
digest is always recorded, including when a friendly identifier is supplied;
arbitrary overrides are not attributed to Spotify. CUDA/DirectML/ROCm need
the corresponding enabled build, runtime and hardware. The empty-input ONNX
fixture is explicitly marked `inference_skipped`: the runner does not call
an API that rejects empty input, and the summary counts those skips. Other
errors are failures, not empty-note successes. Built-in empty/silence cases
actually execute and are covered by Catch2.

## Versioned synthetic ground truth

[synthetic_corpus.py](../python/synthetic_corpus.py) defines corpus v1:
16 PCM16 mono clips at 22,050 Hz with seed 2026.

| Fixture | Purpose |
|---|---|
| monophonic | Separated harmonic notes |
| repeated | Same-pitch attacks separated by silence |
| silence, empty, very_short | No events, zero frames, and 10 ms audio |
| octaves | Register/octave transitions |
| vibrato | Five-Hz, 35-cent pitch modulation around a labeled MIDI pitch |
| chord, overlap | Simultaneous and staggered overlapping notes |
| decay, attack | Exponential decay and a slow attack |
| noise, noise_only | Signal plus noise, and noise false positives |
| reverberation | Deterministic delayed copies with known gains |
| clipping | Deliberate hard clipping |
| long_boundary | 16.5 s audio with a note spanning the default streaming seam |

Each note carries MIDI pitch, onset, offset, duration, synthesis velocity and
instrument identifier. Labels describe dry oscillator activation, not a
psychoacoustic estimate of audibility. Reverb tails are not extra reference
notes. Synthesis velocity controls gain; it is not a calibrated MIDI-velocity
or real-instrument loudness benchmark.

An explicit integer PRNG determines noise. Generated WAVs and the entire
manifest have SHA-256 fingerprints. Two generations are compared in tests.
Reports embed per-clip reference/prediction events, audio fingerprints and
corpus identity. A generator/effect/label change must bump the corpus version
and establish a new baseline; do not silently compare different waveforms.
Floating-point `sin`/`exp` implementations can differ across platforms at
rounding boundaries. The audio/corpus digest is authoritative: if it differs,
the comparison refuses to call it the same corpus.

To retain inspectable fixtures outside normal temporary evaluation:

```powershell
python .\python\synthetic_corpus.py --output .\build-evaluation\corpus
```

No generated WAVs need to be committed. The older checked-in
[sample generator](../samples/generate_samples.py), sample files and expected
pitch lists are preserved.

## Metric definitions

Shared implementation: [evaluation_metrics.py](../python/evaluation_metrics.py).
The historical GuitarSet evaluator imports the same core scoring functions.
The new runner emits successful empty predictions directly, without parsing
the production CLI's error text.

- **Strict note-event precision/recall/F1:** maximum-cardinality one-to-one
  matching with exact integer MIDI pitch, onset tolerance 50 ms and offset
  tolerance `max(50 ms, 20% of reference duration)` by default.
- **Onset + pitch:** same matching without the offset condition. Matching
  uses iterative augmenting paths; eligible matches prefer closer timing but
  do not optimize total timing error. Boundary comparisons allow a 1e-12 s
  floating-point epsilon. Set `--onset-tolerance`, `--offset-tolerance`,
  `--offset-ratio` and `--frame-step` to declare a different protocol.
- **Pitch accuracy:** exact-pitch fraction among pitch-independent one-to-one
  onset-time matches. This measures pitch correctness conditional on timing
  matches, **not** pitch recall or cents accuracy. Misses remain visible in
  event recall. Timing ties prefer equal pitch; other same-onset chord
  assignments can be ambiguous. Use chord
  recall and frame metrics alongside this diagnostic.
- **Onset/offset error:** absolute error distributions over onset+pitch
  matches, including offset errors that fail the strict offset tolerance.
  Unmatched notes do not enter these distributions; never interpret low
  timing error without checking recall.
- **False positives / missed notes:** unmatched predictions/references under
  the strict event protocol, also available per clip.
- **Chord-note recall:** onset+pitch recall for reference events overlapping
  at least one differently pitched reference event. This includes staggered
  overlaps, not just identical-onset chords.
- **Frame precision/recall/F1:** pitch occupancy at 10 ms cell centers,
  merging same-pitch overlapping predictions. The final partial cell counts
  once, not fractionally. True negatives are not included. Frame identity is
  separate from note identity; fragmentation can have high frame F1 and low
  event F1.
- **Aggregation:** event/frame counts and chord/pitch denominators are
  micro-aggregated. Null denotes no eligible timing/pitch/chord observations.
  All-empty P/R/F1 is defined as 0, not a fabricated perfect score. Per-clip
  results show silence/noise false positives separately.

The metric suite checks perfect/empty/duplicate/wrong-pitch/chord cases,
explicit tolerances, augmenting-path results against brute force, frame
interval scoring against a dense reference, and malformed labels.
Frame scoring merges sparse intervals, avoiding one dense Boolean array per
pitch per long recording.

## Timing and reproducibility

Default runs use **one discarded warmup and three measured repetitions**.
Every invocation is a fresh process, so discarded runs can warm filesystem/
OS caches but **do not warm an in-process ONNX session**. Predictions must
agree exactly across measured repetitions; nondeterminism fails the run.
There is no real-time sleeping in the synthetic realtime feed.

Reports distinguish:

- Engine steady-clock decode, analysis and total elapsed seconds. Streaming
  decode is included in analysis; separate decode is `null`, not zero cost.
  Built-in stage timings are supplied where supported, otherwise `null`.
- Parent `perf_counter` process-wall time, including startup/model loading and
  JSON emission. Offline engine totals exclude score exporters.
- Wall time to first nonempty streaming update or realtime event. No update
  is `null`; full-buffer offline mode does not claim one.
- Realtime input-time analysis latency: `detected_at - estimated onset`,
  pooled for emitted onset events. This is not ground-truth onset error,
  device/driver/GUI latency or a physical end-to-end measurement.
- Per-clip real-time factors and corpus-duration-weighted engine real-time
  factor. Empty clips have no per-clip factor.

Every benchmark includes compiler/version/path, configuration/flags, feature
switches, binary digest, model identifier/digest, provider/device setting,
audio sample rates, quality/chunk/threshold options, protocol tolerances,
source identity, Python/platform/CPU information, repetitions and clock
definitions. There are no hardcoded expected engine-accuracy numbers.

For meaningful performance comparisons: use the same machine, toolchain,
Release flags, power policy, CPU load, corpus and backend configuration;
record extra environment constraints in experiment notes. The framework
does not control turbo, scheduling, antivirus, thermal state or system load.
CTest uses one measured repetition/no warmup as a correctness smoke, not a
statistically qualified speed gate. CI uploads its JSON/text reports.

### Compare a future engine against the baseline

```powershell
python .\python\evaluate.py `
  --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --compare .\build-evaluation\evaluation\baseline-yin.json `
  --output-json .\build-evaluation\evaluation\candidate-yin.json `
  --output-text .\build-evaluation\evaluation\candidate-yin.txt
```

The corpus digest, model identity, metric tolerances, schema, timing protocol
(including repetition/warmup counts) and relevant engine configuration must
match. Build/environment differences are disclosed,
not silently assumed equivalent. The output reports event/frame F1 deltas and
median process-time ratio. It does not require equal binary hashes because
the purpose is to compare changed engines. An optional
`--max-event-f1-drop 0.01` fails if strict F1 drops more than one percentage
point; it is a caller-selected gate, not a claimed baseline accuracy.
No default noisy timing failure threshold is imposed.

Measured reference snapshots and their limitations are described in
[the baseline record](../python/evaluation/README.md). Do not overwrite the
historical GuitarSet results with synthetic or unverified values.

## Licensed real-audio workflow

GuitarSet v1.1.0 provides 360 microphone recordings with aligned `note_midi`
labels. Source: Qingyang Xi et al., ISMIR 2018, DOI
[10.5281/zenodo.3371780](https://doi.org/10.5281/zenodo.3371780), **CC BY 4.0**.
Retain attribution and consult the dataset's annotation caveats. The archives
are deliberately not downloaded automatically or redistributed here.

Follow the existing [download/provenance instructions](../python/README.md#real-recording-evaluation-guitarset)
to place `audio_mono-mic.zip` and `annotation.zip` outside the repository.
The new workflow verifies the pinned archive MD5s before evaluation, records
their actual identities, and additionally hashes extracted WAV files. It has
no checksum-bypass option.

```powershell
python .\python\evaluate.py --corpus guitarset `
  --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --audio-zip "$env:TEMP\guitarset-audio-mono-mic-v1.1.0.zip" `
  --annotations-zip "$env:TEMP\guitarset-annotation-v1.1.0.zip" `
  --backend yin --mode streaming --limit 24 --seed 2026 `
  --output-json .\build-evaluation\evaluation\guitarset-yin-subset.json
```

Omit `--limit` for all recordings. Change to a capable ONNX runner and add
`--backend onnx --mode full --model ... --provider cpu` for a model run, or
use `--backend spectral --mode full` without a model. The exact selected
track list, reference events, archive/audio hashes and configuration determine
corpus identity. Missing files/ambiguous archive basenames/malformed labels or
engine failures stop the evaluation instead of becoming empty successes.
Transient extraction is cleaned automatically; all dataset data stays outside
the repository except the report's annotation-derived event values.

GuitarSet references identify guitar but do not supply calibrated velocity
here (`velocity: null`). Do not invent unavailable annotations. It is a
guitar-only diagnostic dataset, not evidence for vocals, piano, arbitrary
mixes, dense ensemble AMT or commercial score reconstruction.
Possible overlap with Basic Pitch training is unresolved. Report it as such,
not as an independent held-out benchmark. Freeze recording/performer splits
before tuning; measure a separately licensed independent holdout before
making generalization claims.

The historical [GuitarSet evaluator](../python/evaluate_guitarset.py) and
its CLI remain available. New reports from that entry point add build/binary
identity, sample rates and one process-wall timing per clip, including score
exports. Pass `--build-info` if the matching CMake metadata cannot be uniquely
auto-detected; reconfigure the matching CMake build to generate it (tests are
not required for this legacy workflow). Its timing protocol
is intentionally distinct from the new runner's repeated engine-only timings.
The old stored report is preserved without invented metadata.
Use the new workflow for full quality diagnostics, repeated performance and
compatible baseline comparisons. The stored historical 14.58% strict event F1 / 72.97% frame F1
was not remeasured by this framework's synthetic tests.

## Regression coverage and known limits

Existing tests and public overloads are preserved. Added Catch2 regressions
cover empty/silent/sub-window inputs, clipping, malformed/unsupported WAV,
disabled-format handling, invalid analysis/realtime/streaming parameters,
31 s held-note streaming with bounded decoder blocks, and onset sweeps around
short-chunk core boundaries.

The sweep exposed current held-note fragmentation near a seam: a one-note
assertion failed with two events for an onset at 0.70 s. This phase does not
hide that behavior by changing the DSP. Structural tests bound event count,
check pitch/timing validity and retained duration; quality reports count
fragmentation as extra/unmatched events. Sweep accuracy should improve in a
separately measured engine fix, not by relabeling the ground truth.

Synthetic fixtures are intentionally simple oscillators/effects. They cannot
substitute for representative recordings or establish model calibration.
Timing diagnostics do not verify engraving, output-schema conformance,
hardware capture reliability or GUI rendering. Optional model/provider,
Linux, hardware and real-data checks must be reported as unexecuted when
their environments are unavailable.
