# Transcribe

Transcribe is a C++20 CLI and native Windows GUI for turning audio into a
first-pass musical score and standard-tuning guitar tablature. Version 0.1.1
provides a dependency-light WAV/YIN default, optional ONNX inference, optional
FFmpeg decoding, and optional Windows ASIO capture.

This release is an early transcription aid, not a commercial-grade
polyphonic automatic music transcription (AMT) system. The built-in tracker is
monophonic and works best on a single clear guitar or vocal line.

## Architecture

```text
WAV / optional FFmpeg       Windows ASIO
          \                   /
           AudioBuffer / ring buffer
                    |
        high-pass + adaptive noise gate
                    |
     multiresolution YIN + onset detection
              or ONNX model
                    |
 confidence-filtered Notes / pitch overlaps
          /            |             \
     MusicXML       guitar tab       MIDI
          \            |             /
                  CLI / Win32 GUI
```

The default executable and core library use only the C++ standard library.
Optional integrations are configured explicitly and are not needed for WAV
transcription.

## Build

Requires CMake 3.20+ and a C++20 compiler.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

To build and run the test suite (Catch2 is fetched by CMake):

```powershell
cmake -S . -B build-tests -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

## CLI

```text
transcribe <input> [options]
transcribe --asio <seconds> [options]
transcribe --batch <directory> [options]
```

Examples:

```powershell
.\build\Release\transcribe.exe guitar.wav --musicxml score.musicxml --tab guitar.txt
.\build\Release\transcribe.exe take.wav --midi take.mid --json take.json --benchmark
.\build\Release\transcribe.exe --batch .\recordings --output-dir .\scores --quiet
.\build\Release\transcribe.exe --asio 8 --musicxml live.musicxml --tab live.txt
```

Run `transcribe --help` for all options. Batch mode writes outputs into the
selected output directory (or a `transcribe-output` subdirectory) and supports
WAV by default; its input filter expands when FFmpeg is enabled. The simple
`key=value` config file uses the same long-option names without `--`, such as
`minimum-confidence=0.3`.

The WAV reader supports PCM 8/16/24/32-bit and IEEE float 32-bit RIFF/WAVE,
downmixes channels to mono, and decodes audio data in bounded blocks. For the
built-in YIN path, CLI and GUI analyze WAV files in overlapping 15-second
chunks, keeping decoded audio memory bounded independently of file duration.
The existing `AudioBuffer` API remains available. ONNX and compressed FFmpeg
inputs still use full-buffer inference/decoding.

## Windows GUI

On Windows, the build also creates `transcribe-gui`. It uses native Win32
controls and provides file pickers, drag-and-drop, an activity/progress
indicator, a rolling note preview during ASIO capture, a final note preview,
remembered paths, and an open-output-folder action. The ONNX/polyphonic
controls are only enabled in builds with ONNX Runtime.

```powershell
.\build\Release\transcribe-gui.exe
```

## Optional integrations

### ONNX Runtime

Install ONNX Runtime and set `ONNXRUNTIME_ROOT` to its installation directory,
then configure with `-DTRANSCRIBE_ENABLE_ONNX=ON`. Pass `--model model.onnx` to
use the model. `--polyphonic` requests independent per-pitch probabilities and
greedy multi-pitch peak picking. The built-in monophonic YIN path remains
selectable by omitting `--model`. The model input/output contract is
versioned in [python/README.md](python/README.md).

### FFmpeg

The zero-dependency default reads WAV only. To enable MP3, FLAC, OGG, M4A, and
other FFmpeg-supported formats, install FFmpeg development libraries exposing
`libavformat`, `libavcodec`, `libavutil`, and `libswresample` through pkg-config,
then configure with:

```powershell
cmake -S . -B build-ffmpeg -DTRANSCRIBE_ENABLE_FFMPEG=ON
```

The FFmpeg-enabled target is optional and has its own CI build. Use a
development package that matches the compiler/toolchain; merely installing the
FFmpeg command-line executable is not sufficient.

### ASIO capture

ASIO is Windows-only and disabled by default. Install/build RtAudio with its
ASIO backend and expose its CMake package, then configure:

```powershell
cmake -S . -B build-asio -DTRANSCRIBE_ENABLE_ASIO=ON
```

Capture uses a fixed-size single-producer/single-consumer ring. The audio
callback only copies samples; partial preview analysis and final transcription
run outside the callback. Real-time support depends on the selected device's
driver, sample-rate support, and buffer configuration.

## Accuracy and known limitations

- The built-in tracker is monophonic; chords require a trained ONNX model.
- YIN confidence, adaptive gating, and spectral-flux/energy onsets are
  heuristics. Distortion, vibrato, reverberation, noisy rooms, and overlapping
  instruments can still create missed or spurious notes.
- The ONNX path consumes one full audio tensor per request; it is not a
  persistent streaming neural inference engine. ONNX and FFmpeg decoding
  currently retain the full decoded input.
- Tempo, meter, and key are approximate estimates from note events. They are
  not guaranteed musical analysis and should be reviewed before engraving.
  The CLI, GUI summary, and JSON output expose inference flags and heuristic
  confidence scores; these scores are not calibrated probabilities. A default
  120 BPM, 4/4, or C major value is not reported as inferred when evidence is
  insufficient.
- MusicXML rhythmic quantization, ties, rests, and voice separation are
  intentionally basic; unusual tuplets and complex polyphony are not modeled.
- Guitar fingering is cost-based and uses standard tuning; legato markings are
  inferred from pitch transitions and are not performance annotations.
- ASIO, ONNX Runtime, and FFmpeg paths require the optional dependencies and
  hardware/runtime combinations above.
- Performance varies with compiler and input; `--benchmark` reports real-time
  factor for the measured run. No fixed throughput guarantee is made.

## Samples

The `samples/` directory contains short generated tone/phrase WAV files and
their expected pitch-event lists for repeatable smoke tests. They are
synthetic signals, not representative recordings; use them to check build and
pipeline behavior, not to estimate real-world accuracy. See the sample README
for provenance and license notes.

## Contributing

Keep the default WAV build dependency-light and preserve existing public
overloads when extending the API. Add focused Catch2 tests for changes to audio
decoding, pitch/onset analysis, score export, or tablature. Before submitting:

```powershell
cmake -S . -B build-tests -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

Do not commit model weights, unlicensed recordings, or generated build output.
Model contributors should document provenance, license, sample rate, labels,
and evaluation data alongside exported ONNX files.
