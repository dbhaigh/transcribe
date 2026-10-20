# Optional Python model development

Python is optional and is intended for model experiments, data preparation,
evaluation, and ONNX export. The product and built-in YIN analysis are C++20.

## ONNX contract, version 1

The C++ runner expects:

- Input: one mono `float32` waveform, shape `[1, samples]`, resampled by the
  runner to 16 kHz.
- Output: pitch activations at 100 frames per second, shape
  `[1, frames, 69]` or `[1, frames, 70]`.
- Pitch indices `0..68` map to MIDI `28..96`, inclusive. If present, output
  class `69` is silence and is used by the monophonic path.
- Output tensors must be `float32`; a second output head may be supplied for
  polyphonic pitch activations. In polyphonic mode, the runner selects that
  second head when its shape meets the pitch-output contract.

For monophonic inference, output values are treated as logits by default and
normalized with softmax across pitches and the optional silence class. The
built-in CLI uses `--model` to select ONNX; omitting it selects the independent
YIN tracker.

For polyphonic inference, enable `--polyphonic`. A single pitch head or a
second pitch head should represent per-pitch probabilities, ideally calibrated
to `[0,1]`; the runner applies greedy local-peak selection at a configurable
threshold and allows overlapping notes. It does not use a silence class to
decide which notes coexist. The current command-line and GUI options configure
the model outputs as independent probabilities. Training labels should
therefore be multi-hot, not a mutually exclusive softmax target. The pitch
count should be 69; a 70th value is reserved for mono silence and ignored in
polyphonic peak picking.

The implementation is intentionally strict about tensor rank, batch size,
pitch dimension, and floating-point type. Frame count is read from the output
shape; output timing is assumed to begin at zero with 10 ms spacing. Models
with different rates, pitch ranges, or input layouts need a versioned adapter
before they can be used.

## Suggested development workflow

1. Prepare aligned audio and note-event labels, with licenses and provenance
   documented.
2. Prototype/evaluate on held-out recordings and report both frame metrics
   (precision/recall) and note-event metrics (onset/pitch tolerance).
3. Export a model conforming to version 1 above. Verify output dimensions,
   sample rate, activation semantics, and timing on a known synthetic signal.
4. Run the model with ONNX Runtime enabled and compare against the built-in
   monophonic baseline.

## Bundled Spotify Basic Pitch model

`models/basic_pitch.onnx` is the ONNX export of Spotify's Basic Pitch model
from commit
[`fa5997af0a8210982619003269994a1be25eddf3`](https://github.com/spotify/basic-pitch/commit/fa5997af0a8210982619003269994a1be25eddf3).
Basic Pitch is Apache-2.0 licensed; see
[`models/LICENSE-basic-pitch.txt`](models/LICENSE-basic-pitch.txt) for the
license. The model is included with attribution for out-of-the-box polyphonic
inference.

The C++ adapter recognizes the model by its `[batch, 43844, 1]` waveform input.
It resamples mono audio to 22,050 Hz, processes two-second windows with the
official 30-frame overlap and edge trimming, and stitches note activations.
The note head is `StatefulPartitionedCall:1`, shaped `[1, 172, 88]`, with
independent probabilities for MIDI 21–108 at a 256-sample hop (~86.13 frames
per second). Transcribe crops to MIDI 28–96; the model's onset and contour
heads are not currently consumed. Greedy peak picking is applied when
`--polyphonic` is enabled.

Example from the repository root:

```powershell
.\build-onnx\transcribe.exe input.wav --model python\models\basic_pitch.onnx --polyphonic
```

This adapter is specific to the pinned Basic Pitch ONNX export and is separate
from the generic version 1 contract above. Results depend on instrument,
register, tuning, and recording quality; the bundled model is not a
commercial-grade general-purpose audio-to-score system. Do not train on
unlicensed audio or redistribute datasets without permission.

## Real-recording evaluation: GuitarSet

The reproducible evaluation uses GuitarSet v1.1.0's 360 mono-microphone guitar
recordings and aligned `note_midi` annotations. GuitarSet is distributed under
CC BY 4.0; the full dataset is not included here. The Zenodo record documents
the source, attribution, license, and known annotation issues:
[10.5281/zenodo.3371780](https://doi.org/10.5281/zenodo.3371780).

Download the two required archives to a data location outside the repository:

```powershell
$audio = "$env:TEMP\guitarset-audio-mono-mic-v1.1.0.zip"
$annotations = "$env:TEMP\guitarset-annotation-v1.1.0.zip"
curl.exe --location --fail --output $audio `
  "https://zenodo.org/records/3371780/files/audio_mono-mic.zip?download=1"
curl.exe --location --fail --output $annotations `
  "https://zenodo.org/records/3371780/files/annotation.zip?download=1"
```

With an ONNX-enabled `transcribe.exe` and Python 3.10 or newer, run from the
repository root:

```powershell
python .\python\evaluate_guitarset.py `
  --transcribe .\build-onnx-asio-mingw\transcribe.exe `
  --audio-zip $audio `
  --annotations-zip $annotations `
  --output-json .\python\evaluation\guitarset-v1.1.0-results.json
```

By default, this evaluates all 360 recordings for both the bundled Basic Pitch
model and the built-in YIN baseline. Zenodo MD5 checksums are verified before
processing. The per-recording results are written only to the requested JSON
file; no dataset audio or annotations are copied into the repository. Use
`--limit 24 --seed 2026` for a repeatable smoke subset. Do not tune thresholds
against this test set.

Reported scores are micro-averaged across the evaluated recordings:

- `onset_pitch`: one-to-one exact-MIDI event matches within 50 ms onset error.
- `note_event`: the same onset condition plus an offset tolerance of
  `max(50 ms, 20% of reference duration)`.
- `frame_10ms`: exact-MIDI active-pitch precision/recall/F1 on a 10 ms grid.

The run uses the shipped defaults (`minimum_confidence=0.25`,
`polyphonic_threshold=0.4`, at most six pitches per frame); no thresholds are
selected using GuitarSet labels.

Pitch values in GuitarSet's `note_midi` annotations can be fractional due to
intonation; they are rounded to the nearest MIDI semitone for comparison.
These scores characterize this dataset and the pinned, untuned defaults; they
are not a universal accuracy guarantee. GuitarSet has published annotation
issues, and its acoustic guitar recordings do not represent every instrument
or recording condition.

### Measured result (2026-10-19)

The full v1.1.0 mono-microphone set (360 recordings: 180 comping, 180 solo)
was evaluated with the model's default thresholds and compared to YIN on the
same recordings. Metrics are micro-averaged; the detailed per-recording counts
are in [guitarset-v1.1.0-results.json](evaluation/guitarset-v1.1.0-results.json).

| System | Onset + pitch F1 (P/R) | Note-event F1 (P/R) | 10 ms frame F1 (P/R) |
|---|---:|---:|---:|
| Basic Pitch | 33.14% (25.89/46.01) | 14.58% (11.39/20.24) | 72.97% (78.67/68.03) |
| YIN monophonic baseline | 22.31% (35.82/16.20) | 4.26% (6.83/3.09) | 22.67% (60.79/13.94) |

Basic Pitch predicted 111,040 note events against 62,476 reference events.
Its better frame score but low event precision/strict note score indicates
substantial false-positive and note-boundary/segmentation errors; do not treat
its overlapping-note output as engraving-ready without review. Comp/solo
note-event F1 was 14.00%/15.93% for Basic Pitch and 0.95%/9.13% for YIN.

This is a diagnostic benchmark, not a clean held-out generalization result:
the pinned Basic Pitch
[source includes a GuitarSet training-data loader](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/data/datasets/guitarset.py),
while the exact training records for the distributed weights are not
documented. Possible training-set overlap cannot therefore be ruled out. An
independent dataset or newly recorded, manually verified set is needed for an
unbiased estimate on unseen performances. GuitarSet itself also lists known
annotation timing/duplication issues.
