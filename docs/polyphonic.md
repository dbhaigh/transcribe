# Polyphonic activation and model adapters

The fast monophonic YIN path is unchanged. Model-free builds remain supported.
The existing spectral decoder and Basic Pitch stitching remain the defaults;
the new alternatives are explicit opt-ins. In particular, **the competing
spectral decoder is experimental, not a demonstrated real-guitar upgrade**.
Its real-data regressions below are why it is not enabled by default.

For bounded long-file processing and its separate overlap/consistency limits,
see [Long recording pipelines](streaming.md).

## Frame-by-pitch contract

[polyphonic.hpp](../include/transcribe/polyphonic.hpp) defines
`PitchActivationGrid`. It contains:

- Inclusive integer `minimum_midi`/`maximum_midi`, with contiguous pitch columns.
- Frame-major `values[frame * pitch_count + pitch - minimum_midi]`.
- Explicit input-relative timestamps in seconds, an analysis-window duration,
  and `hop_seconds`; `frame_rate_hz()` is exactly `1 / hop_seconds`.
  Times denote **window starts**, not arrival times or musical beats. Pointwise
  model activity has zero window duration.
- `EvidenceSemantics`, method identity and a separately declared `PitchObjective`.
  Raw activations, normalized salience and calibrated probabilities are not
  interchangeable. An unspecified objective does not imply polyphony.
- Optional same-shape onset and local fractional-MIDI contour heads. Missing
  heads are empty, not fabricated probability arrays.

`pitch_activation_grid(tracks, objective)` converts aligned fixed-pitch evidence
tracks. Missing/duplicate pitch columns, inconsistent timestamps, head shapes,
semantics, nonfinite values and invalid geometry throw descriptive exceptions.
No ONNX dependency is introduced. `ModelBackend::infer` populates the optional
`InferenceOutput.activations` for retained polyphonic evidence. YIN retains its
existing monophonic frame evidence instead of pretending to be a multi-pitch
activation model.

```cpp
transcribe::ModelRegistry registry;
auto created = registry.create("builtin:spectral");
if (const auto* error = std::get_if<transcribe::InferenceError>(&created)) {
    throw std::runtime_error(error->message);
}
auto model = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
auto loaded = model->load();
if (const auto* error = std::get_if<transcribe::InferenceError>(&loaded)) {
    throw std::runtime_error(error->message);
}
transcribe::InferenceRequest request;
request.retain_evidence = true;
request.analysis.spectral_decoder =
    transcribe::SpectralDecoder::HarmonicCompetition;
auto result = model->infer(audio, request);
if (const auto* error = std::get_if<transcribe::InferenceError>(&result)) {
    throw std::runtime_error(error->message);
}
// Successful InferenceOutput contains notes, original tracks, and a raw grid.
```

See the [model-interface example](model-inference.md#versioned-pluggable-api)
for session lifetime and provider details.

## Spectral candidate competition

`AnalysisOptions::spectral_decoder` selects `LegacySalience` (default) or
`HarmonicCompetition`. Both use the existing cached Hann STFT, resampling,
instrument ranges, pitch count limits, relative selection threshold and
note-event decoder.

The alternative builds sparse eight-partial magnitude templates for each
permitted fundamental, with harmonic amplitude proportional to `1/h^1.2`.
Each partial includes a Hann main-lobe leakage kernel at the actual fractional
FFT-bin position. Twelve deterministic nonnegative coordinate-descent sweeps
jointly fit the observed spectrum. Shared partial energy is assigned
competitively instead of being summed independently for every candidate.
This can suppress false octaves without an unconditional octave exclusion:
a genuine upper octave with independent residual energy can remain active.
Independent fitted pitch columns are thresholded/ranked without adjacent-pitch
local-maximum suppression, so real semitone dyads can coexist.

The guitar prior still constrains MIDI 40-88 and at most six notes; piano and
voice use their existing limits. No instrument is inferred automatically.
Coefficients are relative harmonic-fit evidence, **not probabilities**.
Selected tracks use `normalized-harmonic-fit`; the original decoder continues
to use `normalized-salience`.

The fixed harmonic envelope is a limitation, especially for different string
positions, resonances, missing fundamentals and distortion. It improves several
controlled mixtures but loses real-guitar recall. A geometric combination with
the old salience was also tried on the separate performer-04 validation subset:
frame F1 fell from 0.6254 to 0.6134 and event F1 from 0.1375 to 0.1259. That
combination was rejected, not made a new default or hidden behind pitch-only
accuracy. Adaptive instrument templates require further independent work.

## Basic Pitch heads and temporal alignment

`OnnxOptions::aligned_model_heads = true` enables the alternative for the pinned
Basic Pitch v1 adapter. The model's independent multi-pitch objective is known
from the explicitly selected adapter, not inferred from the number of classes.

| Head | Named tensor | Required float32 runtime shape | Use |
|---|---|---|---|
| Note | `StatefulPartitionedCall:1` | `[1,172,88]` | Independent per-pitch activity |
| Onset | `StatefulPartitionedCall:2` | `[1,172,88]` | Canonical event retriggers with note-state drop/recovery |
| Contour | `StatefulPartitionedCall:0` | `[1,172,264]` | Local fractional-MIDI evidence, three bins per semitone |

The input remains float32 `[1,43844,1]` at 22050 Hz. The adapter prepends 3840
zeros, trims 15 frames from each edge, advances windows by 36164 samples,
and retains 142 frames per window. **36164 is not `142 * 256`**. Concatenating
these frames on a nominal 256-sample grid produces cumulative drift.

The aligned path instead gives each retained frame its actual sample position
`window_start + (local_frame - 15) * 256`, accounting for the left pad.
It linearly interpolates all available heads onto the same uniform
256-sample grid, beginning at zero and ending strictly before the input
duration. It does not independently trim/regrid the onset or contour heads.
The last padded model window provides right-edge support; padded output
timestamps are never reported beyond the input.

Contour MIDI is a local activation-weighted center across the three bins
surrounding each supported semitone. It is retained in `contour_midi` and
`PitchFrame::fractional_midi`, then in canonical/score evidence. It is not an
instrument label, calibrated pitch probability, or a complete bend-curve
transcription. Event MIDI remains integer. The aligned independent note
selection no longer suppresses one semitone simply because its neighbor has
a larger activation.

Onset evidence is consumed by the existing canonical
[temporal decoder](decoding.md), not treated as equivalent to note-state
activity. Legacy `Note` conversion retains its existing acoustic onset logic;
request `--temporal` in evaluation to measure the canonical event path.
The legacy CLI/export shapes and monophonic algorithm remain unchanged.
GUI evidence collection preserves aligned contour fields when supplied, but
no new experimental GUI controls or live-capture behavior are enabled.

## Explicit model objectives, metadata and errors

[inference.hpp](../include/transcribe/inference.hpp) extends the existing
adapter metadata with objective, trimmed-edge count, output origin, output-head
semantics and a temporal-alignment description. Existing input/output tensors,
rate, window, hop, overlap, pitch range and output frame step remain explicit.
Tensor metadata alone cannot prove a training objective.

For the historical `LegacyPitchV1` adapter, polyphonic capability is now
**unspecified by default**, rather than advertised from a 69/70-class shape.
A model definition or inference request must explicitly declare
`PitchObjective::IndependentMultiPitch` before polyphonic inference.
A monophonic declaration cannot be overridden by a polyphonic request.
This declaration is the caller's responsibility, not automatic verification
of a model's training corpus or loss function.

The CLI accepts `--model-objective independent-multi-pitch` for a documented
multi-label generic model; use `monophonic` for a mutually exclusive objective.
`independent_probabilities` only describes activation interpretation and does
not substitute for an objective declaration. The pinned Basic Pitch adapter
needs no extra declaration.

Load-time validation checks waveform type/rank/batch/window and every recognized
Basic Pitch head, including optional onset/contour dimensions and element types.
Errors name the incompatible head and expected shape. Runtime validation also
checks tensor sizes and nonfinite activations. A generic pitch model cannot
request aligned Basic Pitch onset/contour processing: it gets an explicit
unsupported-contract error. Model definitions with conflicting objectives fail
registration. No failed provider selection silently falls back to CPU; CPU,
CUDA, DirectML and ROCm remain explicitly selected as before.

## Paired quality measurements

The baseline is the immediately preceding canonical decoder and original
spectral/Basic Pitch adapter, preserved as a separate executable with its
SHA-256 in the reports. Both sides use identical input hashes, CPU provider,
model digest, existing thresholds and scoring functions. Strict event matching
uses exact MIDI, 50 ms onset tolerance and offset tolerance
`max(50 ms, 20% of reference duration)`. Frame occupancy is scored at 10 ms.
Runs are Release/Clang 23.1.1 on Windows, one discarded warmup and three
fresh-process measurements per clip. Timing medians pool unequal-length clips;
they are not a hardware latency or general speedup claim.

### Real annotated recordings

GuitarSet v1.1.0, Qingyang Xi et al., ISMIR 2018,
[DOI 10.5281/zenodo.3371780](https://doi.org/10.5281/zenodo.3371780), CC BY 4.0.
The existing evaluator verified both pinned archive MD5s and used seed 2026,
performer-05 `test` partition, `--limit 12`: 339.844 seconds, 1764 reference
events. Corpus SHA-256:
`ff8fc998cfb5293040d4a86d1b1ec1f3670fd9e211652163e51f57c84fbbd088`.
Performer-04 validation is disjoint from performer-05, but the test subset was
inspected during development and is **not an untouched hold-out**.
Basic Pitch may have trained on GuitarSet: these results are real-recording
diagnostics, **not proof of independent generalization**. No dataset audio is
committed or redistributed.

| Backend | Strict event precision before -> after | Recall before -> after | Event F1 before -> after | Frame F1 before -> after |
|---|---:|---:|---:|---:|
| YIN control | 0.2173 -> 0.2173 | 0.0811 -> 0.0811 | 0.1181 -> 0.1181 | 0.2983 -> 0.2983 |
| Spectral competition | 0.1088 -> 0.1171 | 0.1389 -> 0.1253 | 0.1220 -> 0.1211 | 0.5973 -> 0.5639 |
| Basic Pitch aligned heads | 0.2434 -> 0.4726 | 0.2285 -> 0.4399 | 0.2357 -> 0.4557 | 0.7583 -> 0.7964 |

Basic Pitch frame precision/recall changes from 0.7802/0.7375 to
0.8169/0.7768. Spectral changes from 0.5588/0.6415 to 0.5658/0.5621:
its recall loss is **not** an acceptable reason to promote its precision gain.

| Real-data diagnostic | Spectral before -> after | Basic Pitch before -> after |
|---|---:|---:|
| Fragmented reference notes | 94 -> 74 | 304 -> 172 |
| Extra fragments | 109 -> 87 | 335 -> 198 |
| Merged predictions | 215 -> 177 | 324 -> 220 |
| False positives/minute | 354.163 -> 294.135 | 221.219 -> 152.894 |
| Mean onset error (ms) | 24.555 -> 25.409 | 22.330 -> 19.992 |
| Mean offset error (ms) | 187.393 -> 199.236 | 105.382 -> 104.645 |
| Median analysis time (ms) | 248.887 -> 297.413 | 1049.701 -> 980.107 |
| Analysis p95 (ms) | 370.963 -> 459.359 | 1631.731 -> 1499.804 |
| Corpus engine real-time factor | 0.00938 -> 0.01079 | 0.03931 -> 0.03536 |

Timing errors are conditional on onset+pitch matches; the matched populations
change. Basic Pitch's median offset error actually increases
40.726 -> 44.259 ms despite its mean and event accuracy improving.
No single aggregate score replaces these diagnostics.

Basic Pitch has no per-clip strict event F1 loss in this subset, but frame F1
regresses on `05_Jazz1-200-B_solo` (0.8294 -> 0.8232) and
`05_SS1-100-C#_comp` (0.8567 -> 0.8502).
Spectral strict-event losses include `05_BN2-131-B_comp`,
`05_Jazz1-200-B_solo`, `05_Jazz3-137-Eb_solo`,
`05_Jazz3-150-C_comp` and `05_Rock2-85-F_comp`; frame losses include several
comping tracks. Full per-clip predictions and metrics remain in the JSON.

**Decision:** retain the spectral baseline as default. Make aligned Basic Pitch
heads available explicitly, but do not silently change all CLI/GUI/live
workflows on the strength of one potentially leaked 12-recording experiment.

### Controlled regression fixtures

Corpus v3 extends the existing framework, rather than replacing it. Its first
22 fixtures are byte/annotation-identical to v2; v1/v2 identities are preserved.
Eight additions cover dyads, dense six-note chords, octave intervals, bass plus
melody, clipped/distorted harmonic mixtures, overlapping decays, simultaneous
semitones and strong harmonic false-positive challenges. The distortion fixture
is **synthetic**, not an actual distorted guitar recording.

The 30 clips contain 51 reference events and 71.91 seconds of audio.
SHA-256:
`68fc63fd97fb9b88fced4b99a7f3d03817116859c2af8205992ff51a25abc677`.

| Backend | Event F1 before -> after | Frame F1 before -> after | Event TP/FP/FN before -> after |
|---|---:|---:|---|
| YIN control | 0.4200 -> 0.4200 | 0.6288 -> 0.6288 | 21/28/30 -> 21/28/30 |
| Spectral competition | 0.7885 -> 0.7850 | 0.8879 -> 0.9246 | 41/12/10 -> 42/14/9 |
| Basic Pitch aligned heads | 0.7619 -> 0.7925 | 0.8757 -> 0.8909 | 40/14/11 -> 42/13/9 |

Spectral strict event F1 regresses on the dense and distorted fixtures even
though aggregate frame F1 improves. It also fragments one reference note
(baseline zero). Basic Pitch has no per-clip strict event F1 loss here.
The spectral pitch-bend and release-tail frame regressions remain visible.
These are development/regression measurements, not independent accuracy claims.

## Reproduce and inspect

Follow [the existing build and dataset instructions](evaluation.md). An
ONNX-enabled build compares YIN, spectral and the bundled model on identical
clips; a model-free build remains useful for both built-ins.

```powershell
python .\python\evaluate.py `
  --runner .\build-onnx-asio-mingw\transcriber-evaluate.exe `
  --build-info .\build-onnx-asio-mingw\evaluation-build-Release.json `
  --backend all --mode full --temporal --polyphonic-decoder competing `
  --corpus-version 3 --warmups 1 --repetitions 3 `
  --output-json .\build-onnx-asio-mingw\evaluation\poly-after-v3.json `
  --output-markdown .\build-onnx-asio-mingw\evaluation\poly-after-v3.md
```

For real data replace `--corpus-version 3` with:

```powershell
--corpus guitarset --dataset-split test --limit 12 --seed 2026 `
--audio-zip "$env:TEMP\guitarset-audio-mono-mic-v1.1.0.zip" `
--annotations-zip "$env:TEMP\guitarset-annotation-v1.1.0.zip"
```

Use output names `poly-after-guitarset.json`/`.md`. Omit
`--polyphonic-decoder competing` for the legacy spectral/stitching control.
The original before-change executable was also measured separately:
`poly-before-v3.json`/`.md` and `poly-before-guitarset.json`/`.md`. Its copied
build metadata records the copied runner path; binary hashes identify the
actual baseline. Report `evaluator` hashes describe the evaluation scripts
at run time, not a claim that the preserved executable was rebuilt from the
then-current dirty source. The earlier `poly-before-v2.json` captures its
pre-edit source context.

`--compare` still rejects different decoder configurations: these are
before/after experiments, not a reason to relabel incompatible baseline
metadata. No new quality or timing threshold is inferred from these results.
Reusing the current legacy mode reproduces the baseline's quality, not
necessarily its timing. Explicit provider selection remains `--provider cpu`
(default), `cuda`, `directml` or `rocm`, with a matching enabled runtime.

The product CLI also accepts:

```powershell
.\build-onnx-asio-mingw\transcriber.exe input.wav --polyphonic `
  --polyphonic-decoder competing --instrument guitar
.\build-onnx-asio-mingw\transcriber.exe input.wav --polyphonic `
  --model .\python\models\basic_pitch.onnx --polyphonic-decoder competing
```

## Validation and remaining limits

- The default model-free and ONNX/ASIO Release configurations are tested with
  CTest, including the new one-repetition `synthetic_polyphonic_suite`.
  This is a correctness smoke, not a new accuracy/performance gate.
  The optional unlabeled external-audio smoke is skipped when its input is
  absent.
- Final model-free CTest: 143 passed, one skipped (144 registered).
  Final ONNX/ASIO CTest: 149 passed, one skipped (150 registered).
  Default legacy predictions and every accuracy metric were also compared
  exactly with the preserved executable on all 30 fixtures for YIN, spectral
  and Basic Pitch; all were unchanged without the opt-in decoder flag.
  The separate GuitarSet runs above did execute annotated real recordings;
  they are not the skipped unlabeled smoke test.
- WebView2/ONNX/ASIO GUI Release builds successfully. No interactive GUI,
  physical ASIO device, GPU inference or non-Windows execution is claimed.
- New Catch2 tests cover exact middle-frame pitch sets for dyads, dense chords,
  octaves, bass/melody, simultaneous semitones and harmonic suppression; event
  retention under synthetic distortion and release overlap; grid validation;
  multi-window model geometry and contour/onset retention. Python tests cover
  optional head shape/type errors and absent/incompatible objective declarations.
- No actual annotated distorted-guitar corpus was evaluated. The real data is
  guitar-only, small and potentially leaked. Further instrument/session-disjoint,
  licensed evaluation is required before promoting experimental decoding.
