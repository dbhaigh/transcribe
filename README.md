# Transcriber

Transcriber 0.1.7 is a C++20 audio-to-score CLI with an optional Windows
WebView2 workstation as its sole graphical interface. It combines first-pass
transcription with editable notation/tab, canonical score projects, evidence
review, and reproducible build/package checks while retaining the
dependency-light WAV/YIN default and explicit optional ONNX, FFmpeg, ASIO,
WebView2, and Verovio integrations.

This release is an early transcription aid, not a commercial-grade
polyphonic automatic music transcription (AMT) system. The fast built-in YIN
tracker is monophonic; the optional spectral path can identify simple
polyphony, and both work best on clear recordings.

Version 0.1.6 adds the evidence-oriented release workflow: deterministic,
per-fixture synthetic regression gates; separated cold, warmup, and warm
performance measurements; host/provider metadata; and machine-readable plus
readable release reports. The reports keep synthetic regressions distinct
from licensed real-recording evidence, document missing qualification, and
include manual acceptance for the workstation's recording, notation, tab,
editing, playback, and export flows.

Version 0.1.7 completes the live ASIO-to-workstation path: bounded callback
transport, reconnect and gap reporting, a versioned/coalesced UI bridge,
incremental live notation/tab/timeline rendering, stage-specific health
metrics, and stop/cancel/refinement review semantics. Hardware-independent
replay verifies the callback transport through the bridge and browser UI.
Physical ASIO hardware acceptance remains outstanding; see the
[live manual acceptance checklist](docs/live-manual-acceptance.md).

## Architecture

```text
WAV / optional FFmpeg       Windows ASIO callback
          |                   /             \
 progressive file chunks   recording ring   analysis ring
          |                   |             |
          |              full AudioBuffer   realtime YIN worker
          |                   |             |
          +---- offline refinement     stable note-on/off events
                    |                    live Note list
                    +----------+-------------+
                               |
                 MusicXML / guitar tab / MIDI
                               |
                    CLI / optional WebView2 workstation
```

The default CLI and core library use only the C++ standard library. Optional
integrations are configured explicitly and are not needed for WAV
transcription. No native Win32 GUI is built; the WebView2 workstation is the
only graphical frontend and is enabled explicitly for Windows builds.

Long model-backed WAV files can opt into `--bounded-inference` with an explicit
`--backend`, or select **Bounded neural windows** in the workstation. This uses
overlapping offline windows, not stateful or full-buffer-equivalent neural
streaming. See [streaming contracts, memory limits, cancellation and measured
tradeoffs](docs/streaming.md) before enabling it.

## Build

Requires CMake 3.20+ and a C++20 compiler.

The default configure is dependency-light and does not build tests:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

For reproducible tested configurations, CMake presets are provided. For
example, on Windows:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --parallel
ctest --preset windows-release
```

On Linux, use `linux-gcc-release` or `linux-clang-release`. Optional presets
include `linux-ffmpeg-release` and `linux-sanitizer`. See
[the release checklist](docs/release-checklist.md) for dependencies and the
complete preset list.

See [Reliability and troubleshooting](docs/reliability.md) for strict CLI/config
validation, Unicode paths, safe per-file exports, sanitizer/static-analysis
commands, package checks and remaining release-qualification limits.

To build and run the test suite (Catch2 is fetched by CMake):

```powershell
cmake -S . -B build-tests -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

For deterministic synthetic quality/performance baselines, compiler-identified
JSON/text reports, event-versus-frame metrics, comparison gates, and the
licensed real-audio workflow, see [Evaluation](docs/evaluation.md).
For release regression thresholds, cold/warm measurement protocol,
provenance/leakage requirements, manual acceptance and JSON/Markdown release
reports, see [Release evidence](docs/release-evidence.md).
The [opt-in temporal decoder](docs/decoding.md) preserves raw evidence and adds
sample-referenced canonical events without changing existing CLI/GUI outputs.
The [versioned model interface](docs/model-inference.md) provides model
discovery, explicit contracts/providers, owned sessions and optional
`--backend <registry-id>` CLI selection.
The [neural event strategy](docs/neural-strategy.md) adds opt-in measured
Basic Pitch speed/quality modes, pinned model acquisition and validation-only
event confidence calibration while retaining the dependency-light default.

The [pluggable engine layer](docs/engines.md) composes those adapters with
capability-aware selection, a common lifecycle and provenance, and an optional
incremental interface. It distinguishes true live YIN from overlapping neural
windows and includes a compiled reference plugin.
The [polyphonic adapter evaluation](docs/polyphonic.md) documents frame-by-pitch
activations, opt-in aligned Basic Pitch onset/contour heads, and experimental
harmonic competition, including measured real-guitar regressions.
The synthetic evaluation needs no model or dataset download and is registered
with CTest when tests and Python are available.
Measured DSP optimizations and the cold/warm CPU/memory benchmark workflow are
documented in [Performance](docs/performance.md).
The optional [mixed-recording instrument-isolation workflow](docs/instrument-isolation.md)
accepts reviewed, sample-aligned guitar/bass/piano/voice stems without adding
dependencies to the default build. The WebView UI supports mixture/stem
audition before transcription; missing-source fallback is explicit. A separate
local-only waveform-ONNX worker and paired evaluation report both gains and
artifact-induced regressions. No separator weights are bundled or downloaded,
and no real-recording separation benefit is claimed from synthetic oracle tests.
The opt-in [incremental live pipeline](docs/live.md) (`--live-pipeline`) emits
provisional, stabilized, corrected, retracted and finalized note revisions with stable IDs,
bounded queues and separate latency metrics, plus a refinement proposal that
never overwrites manual edits. Sequenced sample-timestamped input is shared by
ASIO and bounded progressive WAV decoding. Only YIN is truly sample-incremental;
spectral/neural fallbacks explicitly rerun overlapping context. Configurable
latency targets report misses without silently changing pitch accuracy.
The separate [musical interpretation layer](docs/interpretation.md) preserves
measured event times while producing ranked tempo, meter, key, rhythm and
notation hypotheses for MusicXML and GUI score export.
The separate [guitar fingering solver](docs/guitar-fingering.md) ranks
playability-aware sequence assignments and preserves manual corrections
without changing sounding pitch.
It now checks held-note stretch and conservative finger/barre feasibility,
with configurable C++ search budgets and ergonomic costs. Suggestions expose
pruning and unresolved assignments rather than claiming the performed fingering.
See [Editing and playback](docs/editing-playback.md) for shared timeline mappings,
transactional editing/history, proposal protection and playback limitations.
The [canonical score model](docs/score-model.md) is the versioned source of
truth for accepted notation, tablature, MIDI exports, and transactional edits.
The [evidence-based review workflow](docs/review.md) preserves raw detector
evidence separately from calibrated confidence and provides ranked,
user-confirmed correction suggestions. The current workflow does not present
uncalibrated detector values as probabilities.
The local [project workflow](docs/projects.md) saves that accepted score,
analysis settings, audio references, export status, and correction history in a
versioned project file with autosave recovery. Version 2 adds integrity checks,
annotations, preserved fingering alternatives and rhythm settings, explicit
**Save with audio** portable-directory copies, and **Locate audio** relocation.
Undo stacks reset on reopening; accepted edits and revision audit records remain.
The optional [Verovio engraving view](docs/notation-rendering.md) renders that
accepted score as interactive, zoomable notation in the WebView2 workstation
while preserving the editable canvas.
The optional [WebView2 workstation](docs/workstation-ui.md) connects file and
ASIO capture, score editing, playback, interpretation settings, and explicit
score-proposal acceptance to the existing application backend.
Its [interaction guide](docs/workstation-ui.md#first-recording) covers guided
setup, contextual help, keyboard editing, shared timeline navigation and unsaved
project state. The [usability audit](docs/workstation-usability.md) records the
source findings, responsiveness checks and Windows validation scope.

## CLI

```text
transcriber <input> [options]
transcriber --asio <seconds> [options]
transcriber --batch <directory> [options]
transcriber --batch-manifest <csv> [options]
```

Examples:

```powershell
.\build\Release\transcriber.exe guitar.wav --musicxml score.musicxml --tab guitar.txt
.\build\Release\transcriber.exe guitar.wav --polyphonic --instrument guitar --json notes.json
.\build\Release\transcriber.exe take.wav --midi take.mid --json take.json --benchmark
.\build\Release\transcriber.exe take.wav --fast-preview --benchmark
.\build\Release\transcriber.exe take.wav --quality fast --tempo 96 --key-fifths 2 --mode minor --meter 6/8
.\build\Release\transcriber.exe --batch .\recordings --output-dir .\scores --quiet
.\build\Release\transcriber.exe --batch-manifest .\jobs.csv --output-dir .\scores --concurrency 2 --resume
.\build\Release\transcriber.exe --asio 8 --musicxml live.musicxml --tab live.txt
.\build\Release\transcriber.exe --live 8 --benchmark
```

See [batch processing and integrations](docs/batch-processing.md) for the
versioned CSV manifest, structured results, JSONL progress events, resumability,
memory-admission limits, exit codes, and an external-process example.

Run `transcriber --help` for all options. Batch mode writes outputs into the
selected output directory (or a `transcriber-output` subdirectory) and supports
WAV by default; its input filter expands when FFmpeg is enabled. The simple
`key=value` config file uses the same long-option names without `--`, such as
`minimum-confidence=0.3`, `latency-ms=35`, `quality=fast`, `tempo=96`,
`key-fifths=2`, `mode=minor`, `meter=6/8`, `batch=recordings`, `output-dir=scores`,
`asio=8`, `benchmark=true`, or `quiet=true`. Unknown config keys are reported
instead of silently ignored. `--live <seconds>` is an alias
for ASIO capture. The CLI can select the built-in analysis quality, model and
ONNX execution provider, instrument prior, capture latency, guitar tuning and
capo, score tempo/key/meter, and MusicXML, tablature, MIDI, and JSON outputs.
Tempo, key, and meter options override inference in the score and JSON metadata;
otherwise these values remain automatically estimated.

## MusicXML and guitar tablature

MusicXML export writes synchronized standard-notation and guitar-tablature
parts, with overlapping notes separated into voices. The interpretation layer
retains score hypotheses, distinguishes measured audio times from musical
positions, and writes pickup measures, rests, chord groupings, barline ties,
tempo changes and supported rhythmic spellings. Use `--tempo`, `--meter`,
`--key-fifths`, `--mode`, `--pickup`, and `--written-transposition` to constrain
interpretation; `--preserve-expressive-timing` annotates onset residuals.
Use `--quantization evidence` for confidence-gated passage rhythm inference,
or `--quantization expressive` to preserve measured timing. Optional
`--quantization-strength <0..1>` blends toward a grid; the default remains
legacy quantization. WebView advanced settings expose the same controls.
Uncertain key or meter signatures are not silently asserted in MusicXML.
See [Musical interpretation](docs/interpretation.md) for the current limits and
API.

Canonical MusicXML includes meter-aware beams, tuplet boundaries, double
accidentals and final barlines. Optional offline Verovio 6.3.0
(LGPL-3.0-or-later) supplies polished, paginated A4/Letter engraving; the custom
canvas remains a timing/editing sketch. It is not required for CLI export.
See [notation layout and independent-import tests](docs/notation-rendering.md)
for commands, licensing, supported conventions and explicit limitations.

The ASCII tablature and MusicXML fingering use the same sequence-level
playable-position solver. Select `--tuning standard|drop-d|dadgad|open-d|open-g`,
set `--capo 0..24`, and request up to five ranked sequence alternatives with
`--tab-alternatives 1..5`. For 1-12 independent strings, use
`--string-tuning 64,59,55,50,45,40,35` (string 1 first); reentrant and duplicate
pitches are supported. `--fret-count` sets the physical neck limit and
`--guitar-min-midi`/`--guitar-max-midi` constrain suggestions without discarding
detected pitches. The GUI shows per-note and whole-phrase alternatives and preserves
manual string/fret corrections across later analysis. Fingering edits preserve
the sounding pitch; changing pitch remains a separate explicit edit. These
positions are playability suggestions, not identification of the original
performance's exact string/fret choices. See [Guitar fingering](docs/guitar-fingering.md)
for cost definitions, ambiguity, unknown assignments, and correction matching.
Optional isolated-contour analysis distinguishes vibrato-like modulation and
continuous pitch rises/falls, with separate pitch, technique and fingering
uncertainty. It does not identify a bend versus slide, hammer-on, pull-off,
harmonic or original string from pitch alone. Real-recording technique/string
accuracy is not established by the deterministic fixtures.

## Performance and fast preview

The default YIN path already analyzes independent overlapping frames in parallel
for sufficiently long inputs. WAV transcription also reads and analyzes
overlapping 15-second chunks, so decoded memory stays bounded on long files.
The internal radix-2 FFT remains dependency-free; Hann windows and polyphonic
harmonic templates are reused across frames, and spectral magnitudes are
calculated once per frame. A one-entry, 64 MiB-bounded spectrogram cache also
reuses the most recently analyzed polyphonic input when its audio content,
sample rate, and FFT dimensions match, such as when changing pitch thresholds
or instrument priors.

Use `--fast-preview` for a rough built-in result: monophonic analysis uses an
8 kHz sample rate with a 1024-sample window and 512-sample hop instead of the
normal 11.025 kHz / 2048 / 256 configuration. Polyphonic preview uses a
2048-sample FFT and 512-sample hop instead of 4096 / 256. This trades temporal
and pitch detail for speed; run without the flag for the normal-quality pass.
`--benchmark` reports total analysis time, real-time factor, and built-in
preprocessing, onset/FFT, resampling, and pitch/FFT stage timings.
For `--asio`, it also reports the maximum event-detection latency in the
analysis stream's time domain. This measures the configured window and
stabilization delay, not device, operating-system, or end-to-end playback
latency.

On the bundled `samples/guitar_scale.wav`, a single run in the default
unoptimized Windows CMake build took about 0.50 s in normal mode and 0.21 s in
fast-preview mode (about 2.4x faster). The preview produced 8 events versus 6
in normal mode, illustrating the expected accuracy trade-off. This is one
machine/build/input measurement, not a throughput guarantee; use
`--benchmark` on target hardware for representative results.

The WAV reader supports PCM 8/16/24/32-bit and IEEE float 32-bit RIFF/WAVE,
downmixes channels to mono, and decodes audio data in bounded blocks. For the
built-in YIN path, the CLI uses overlapping 15-second chunks and the GUI uses
two-second chunks for progressive previews, keeping decoded audio memory
bounded independently of file duration. The existing `AudioBuffer` API remains
available. ONNX and compressed FFmpeg inputs still use full-buffer
inference/decoding.

## Windows workstation

The WebView2 workstation is the only graphical interface. It uses local
HTML/CSS/JavaScript and canvas score editing backed by the shared C++ engine;
there is no native Win32 GUI fallback. The workspace provides file and
supported ASIO capture workflows, waveform/spectrogram previews, synchronized
standard notation and guitar tablature, evidence review, project save/recovery,
transactional score editing, undo/redo, themes, keyboard shortcuts, playback,
and edited-score export. Optional Verovio adds offline MusicXML engraving.

Original-audio playback currently supports WAV files; synthesized note playback
uses Web Audio. Playback cursor alignment is approximate, and the custom
notation renderer is an interactive editing view rather than a full engraving
system. Pitch alternatives are adjacent-semitone suggestions with heuristic,
uncalibrated evidence, not additional model predictions. The WebView2 option
is off by default so the normal build stays dependency-light and CLI-only.

```powershell
.\build\Release\transcriber-gui.exe
```

ASIO capture analyzes a second, callback-fed ring buffer on a dedicated worker;
the audio callback itself only copies samples into the recording and analysis
rings. The worker emits stabilized note-on and note-off events and maintains a
queryable live note list. Capture still returns the complete recording for the
existing offline transcription/export pass, which refines the live preview.
Use `--latency-ms 20..50` on the CLI or the GUI's live-latency field to tune the
analysis window (default 40 ms); updates run at 100 Hz and are delivered to the
GUI as notes stabilize. Lower window settings trade low-pitch sensitivity for
faster response, and actual end-to-end latency depends on the audio device and
system load.

For WAV files, the workstation publishes an updated score preview as each two-second
chunk is analyzed. CLI streaming keeps its 15-second default chunks. This
progressive file mode and ASIO preview both preserve the final offline analysis
and export behavior.

## Optional integrations

### WebView2 workstation

The WebView2 workstation is the only graphical frontend and is built
explicitly on Windows. Extract the `Microsoft.Web.WebView2` NuGet package and
point CMake to the extracted package root:

```powershell
cmake -S . -B build-webview2 `
  -DTRANSCRIBE_ENABLE_WEBVIEW2=ON `
  -DTRANSCRIBE_WEBVIEW2_SDK_ROOT="C:\path\to\Microsoft.Web.WebView2"
cmake --build build-webview2 --config Release --target transcriber-gui
```

The build downloads Microsoft's Evergreen WebView2 bootstrapper and copies it,
`WebView2Loader.dll`, its package license/notice, and the local `ui/` assets
beside the executable. You can provide a pre-downloaded bootstrapper with
`-DTRANSCRIBE_WEBVIEW2_BOOTSTRAPPER_PATH="C:\path\to\MicrosoftEdgeWebView2Setup.exe"`
to build without network access during CMake configuration.

At startup, Transcriber checks for the Evergreen Runtime. If it is missing, the
app offers to launch the bundled bootstrapper. Runtime installation needs
internet access; close and restart Transcriber after installation. The runtime
updates independently of Transcriber. See Microsoft's
[WebView2 deployment documentation](https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution)
for deployment details. Leave `TRANSCRIBE_ENABLE_WEBVIEW2` off to build the
dependency-light CLI without a graphical application.

### Polyphonic analysis and instrument priors

The default built-in analysis remains multi-resolution YIN for fast
monophonic transcription. Add `--polyphonic` to select the built-in
STFT/harmonic-salience tracker without requiring ONNX Runtime. It uses greedy
per-frame pitch selection; `--instrument` accepts `guitar`, `piano`, or
`voice` to apply pitch-range and polyphony limits. This lightweight baseline
is less robust than a trained model for dense chords and overlapping sources.

### ONNX Runtime

Install ONNX Runtime and set `ONNXRUNTIME_ROOT` to its installation directory,
then configure with `-DTRANSCRIBE_ENABLE_ONNX=ON`. Pass `--model model.onnx` to
use the model. With ONNX, `--polyphonic` requests independent per-pitch
probabilities and greedy multi-pitch peak picking. Without `--model`, it
selects the built-in STFT polyphonic path; omitting both options keeps the
built-in monophonic YIN path. The model input/output contract is
versioned in [python/README.md](python/README.md). A compatible Spotify Basic
Pitch ONNX model is bundled at `python/models/basic_pitch.onnx`; with ONNX
enabled, use `--model python/models/basic_pitch.onnx --polyphonic` for
polyphonic inference.
Generic ONNX models additionally require an explicit
`--model-objective independent-multi-pitch` declaration; many pitch values do
not establish a multi-label training objective. The bundled Basic Pitch adapter
already declares its known objective. Add `--polyphonic-decoder competing`
to opt into aligned Basic Pitch heads or experimental model-free harmonic
competition; see the [quality tradeoffs](docs/polyphonic.md) before enabling it.

CUDA, DirectML, and ROCm provider selection can be enabled with the matching
ONNX Runtime GPU distribution. CUDA and ROCm provider option types come from
the ONNX Runtime headers. DirectML additionally needs
`dml_provider_factory.h` from a DirectML-enabled ONNX Runtime SDK:

```powershell
cmake -S . -B build-onnx-cuda `
  -DTRANSCRIBE_ENABLE_ONNX=ON -DTRANSCRIBE_ONNX_ENABLE_CUDA=ON
cmake --build build-onnx-cuda --config Release
.\build-onnx-cuda\Release\transcriber.exe guitar.wav --model model.onnx --provider cuda --device-id 0
```

For DirectML, configure with `-DTRANSCRIBE_ONNX_ENABLE_DIRECTML=ON`,
`-DONNXRUNTIME_DML_INCLUDE_DIR` pointing to the directory containing
`dml_provider_factory.h`, and `-DTRANSCRIBE_DIRECTML_SDK_ROOT` pointing to the
DirectML SDK containing `DirectML.h`. For ROCm use
`-DTRANSCRIBE_ONNX_ENABLE_ROCM=ON` with a ROCm-enabled ONNX Runtime build on
Linux. Provider selection is explicit: missing provider binaries, runtime
dependencies, or compatible hardware produce an error rather than silently
falling back to CPU.

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

ASIO is Windows-only and disabled by default. Build and install RtAudio with
its ASIO backend enabled, then point Transcriber at that installation:

```powershell
cmake -S ..\rtaudio -B rtaudio-build `
  -DRTAUDIO_API_ASIO=ON -DRTAUDIO_API_WASAPI=OFF
cmake --build rtaudio-build --config Release
cmake --install rtaudio-build --prefix "$PWD\rtaudio-install"
cmake -S . -B build-asio -DTRANSCRIBE_ENABLE_ASIO=ON `
  -DCMAKE_PREFIX_PATH="$PWD\rtaudio-install"
```

Capture uses a fixed-size single-producer/single-consumer ring. The audio
callback only copies samples; partial preview analysis and final transcription
run outside the callback. Real-time support depends on the selected device's
driver, sample-rate support, and buffer configuration.

## Accuracy and known limitations

- The default built-in YIN tracker is monophonic. The optional built-in STFT
  tracker can identify simple harmonic mixtures but is less robust than a
  trained ONNX model, especially for dense chords and overlapping instruments.
- On the GuitarSet v1.1.0 diagnostic evaluation, the bundled Basic Pitch
  model scored 14.58% strict note-event F1 and 72.97% 10 ms frame F1 at default
  thresholds. This shows useful pitch activity but weak note boundaries and
  substantial false positives; review exported notes. The evaluation and
  possible training-data overlap are documented in [python/README.md](python/README.md).
- YIN confidence, adaptive gating, and spectral-flux/high-frequency-content
  onsets are
  heuristics. Distortion, vibrato, reverberation, noisy rooms, and overlapping
  instruments can still create missed or spurious notes.
- The ONNX path consumes a full decoded input per request; it is not a
  persistent streaming neural inference engine. The bundled Basic Pitch model
  uses overlapping fixed windows and supports piano-range MIDI 28–96, while
  other ONNX models must follow the generic contract or have an adapter.
- GPU providers require a matching provider-enabled ONNX Runtime package and
  its runtime dependencies; they are not bundled with the default build.
- Tempo, meter, and key are approximate estimates from note events. They are
  not guaranteed musical analysis and should be reviewed before engraving.
  The CLI, GUI summary, and JSON output expose inference flags and heuristic
  confidence scores; these scores are not calibrated probabilities. A default
  120 BPM, 4/4, or C major value is not reported as inferred when evidence is
  insufficient.
- Musical interpretation and MusicXML spelling are heuristic. Tuplet ratios,
  swing, meter, tempo changes and local keys require review; the exporter does
  not yet serialize complete alternative hypotheses, full tuplet bracket
  grouping or engraving-quality multi-instrument notation.
- Guitar fingering uses a cost-based playable-position search. Alternative
  fingerings are suggestions, not verified performance fingerings; alternate
  tuning and capo support is limited to the listed tunings and a 24-fret range.
- ASIO, ONNX Runtime, and FFmpeg paths require the optional dependencies and
  hardware/runtime combinations above.
- Performance varies with compiler and input; `--benchmark` reports real-time
  factor for the measured run. No fixed throughput guarantee is made.

## Samples

The `samples/` directory contains short generated tone/phrase WAV files and
their expected pitch-event lists for repeatable smoke tests. They are
synthetic signals, not representative recordings; use them to check build and
pipeline behavior, not to estimate real-world accuracy. For a reproducible
real-recording evaluation against GuitarSet, see the procedure in
[python/README.md](python/README.md). See the sample README for fixture
provenance and license notes.

## Contributing

Keep the default WAV build dependency-light and preserve existing public
overloads when extending the API. Add focused Catch2 tests for changes to audio
decoding, pitch/onset analysis, score export, or tablature. Before submitting:

```powershell
cmake -S . -B build-tests -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

The suite includes synthetic monophonic/polyphonic analysis, realtime
input-time latency, generated MusicXML semantic parsing and multi-format CLI
exports, and UI asset/control-wiring checks. For an external, clean guitar or
vocal recording, set `TRANSCRIBER_REAL_AUDIO_CLIP` to a local audio path before
running CTest; that optional smoke test is skipped when no clip is supplied.
The reproducible GuitarSet guitar evaluation and its dataset/licensing details
are documented in [python/README.md](python/README.md).

## CI and release packages

The GitHub Actions workflow is configured for Windows Debug/Release and Linux
GCC/Clang Debug/Release, with separate optional FFmpeg, CPU ONNX Runtime, and
Windows WebView2/Verovio builds. Production targets use compiler warnings as
errors; separate Linux jobs are configured for
AddressSanitizer/UndefinedBehaviorSanitizer tests and `clang-tidy`. The workflow
configures CPack ZIP/TGZ creation, extracted-package CLI/help/WAV-export
checks, and feature-specific payload/notices audits. MSVC package rules stage
detected runtime DLLs. Linux CPU ONNX package rules stage its runtime library
and notices; Windows CPU packages stage `onnxruntime.dll` when it is present
in the supplied runtime distribution. WebView2 package checks cover its
loader, bootstrapper, local assets, and notices. The default Linux archive is
configured to run in a network-disabled Ubuntu runtime container without the
build toolchain.

The workflow uploads evaluation JSON/text reports for inspection when those
jobs run. Synthetic timing is descriptive and is not a noisy, per-build speed
gate. Maintained measured
baseline results and the comparison protocol are documented in
[Evaluation](docs/evaluation.md) and [Performance](docs/performance.md).
Workflow configuration is not evidence of a passing run; consult the exact
GitHub Actions run and artifact before making release claims.

These archives are not managed installers. The WebView2 Evergreen Runtime is
installed separately using the included bootstrapper. FFmpeg shared libraries
remain external runtime prerequisites for FFmpeg builds; GPU ONNX provider
dependencies are not bundled or hardware-qualified. ASIO builds require
developer-supplied RtAudio/ASIO dependencies and are not exercised on hosted
CI. See [the dependency inventory](docs/dependency-licenses.md) and
[release checklist](docs/release-checklist.md) before redistribution. The
repository is licensed under the MIT License; see [LICENSE](LICENSE). Optional
dependencies retain their own licenses and notices.

Do not commit model weights, unlicensed recordings, or generated build output.
Model contributors should document provenance, license, sample rate, labels,
and evaluation data alongside exported ONNX files.
