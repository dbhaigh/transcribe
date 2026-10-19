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

The `models/` folder is a placeholder for a future model catalog. Do not add
weights until their origin, license, evaluation results, supported use case,
and contract version are recorded. Do not train on unlicensed audio or
redistribute datasets without permission.
