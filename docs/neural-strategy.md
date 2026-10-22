# Evaluation-driven neural note events

## Decision and scope

Keep the pinned Apache-2.0 Basic Pitch weights. No new weights were trained,
converted or redistributed. The default build still has no neural dependencies.
The default decoder remains compatible with previous CLI results.

The opt-in `--neural-decoder quality` (`events`) mode exposes the existing
aligned multi-head evidence and canonical event decoder as one usable neural
strategy. This is an integration improvement over the default legacy note
path, **not a new neural network or an improvement over the already available
`--temporal --polyphonic-decoder competing` evaluation path**. Tests compare
the new output directly with canonical decoding of the same evidence.

`--neural-decoder speed` (`legacy`) retains the previous note conversion.
These names describe the measured tradeoff for the pinned model on the CPU
configuration below, not universal guarantees. They do not select GPU, change
sample rate, download models or use the built-in `--quality high|fast` setting.
Other weights/devices/input lengths need fresh measurements.

```powershell
.\build-onnx\transcriber.exe recording.wav `
  --model .\python\models\basic_pitch.onnx --backend onnx:basic-pitch-v1 `
  --polyphonic --neural-decoder quality --provider cpu
```

Config files accept `neural-decoder=quality` or `speed` (and the neutral
`events`/`legacy` names). Quality mode requires polyphonic Basic Pitch;
unsupported contracts and missing onset heads fail explicitly. Bounded WAV
windows are supported through the existing `--bounded-inference` path; they
remain an approximation to full-buffer inference, not stateful model streaming.
The persistent `--live-pipeline` now also supports this strategy over bounded
overlapping windows, including ASIO capture; see [latency budgets](live.md).
It does not repeatedly initialize the model or run inference in the callback.
The accuracy/timing results below describe the stated offline protocol, not a
claim that windowed live inference has identical quality or latency.

## Verified model contract

The inspected [upstream release source](https://github.com/spotify/basic-pitch/tree/fa5997af0a8210982619003269994a1be25eddf3)
ships the 230,444-byte ONNX artifact under its Apache-2.0 license.
The [provenance manifest](../python/models/basic_pitch.provenance.json)
pins the commit, HTTPS URL, byte count, SHA-256 and retained license.

| Head | Tensor | Float32 shape | Temporal resolution | Use |
|---|---|---|---|---|
| Note state | `StatefulPartitionedCall:1` | `[1,172,88]` | 256 samples at 22050 Hz | Independent MIDI 21-108 activity; adapter retains MIDI 28-96 |
| Onset | `StatefulPartitionedCall:2` | `[1,172,88]` | Same | Separate attack peaks and repeated-note evidence |
| Contour | `StatefulPartitionedCall:0` | `[1,172,264]` | Same | Three bins/semitone; local fractional-MIDI evidence |

`ModelMetadata::heads` now represents role, tensor identity, sample rate,
frame step, origin, pitch/bin geometry, required status and actual availability
for each head independently. It does not assume future adapters have the same
temporal resolution. These head origins describe the trimmed input-relative
clock; the existing model metadata describes the raw window/edge transformation.
Loading validates actual tensors, not just this descriptive metadata.
There is no separate learned offset head; offsets use note activity and audio
refinement. Contours are retained as evidence, not converted into invented
instrument labels, calibrated probabilities or additional semitone notes.

Quality mode collects heads even without an application evidence callback,
regrids all heads using actual window positions and invokes canonical
segmentation once. It skips the redundant legacy acoustic/event decoding.
The 43844-sample input, 3840-sample left pad, 36164-sample window advance,
15-frame edge trim and 142 retained frames are unchanged. The advance is not
`142 * 256`: concatenation would drift. The aligned grid starts at recording
zero and ends before the source duration. Source-sample projections and
fractional contour evidence use that same clock. See [polyphonic alignment](polyphonic.md).

The strategy uses the existing temporal defaults (0.4 activity on/off,
55 ms minimum duration, zero gap bridging and model attack/state-drop guards).
It does not raise thresholds to suppress quiet false positives. The existing
evaluation `--temporal` controls remain available for explicit parameter
experiments; do not combine that second decoder with neural event mode.
`minimum-confidence` remains the legacy note filter; event mode uses the
canonical activity/onset rules instead. Event `Note::confidence` is the mean
selected event strength, explicitly heuristic, not a calibrated probability.

## Sessions, providers and limits

Use `ModelBackend::load` once, optional `warm_up`, repeated `infer`, then
`release`, or the engine lifecycle. Warm-up is explicit, not a hidden download
or initialization cost. Evaluation supports `--model-interface
--session-warmups 1 --session-repetitions 3`; each clip's process reports load,
warm-up and separate within-session inference timings. Parent `--warmups` are
still process-cold runs, not persistent-session warm-ups. Outer total/RTF
includes all requested inner repetitions; do not call it single-pass latency.

CPU is the default. CUDA, DirectML and ROCm require compatible build flags,
ONNX Runtime distributions and devices. Explicit accelerator requests retain
the existing no-CPU-provider-fallback contract. Only CPU was measured here;
no GPU speedup is asserted.

Cancellation uses the existing stop token and ONNX Run termination, with checks
before and after canonical decoding. Audio refinement/segmentation is not
interruptible at every operation. Sessions survive cancellation for subsequent
requests. Single-owner/non-reentrant ownership is unchanged.
`ModelLoadOptions::maximum_input_bytes` optionally rejects oversized decoded
input before inference allocations; zero preserves the existing unrestricted
offline contract. It is **not a whole-process memory ceiling**. Full-buffer
mode retains audio/heads/event evidence and needs more memory than legacy mode.
Use the existing bounded-window buffer/note limits for long files; result-cache
limits and lifetimes remain explicit. See [streaming limitations](streaming.md).

## Explicit acquisition and rights

```powershell
# Verify an existing clean-checkout model; no network access.
python .\python\models\acquire.py
# Explicitly acquire a missing artifact, if needed; never runs at startup.
python .\python\models\acquire.py --download --output .\models\basic_pitch.onnx
```

Downloads are size-bounded, checked against the pinned SHA-256, then published
exclusively. Existing mismatched files are not overwritten; failures remove
only the temporary download. Source/package checks validate provenance and
model size/hash. Existing packaging retains the model license and manifest.
No training-data rights for new training or new weight distribution are assumed.

Additional candidates were screened, not installed:

- [MT3](https://github.com/magenta/mt3) uses T5X and autoregressive event-token
  decoding, not this fixed-window ONNX multi-head contract. Its README says it
  is not an officially supported Google product. No reviewed small ONNX artifact,
  conversion equivalence or independent local benchmark was established.
- [ByteDance piano transcription](https://github.com/ByteDance/piano_transcription)
  documents a PyTorch piano-specific onset/offset/frame model, developed with
  Python 3.7/PyTorch 1.4 and trained on MAESTRO. That is not evidence of a
  maintained, rights-reviewed ONNX guitar/general-instrument replacement.
  The separate checkpoint license, footprint and conversion were not verified.

Neither candidate met all runtime, maintenance, weight-rights and output-contract
gates. No speculative adapter, heavyweight dependency or replacement model is
added, and no additional-model accuracy claim is made.

## Calibration and reproducible evaluation

Reuse [evaluate.py](../python/evaluate.py) and
[evaluation_metrics.py](../python/evaluation_metrics.py), rather than a separate
scoring framework. Follow [build/data instructions](evaluation.md) to build
`transcriber-evaluate` with ONNX and obtain the checked GuitarSet archives.
Replace `build-onnx` below with the configured build directory.

```powershell
$runner = ".\build-onnx\transcriber-evaluate.exe"
$info = ".\build-onnx\evaluation-build-Release.json"
$audio = "$env:TEMP\guitarset-audio-mono-mic-v1.1.0.zip"
$labels = "$env:TEMP\guitarset-annotation-v1.1.0.zip"
$out = ".\build-onnx\evaluation"

# Same 30 synthetic fixtures for YIN, spectral and the compatible ONNX baseline.
python .\python\evaluate.py --runner $runner --build-info $info `
  --backend all --mode full --model-interface --corpus-version 3 `
  --repetitions 2 --warmups 0 --output-json "$out\synthetic-baselines.json"

# Freeze event strategy on performer-04 validation data, before test inspection.
python .\python\evaluate.py --runner $runner --build-info $info `
  --backend onnx --model .\python\models\basic_pitch.onnx --model-interface `
  --neural-decoder quality --corpus guitarset --dataset-split validation `
  --audio-zip $audio --annotations-zip $labels --limit 4 `
  --repetitions 1 --warmups 0 --session-warmups 1 --session-repetitions 3 `
  --output-json "$out\validation-events.json"
python .\python\evaluation_metrics.py --fit-report "$out\validation-events.json" `
  --output-json "$out\confidence.json"

# Identical performer-05 inputs across all baselines.
python .\python\evaluate.py --runner $runner --build-info $info `
  --backend all --mode full --model-interface --corpus guitarset --dataset-split test `
  --audio-zip $audio --annotations-zip $labels --limit 4 --repetitions 1 --warmups 0 `
  --output-json "$out\test-baselines.json"

# Apply the frozen calibration; no test-time fitting or note suppression.
python .\python\evaluate.py --runner $runner --build-info $info `
  --backend onnx --model .\python\models\basic_pitch.onnx --model-interface `
  --neural-decoder quality --corpus guitarset --dataset-split test `
  --audio-zip $audio --annotations-zip $labels --limit 4 `
  --repetitions 1 --warmups 0 --session-warmups 1 --session-repetitions 3 `
  --confidence-calibration "$out\confidence.json" `
  --output-json "$out\test-events.json" --output-markdown "$out\test-events.md"
```

For synthetic event comparison, repeat the synthetic command with `--backend
onnx --model .\python\models\basic_pitch.onnx --neural-decoder quality`.
For the within-session validation speed comparison, repeat the validation
command with `--neural-decoder speed`, without calibration. Default seed 2026
selects the recorded subsets deterministically.

Calibration uses isotonic regression of heuristic event strength against
one-to-one **pitch + onset + offset** matches, including duplicate predictions
as false positives. It requires both positive and negative predictions and
validation-only labels. Application checks inference configuration, model hash,
evaluator identity, scoring tolerances, dataset provenance, test-only labels,
and disjoint audio hashes/performers. It reports held-out Brier score,
reliability bins and ECE, retaining raw scores and every predicted note.
The calibration is evaluation-only; it is not silently installed as a universal
runtime probability model. It measures correctness conditional on predictions,
not missed-note probability. GuitarSet may overlap Basic Pitch training data,
so performer separation here is **not an independent model-training holdout**.

## Measured evidence

Windows LLVM-MinGW Clang 23.1.1 Release, ONNX Runtime 1.20.1 CPU:

| Corpus/system | Event precision | Event recall | Event F1 | Frame occupancy F1 |
|---|---:|---:|---:|---:|
| Synthetic v3, YIN | 0.3125 | 0.3922 | 0.3478 | 0.6006 |
| Synthetic v3, spectral | 0.1546 | 0.6275 | 0.2481 | 0.7859 |
| Synthetic v3, Basic Pitch legacy | 0.3301 | 0.6667 | 0.4416 | 0.8538 |
| Synthetic v3, Basic Pitch events | 0.7636 | 0.8235 | 0.7925 | 0.8909 |
| GuitarSet test subset, YIN | 0.0551 | 0.0647 | 0.0595 | 0.3479 |
| GuitarSet test subset, spectral | 0.0064 | 0.0421 | 0.0111 | 0.4286 |
| GuitarSet test subset, Basic Pitch legacy | 0.0473 | 0.1586 | 0.0729 | 0.7839 |
| GuitarSet test subset, Basic Pitch events | 0.4554 | 0.6440 | 0.5335 | 0.8397 |

Frame occupancy is sampled from predicted note events using the existing metric,
not a claim about raw neural tensor calibration. Reports separately retain
pitch, onset+pitch, offset-tolerant event scores, timing errors, fragments,
merges, false positives/minute, per-register and annotated scenario results.
No new aggregate regression threshold is introduced from these small subsets.

All four test clips improved event and frame F1, but results remain uneven:

| Test clip (performer 05) | Legacy event F1 | Events F1 |
|---|---:|---:|
| BN1-129-Eb solo | 0.1486 | 0.7755 |
| Jazz2-110-Bb solo | 0.1528 | 0.7297 |
| SS1-100-C# comp | 0.0057 | 0.2943 |
| SS1-100-C# solo | 0.1354 | 0.5669 |

On four validation clips, legacy -> events reduces fragmented references
226 -> 38 and false positives/minute 461.91 -> 121.23. Offset error median
improves 112.0 -> 28.0 ms, but onset error median **worsens 23.8 -> 30.8 ms**.
Merged predictions remain 16; their rate increases because fewer notes are
predicted. This is not a uniform accuracy improvement. Synthetic very-short,
pitch-bend, distorted-guitar and harmonics-only fixtures still have zero strict
event F1; reverb also remains problematic. No synthetic per-clip event or frame
F1 regressed against legacy, but synthetic success alone is insufficient.

Held-out event confidence assessment on the four test recordings gives Brier
0.1685 and ECE 0.0731 across 437 predictions, without filtering notes. This is
small, potentially training-overlapping evidence, not universal calibration.

Validation timing uses serial CPU runs, one persistent session per clip,
one explicit warm-up and three measured inferences (12 measurements per mode).

| Validation measurement | Speed / legacy | Quality / events |
|---|---:|---:|
| Within-session inference median | 0.8059 s | 0.8550 s |
| Within-session inference p95 | 1.0011 s | 1.0449 s |
| Audio-weighted single-pass inference RTF | 0.02814 | 0.02976 |
| Maximum observed process peak working set | 62.21 MiB | 81.18 MiB |

These are the same four validation recordings and unchanged weights. Quality
mode costs about 6% more median inference time on this subset. Initialization
was 58-61 ms and the two-second warm-up 83-87 ms, measured separately. Working
set includes the process/runtime/audio, not just model weights or live tensors.
P95 is the existing nearest-rank estimator over 12 observations, not a latency
guarantee. There is no GPU comparison.

Do not use concurrently executed synthetic suite timings to select providers.
See generated JSON for per-clip initialization, warm-up, inference and process
peak memory. No hardware ASIO, GPU, Linux or MSVC qualification is implied.

## Executed verification

- Dependency-light Release/warnings-as-errors: **186 passed, 2 skipped** out
  of 188 CTests.
- ONNX CPU + ASIO Release/warnings-as-errors: **194 passed, 1 skipped** out
  of 195 CTests. These include scoring/calibration/acquisition integration,
  model-head shape/missing-head tests, CLI/export and headless UI regressions.
- Focused ONNX inference/engine/polyphonic/decoding: **3,807 assertions in
  43 cases** passed.
- Debug ASan/UBSan model-free focused selection: **2,444 assertions passed**;
  39 cases passed and one ONNX-only case skipped. Leak detection was disabled.
- Strict Windows GUI + ONNX + ASIO build and production clang-tidy build passed.
- Explicit pinned HTTPS acquisition into a separate temporary destination,
  installed ONNX package/license/provenance smoke and actual CLI quality-mode
  MusicXML/ASCII-tab/JSON export smoke passed.

The model-free CTest skip is ONNX-only bounded inference. Both configurations
skip the optional standalone real-audio smoke fixture; that is separate from
the explicitly executed, archive-verified GuitarSet evaluations above.
IDE test discovery found no tests; Catch/CTest/Python runners were used.
No FFmpeg, GPU, hardware capture or new-platform validation is claimed.
