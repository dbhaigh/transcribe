# Opt-in note-event decoding

The new C++20 API in [decoding.hpp](../include/transcribe/decoding.hpp)
separates temporal interpretation from pitch estimation. It is **offline and
opt-in**: existing `Note`, realtime `NoteEvent` notifications, CLI output,
GUI behavior, exporters, and realtime stabilization are unchanged. No musical
grid is applied. No model or dataset download is needed for the built-in tests.
The default library still needs only the standard library.

```cpp
#include "transcribe/decoding.hpp"

auto audio = transcribe::read_wav("input.wav");
transcribe::AnalysisOptions analysis;
analysis.polyphonic = true; // false selects YIN
auto result = transcribe::transcribe_note_events(
    audio, analysis, {}, "caller-supplied-audio-content-identifier");
// result.events: accepted events; result.rejected_events: rejected candidates.
// result.raw_tracks: evidence before temporal interpretation.
```

`decode_note_events` accepts existing frame tracks without running inference
again. `transcribe_note_events_with_onnx` accepts the existing ONNX options,
a model path, and an explicit model identifier (prefer its SHA-256).
`transcribe_note_events_wav_streaming` accepts the existing streaming options.
The old notification type already uses the name `NoteEvent`, so the new
duration-bearing representation is named `CanonicalNoteEvent`.

## Event and evidence contract

- Events carry a string identity, MIDI pitch, onset/offset seconds, half-open
  source-sample coordinates `[onset_sample, offset_sample)`, original source
  sample rate, audio-source identifier, model identifier, method and evidence
  kind. Source sample zero is the beginning of decoded mono input, **not**
  analysis-resampler output, wall-clock capture time or a musical beat.
- Seconds are rounded to the nearest original source sample, clamped to the
  input, then derived back from those sample coordinates. Frame timestamps
  refer to analysis-window starts; the decoder uses window centers and one hop
  of support. Pointwise model activations have zero window size. The legacy
  onset detector's 1024-sample window-start timestamps are translated to window
  centers before they constrain the new decoder.
- IDs encode length-prefixed source/model/method identifiers, pitch and the
  decoded onset sample. Identical source identifiers, input, configuration and
  engine version produce identical IDs, including when evidence is requested.
  A consumer editing an event should retain its ID and change its lifecycle.
  **Re-decoding with different boundaries/options is not an identity-tracking
  service:** changed onset estimates can produce different IDs. Full-buffer
  and differently phased chunks need not produce identical IDs or samples.
- MIDI pitch is stored as a double to accommodate future fractional estimates;
  the present adapters emit integer MIDI pitches. Fractional evidence is absent,
  not invented from an integer estimate.
- `intensity_rms` is measured from the event's source audio. It is not MIDI
  velocity or calibrated loudness. A merged streaming event's RMS is absent:
  reporting one partial chunk's RMS as the whole event would be misleading.
  Velocity, calibrated correctness confidence, instrument, voice and
  articulation remain absent when unsupported.
- Lifecycle values support provisional, stabilized, refined, manually corrected
  and rejected states. Offline accepted events are stabilized or audio-refined;
  neither label means a musician has verified them. Rejected candidates and
  original tracks are retained. Streaming rejected candidates are chunk-local
  decisions, not a claim that no accepted event exists in another chunk.
- Per-event evidence retains original frame times, pitches, selected strength,
  voicing heuristic, optional raw model/salience value and its pitch. Original
  octave evidence is retained even when optional correction changes the event.
  Evidence is ordered chronologically after overlap reconciliation.

### What "raw" means here

`FrameEvidenceTrack.frames` preserves the existing adapter's **selected**
activity, before temporal decoding. Its `raw_values`, when available, additionally
preserve values before selection/normalization; neither is a correctness
probability. Rejected and unselected evidence remains in `result.raw_tracks`.

| Adapter | Selected frame strength | Additional retained values |
|---|---|---|
| YIN | `1 - CMND`, plus the existing voicing heuristic | Original integer frame pitch, including octave excursions |
| Spectral | Salience divided by strongest pitch salience, after candidate selection | Preselection salience per pitch and frame peak/summed-spectrum ratio |
| Basic Pitch | Selected/clamped note-head activation | Unclamped note-head activation for every frame in supported MIDI range 28-96 |
| Generic ONNX mono | Selected normalized activation | Selected raw tensor activation; not the complete class distribution |
| Generic ONNX poly | Selected normalized activation | Raw tensor activation for each adapted pitch track |

Basic Pitch's onset and contour heads are still **not consumed or retained**
by this adapter. This change does not claim onset-head/contour decoding, expand
its pitch range, expose fractional pitch, calibrate activations, or correct
generic ONNX model alignment. Existing model frame steps, trimming and
overlap handling remain the adapter's time reference. The generic-model
adapters compile in the optional build but were not accuracy-tested with a
separate generic ONNX model here.

The synchronous `AnalysisOptions`/`OnnxOptions.evidence_callback` is additive:
evidence buffers/copies are populated only when requested. Borrowed tracks must
be copied if retained. Full-buffer callbacks use input-relative seconds;
canonical WAV streaming callbacks and retained tracks use whole-file seconds.
Existing source callers need no changes, but rebuild C++ consumers: appending
callbacks changes aggregate layout and is not a binary-ABI guarantee.

## Decoder and parameters

1. Validate finite audio, source rate (at least 2000 Hz), nonempty provenance,
   track geometry, ordered nonnegative frame times, MIDI range and raw shapes.
   Invalid input throws; it is not returned as a successful empty transcription.
2. Form pitch-connected regions using activation thresholds and optional gap
   closing. Pitch tracks remain independent; different pitches may overlap.
   Below-off frames do not extend regions, but their evidence remains available.
3. Optionally correct one-frame YIN octave excursions only between matching,
   strong neighbors, with a gap allowance of at least one hop and no observed
   attack between the neighbors. Sustained octave changes are not corrected.
4. Build approximately 5 ms source-audio RMS regions. Start at the larger of
   the absolute floor and the relative peak threshold; sustain down to the
   absolute floor so a decaying note is not cut at its initial attack threshold.
   Actual zero samples remain silent even when the floor is zero.
5. Refine against intersecting audio regions and split at real energy gaps.
   Snap edges only within half an analysis window plus one hop. Do not use
   another sustained pitch's earlier/later activity to stretch an overlapping
   event to the shared audio envelope: cross-pitch bounds use a two-hop guard
   and at least minimum-duration evidence. Nearby onset peaks can adjust a
   start by at most one hop. A region starting at sample zero with RMS below
   half the clip peak is treated as a weak initial region for onset fallback.
6. Quantize only to source samples; reject below-duration/energy candidates,
   retain their evidence, and coalesce overlapping same-pitch/same-method
   duplicates. No tempo, score rhythm or musical-grid quantization is involved.

All numeric parameters must be finite and nonnegative, with these additional
constraints:

| Option | Default | Meaning / constraints |
|---|---:|---|
| `activation_on` | 0.4 | Selected normalized strength required to start; at most 1 |
| `activation_off` | 0.4 | Strength to sustain; at most `activation_on`; lower it to enable hysteresis |
| `maximum_gap_seconds` | 0 | Maximum inactive time bridged between frames or RMS regions; zero disables gap closing |
| `minimum_duration_seconds` | 0.055 | Minimum accepted sample-rounded duration; strictly positive, configurable for shorter notes |
| `energy_floor_rms` | 0.0015 | Absolute source RMS sustain/rejection floor, in floating audio sample units |
| `energy_relative_threshold` | 0.08 | Fraction of clip/chunk peak RMS required to start an energy region; at most 1 |
| `spectral_peak_fraction` | 0.02 | Minimum frame peak/summed-spectrum ratio when supplied; at most 1; rejects diffuse spectra, not a probability |
| `refine_with_audio` | true | Enable audio boundaries, energy-gap splitting and nearby onset constraints; false still enforces duration/RMS |
| `correct_isolated_octaves` | false | Enable the constrained, YIN-only correction above; requires adequate gap allowance |

### Streaming

WAV ingestion retains one configured chunk plus the decoder's 4096-sample I/O
block; preprocessing/resampling and FFT workspaces add per-chunk memory.
Existing chunk limits (1-300 seconds, overlap at least
0.25 seconds and less than the chunk) are checked. Each chunk uses its original
source rate; events and evidence are translated using the integer chunk origin.
A continuation chunk's left edge is **not** an observed onset. Same-pitch events
with positive temporal overlap are reconciled, preserving the first accepted ID,
extending the end, retaining both chunks' evidence and ordering it by source
time. Touching but nonoverlapping events are not blindly merged.

The result retains all raw tracks and events, so **total result memory grows
with file duration**. This is bounded audio buffering, not bounded total memory
or online canonical-event delivery. The legacy `notes_callback` is explicitly
rejected by the canonical streaming API; it has a different event type.
Audio/evidence callbacks are synchronous and overall progress is monotonic.
Realtime capture, its callback, locks and stabilization are not changed.

## Measurements and selection

Same-mode full-buffer Release comparisons used corpus v1: 16 clips, 24 reference
notes, seed 2026, PCM16 mono 22050 Hz, corpus SHA-256
`183c7f68643d6ac54a1b15a26af195e16590f115c5194f25c139366844f6fca9`.
Exact MIDI, onset tolerance 50 ms, offset tolerance
`max(50 ms, 20% of reference duration)`; frame occupancy is measured separately
at 10 ms. Windows/LLVM-MinGW Clang 23.1.1, C++20 `-O3 -DNDEBUG`, CPU inference.
Each final configuration ran sequentially with one discarded warmup and three
fresh-process repetitions; canonical events/evidence were deterministic.

| Backend | Decoder | Event TP/FP/FN | Precision | Recall | Strict event F1 | Frame F1 |
|---|---|---|---|---|---|---|
| YIN | Legacy | 13/17/11 | 43.33% | 54.17% | 48.15% | 78.43% |
| YIN | Opt-in | 15/7/9 | 68.18% | 62.50% | 65.22% | 80.95% |
| Spectral | Legacy | 16/80/8 | 16.67% | 66.67% | 26.67% | 81.01% |
| Spectral | Opt-in | 20/3/4 | 86.96% | 83.33% | 85.11% | 97.74% |
| Basic Pitch | Legacy | 17/23/7 | 42.50% | 70.83% | 53.13% | 95.57% |
| Basic Pitch | Opt-in | 20/3/4 | 86.96% | 83.33% | 85.11% | 98.26% |

| Backend | Onset error mean / median, legacy -> opt-in (ms) | Offset error mean / median, legacy -> opt-in (ms) |
|---|---|---|
| YIN | 27.69 / 26.53 -> 4.20 / 0.57 | 170.56 / 40.54 -> 61.69 / 2.72 |
| Spectral | 26.14 / 24.60 -> 2.80 / 0.34 | 163.61 / 53.24 -> 16.43 / 1.81 |
| Basic Pitch | 26.55 / 23.45 -> 7.23 / 0.40 | 164.69 / 50.23 -> 26.45 / 2.04 |

Errors use onset+pitch matches, not all references. The matched populations
change: YIN 17->18, spectral 21->21, Basic Pitch 20->22. Read errors alongside
recall, not as a paired timing experiment. Offset p95 still reaches 596 ms for
YIN and 221 ms for Basic Pitch; small medians do not eliminate difficult tails.
Chord-note recall is 20% for YIN and 100% for the other two on these five chord
reference pitches; YIN remains a monophonic tracker.

The selected decoder adds work and retained evidence; it is **not a speed
optimization**. Final median analysis times across unequal-length clips were
18.23 ms (YIN), 15.33 ms (spectral), 147.40 ms (Basic Pitch); see the reports for
per-clip timings and build identity. Do not interpret these pooled medians as
throughput for a fixed-length recording.

### Ablations

One measured invocation per clip was used for method selection, not timing
claims. The final combined configuration was then repeated as above.

| Variant | YIN event / frame F1 | Spectral event / frame F1 |
|---|---|---|
| Connected regions, no audio refinement | 0.00% / 76.18% | 44.44% / 89.31% |
| Audio refinement, on=0.4/off=0.3, gap=40 ms | 65.22% / 80.95% | 85.11% / 97.67% |
| Previous variant plus isolated-octave correction | 65.22% / 80.95% | 85.11% / 97.67% |
| Selected: audio refinement, on=off=0.4, gap=0 | 65.22% / 80.95% | 85.11% / 97.74% |

Audio refinement matters here; hysteresis/gap closing/octave correction do not
improve strict event F1 and are disabled by default. An earlier 0.5/0.5
activation trial gave Basic Pitch F1 80.85% and regressed long-boundary recall
from 2/3 to 1/3. The measured 0.4/0.4 setting restores 2/3, yields F1 85.11%
and preserves built-in event F1. Spectral frame F1 at 0.5 was slightly higher
(97.85%); this is a disclosed 0.11 percentage-point tradeoff **between new
candidates**, not a regression from the legacy engine.

These are deliberately simple connected-region/threshold alternatives, not a
trained sequence model. No HMM/neural sequence decoder is selected without
training data and independent evaluation. Thresholds were selected on this
small development corpus, not held-out real recordings.

## Reproduce, compare and validate

Use the [evaluation build instructions](evaluation.md). A normal CTest run
executes the canonical unit tests and the Python quality/provenance contract
inside `evaluation_framework`; the existing CI CTest invocation includes them.
The contract compares measured TP/FP floors with the stored snapshots, requires
better aggregate precision/recall/F1 and median boundary errors than same-build
legacy output, and checks repeated/silent/very-short/long-boundary cases.
ONNX quality checks run only in an ONNX-enabled build with the bundled model.

```powershell
python .\python\evaluate.py `
  --runner .\build-release-mingw\transcriber-evaluate.exe `
  --build-info .\build-release-mingw\evaluation-build-Release.json `
  --backend spectral --mode full --temporal `
  --compare .\python\evaluation\synthetic-v1-windows-clang23-spectral-temporal-full.json `
  --max-event-f1-drop 0 `
  --output-json .\build-release-mingw\evaluation\candidate-temporal.json `
  --output-text .\build-release-mingw\evaluation\candidate-temporal.txt
```

Change backend/baseline together for YIN. For Basic Pitch use an ONNX-enabled
runner/build metadata and `--backend onnx --mode full --temporal --model
.\python\models\basic_pitch.onnx`. Model identity is its file SHA-256:
`2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`.

To reproduce ablations, omit `--compare` and add
`--no-audio-refinement`, or `--activation-off .3 --maximum-gap .04`,
optionally with `--correct-isolated-octaves`. To recheck the earlier threshold
trial use `--activation-on .5 --activation-off .5`.
Full and streaming are both available for built-ins; ONNX evaluation remains
full-buffer, and temporal realtime mode is explicitly rejected.

Reports retain accepted canonical events/evidence and use `null` for unavailable
confidence/velocity, including the metric-only legacy-shaped projection.
Full raw tracks/rejected candidates are available through C++, not serialized
in these benchmark snapshots. Normal `--compare` rejects changed decoder
configuration: legacy-versus-temporal and ablations are distinct experiments,
not falsely compatible regression comparisons.

Stored measured snapshots:
[YIN](../python/evaluation/synthetic-v1-windows-clang23-yin-temporal-full.json),
[spectral](../python/evaluation/synthetic-v1-windows-clang23-spectral-temporal-full.json),
[Basic Pitch](../python/evaluation/synthetic-v1-windows-clang23-onnx-temporal-full.json).
They record actual dirty-checkout source digests, base commit, executable,
compiler, model, provider, configuration and repetition metadata; they do not
claim an uncommitted source tree was a clean release.
All three were independently rerun against their stored snapshots with a
zero-drop gate: event and frame F1 deltas were exactly zero, with matching
build/environment identity. Process timing varied by about 6%, as expected;
this is not a claimed decoder speed improvement.

Validation performed locally:

- Final default Release: **54/54 CTests passed**; optional ONNX/ASIO Release:
  **57/57 passed**, both with the supplied external recording enabled. Earlier
  runs without that environment variable explicitly skipped the recording test.
- Held/repeated/overlapping short-chunk tests sweep five onsets around seams
  at 11025 and 48000 Hz, assert all event onsets within 50 ms and offsets within
  100 ms, unique accepted IDs and monotonic progress. Full/default-streaming
  strict counts agree across the 16 fixtures.
- Empty/silent input, configurable short durations, raw shapes/finite values,
  rate conversions (8000-48000 Hz), all decoder parameters, observed-onset
  octave protection and unchanged legacy evidence-callback outputs are tested.
- The full WebView2/ONNX/ASIO GUI Release builds successfully. No interactive
  GUI, ASIO hardware, GPU-provider or Linux execution is claimed.
- Legacy YIN and spectral note vectors, times and heuristic confidences on the
  supplied unlabeled recording exactly match the saved pre-decoder performance
  report. Private audio and its predictions remain local/ignored.

## Limits

The 10 ms very-short fixture is still missed by all compared paths; the engine
does not produce usable evidence for it. Lower minimum duration can retain
short supplied evidence but cannot invent missing pitch frames. Repeated notes
with no energy gap or discriminating model activity can still merge. Shared
audio RMS is not source separation, isolated octave correction is not general
octave disambiguation, and quiet overlapping voices remain difficult.

No annotated real-audio evaluation was rerun for this decoder. The supplied
recording validates legacy preservation, **not accuracy**. The historical
GuitarSet strict F1 14.58% / frame F1 72.97% remains unchanged, is not a baseline
for this decoder, and possible model training-data overlap is unresolved.
Use the [licensed workflow](evaluation.md#licensed-real-audio-workflow) for
independent real-data work before enabling this decoder in production UI,
realtime capture, or score/export workflows.
