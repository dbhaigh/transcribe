# Model catalog

`basic_pitch.onnx` is Spotify Basic Pitch's Apache-2.0 model, pinned to
commit `fa5997af0a8210982619003269994a1be25eddf3`. Its license and inference
contract are documented in [../README.md](../README.md). The C++ adapter
supports this model's fixed-window input and note-output head.
The opt-in [neural event strategy](../../docs/neural-strategy.md) uses compatible
onset/note-state/contour heads and documents measured speed/quality modes.
Verify the bundled model with `python .\python\models\acquire.py`; explicit
`--download --output <path>` acquires the pinned artifact when missing, with
size/checksum verification and no overwrite of mismatched files. Acquisition
never runs automatically at application startup.
The [machine-readable provenance](basic_pitch.provenance.json) records the
reviewed SHA-256, upstream commit, adapter/objective, license and evaluation
leakage warning. Source and package smoke tests verify the model bytes.
Its actual tensor declarations, supported versus unused heads, window
stitching and explicit `BasicPitchV1` adapter are described in
[Model inference](../../docs/model-inference.md).

New models must include their source and dataset licenses, supported
instrument/use case, evaluation methodology and results, and export/runtime
requirements. Generic models must conform to the versioned interface in
[../README.md](../README.md), or have a documented adapter.
