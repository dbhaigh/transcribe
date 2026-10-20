# Model catalog

`basic_pitch.onnx` is Spotify Basic Pitch's Apache-2.0 model, pinned to
commit `fa5997af0a8210982619003269994a1be25eddf3`. Its license and inference
contract are documented in [../README.md](../README.md). The C++ adapter
supports this model's fixed-window input and note-output head.

New models must include their source and dataset licenses, supported
instrument/use case, evaluation methodology and results, and export/runtime
requirements. Generic models must conform to the versioned interface in
[../README.md](../README.md), or have a documented adapter.
