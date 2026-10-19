# Transcribe

Transcribe is a C++20 desktop/CLI foundation for turning audio into a first-pass
musical score and guitar tablature. The current built-in pitch tracker is a
monophonic YIN-style detector intended as a usable baseline, not a finished
polyphonic transcription model. It does not yet infer chord voicings, key,
meter, or expressive/performed rhythm.

## Build

Requires CMake 3.20 or newer and a C++20 compiler.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Transcribe a WAV file

```powershell
.\build\Release\transcribe.exe input.wav --musicxml score.musicxml --tab guitar.txt
```

The WAV reader accepts PCM 8/16/24/32-bit and IEEE float 32-bit RIFF/WAVE
files. Multi-channel audio is downmixed to mono. Output is MusicXML plus a
plain-text tablature view in standard guitar tuning.

## Windows GUI

The Windows build also produces a native `transcribe-gui` application. It
provides file pickers for audio and output paths, optional ONNX model selection,
and an ASIO capture mode when ASIO support is enabled. Transcription runs on a
worker thread so the window remains responsive; closing during capture requests
that capture stop.

```powershell
.\build\transcribe-gui.exe
```

With a multi-configuration generator such as Visual Studio, run
`.\build\Release\transcribe-gui.exe` instead.

## Optional real-time ASIO capture (Windows)

Install/build RtAudio with its ASIO backend and make its CMake package
discoverable, then configure:

```powershell
cmake -S . -B build -DTRANSCRIBE_ENABLE_ASIO=ON
cmake --build build --config Release
.\build\Release\transcribe.exe --asio 10 --musicxml live.musicxml --tab live.txt
```

The capture implementation records up to five minutes from the default ASIO
input. The audio callback only copies into preallocated memory; pitch analysis
and file output happen after capture has stopped.

## Optional ONNX Runtime inference

The built-in YIN-style detector is the default. To use a trained model, install
ONNX Runtime and point `ONNXRUNTIME_ROOT` at its installation directory, then
configure with `-DTRANSCRIBE_ENABLE_ONNX=ON`. Pass `--model model.onnx` to use
it for either file or ASIO input. The model contract is mono float audio at
16 kHz on input `[1, samples]`, with float logits `[1, frames, 70]` on output.
Output classes 0-68 represent MIDI pitches 28-96; class 69 means silence.
Frames are interpreted at 100 frames/second. See `python/README.md` for the
development workflow.

## Python model development

The optional `python/` directory is for research and model prototyping. The
shipping executable and audio pipeline remain C++. Export an inference model
to the ONNX contract above. See `python/README.md`.
