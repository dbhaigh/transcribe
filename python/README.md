# Optional Python model development

Use Python for experimenting with pitch/onset models and preparing training
data; it is not required to build or run the C++ product. This folder currently
documents the intended workflow and intentionally does not prescribe a
particular neural-network architecture or dataset.

Recommended workflow:

1. Prepare aligned audio and note-event labels (MIDI or equivalent).
2. Prototype and evaluate a model in Python, including polyphonic and
   instrument-specific cases.
3. Export an ONNX model that accepts mono float audio at 16 kHz with input
   shape `[1, samples]`, and emits float logits at 100 frames/second with
   output shape `[1, frames, 70]`. Output classes 0-68 map to MIDI pitches
   28-96; class 69 is silence.
4. Run it in the C++ pipeline using ONNX Runtime. The built-in monophonic
   detector remains available when no model is specified.

Do not train on unlicensed audio or redistribute datasets without permission.
