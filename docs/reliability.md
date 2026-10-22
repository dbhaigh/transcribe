# Reliability, failure handling and release validation

## Reproducing the checks

The dependency-light default still needs only CMake 3.20+ and a C++20 compiler.
Tests additionally fetch Catch2 v3.7.1 and use Python 3.10+. Nothing downloads
models or datasets during the default evaluation. Follow the Windows/MSVC and
Linux GCC/Clang presets in [the release checklist](release-checklist.md).

For LLVM-MinGW on Windows with its compiler and `mingw32-make` on `PATH`:

```powershell
cmake -S . -B build-release -G "MinGW Makefiles" `
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release `
  -DTRANSCRIBE_BUILD_TESTS=ON -DTRANSCRIBE_WARNINGS_AS_ERRORS=ON
cmake --build build-release -j 4
ctest --test-dir build-release --output-on-failure
```

Use a new build directory when changing generators or compilers. Do not edit
source or rebuild a measured runner during evaluations: the evaluation scripts
deliberately fail if source/runner identity changes during measurements.

Smallest release-contract selection:

```powershell
ctest --test-dir build-release --output-on-failure `
  -R "cli_export_validation|atomic.exports|dependency_manifest_smoke|interpretation_semantics|fingering_export_semantics|ui_workflow_browser"
cpack --config build-release\CPackConfig.cmake -G ZIP -B build-release\packages
python tests\package_smoke.py build-release\packages\transcriber-0.1.6-win64.zip --isolated-runtime
```

`--expect-model`, `--expect-webview2`, `--expect-verovio`, `--expect-ffmpeg`
and `--expect-msvc-runtime` apply only to matching package configurations.
Package tests extract and execute the CLI and validate installed assets; they
do not launch the native GUI or establish audio-device compatibility.

For static analysis add `-DTRANSCRIBE_ENABLE_CLANG_TIDY=ON` in a separate build.
For ASan/UBSan use `linux-sanitizer`, or a compatible Clang/GCC toolchain with
`-DTRANSCRIBE_ENABLE_SANITIZERS=ON`. CMake now probes sanitizer linking and
reports a missing runtime at configuration time. Windows LLVM-MinGW was tested
with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`; this is **not** LeakSanitizer
coverage. Run `ctest --test-dir <sanitizer-build> -LE evaluation --output-on-failure`
for the non-benchmark suite. Release timing comparisons must not use sanitizer
builds.

## Inputs and configuration

- Windows arguments are read from the Unicode command line. Config files and
  serialized path strings use UTF-8; a first-line UTF-8 BOM is accepted.
  File APIs retain native filesystem paths. Console output is UTF-8; configure
  the terminal/consumer accordingly.
- CLI and config numeric values use the same full-token, finite parser.
  Trailing units/junk, integer fractions/overflow, unsigned negatives, NaN and
  infinity are rejected. Numbers use `.` as decimal separator without locale
  grouping or a leading `+`. Existing option-specific ranges still apply.
  Capo is 0-24 and requested fingering alternatives are 0-5.
- Unknown, duplicate, empty and malformed config entries fail explicitly;
  later config files may intentionally override earlier ones, followed by CLI
  overrides. `meter=auto` and `instrument=none` reset prior constraints.
  Existing simple `#`/`;` comment syntax remains; quoted/escaped config paths
  containing those characters are not supported.
- Exactly one input mode is allowed: file, batch directory, or ASIO capture.
- WAV validation checks RIFF/chunk extents, frame alignment and encoding before
  sample decoding, including zero-frame files. Supported encodings remain PCM
  8/16/24/32-bit and IEEE float 32-bit. RF64, extensible WAV and compressed WAV
  codecs are not supported by the built-in reader. No production detection
  threshold was changed.
- Model adapters continue to validate input/output dimensions, temporal
  contracts, pitch objectives and explicit providers. Accelerator requests
  never silently become CPU success. A model-free build does not gain neural
  support merely by receiving `--model`.

## Output safety

MusicXML, MIDI, ASCII tab, CLI notes JSON, score-project JSON and proposal JSON
now share the existing project writer's exclusive temporary-file and atomic
replacement mechanism. Complete serialization precedes disk replacement;
write, flush, close and replacement errors are surfaced. A failed replacement
cleans up its temporary file and preserves the previous destination. Windows
locked-destination tests exercise failure **after** writing the temporary.
Directories, symlinks and device destinations are rejected by the atomic writer.
Exports use memory proportional to the serialized score, not a bounded streaming
document writer; large scores can still fail allocation before replacement.

Before analysis the CLI rejects output/output collisions, paths aliasing its
audio/model/config inputs, missing output parents, and directory destinations.
Equivalent existing files (including hard links) are checked as well as
normalized paths; Windows comparisons are case-insensitive. Batch mode rejects
duplicate output stems instead of overwriting another input's results. Parent
directories are not created by individual exporters; batch mode creates its
requested output directory.

These guarantees are **per file**, not an atomic multi-format transaction.
If the third export fails, the first two successful exports remain and the
command exits nonzero without printing a success summary. Concurrent external
filesystem changes are not locked out by CLI preflight. Power-loss durability
on every filesystem is not promised. A killed process can leave an ignored
`.tmp-<pid>-<sequence>` sibling, never a partially replaced final export.
Remove confirmed abandoned temporary files only when no writer owns them.

The optional live JSON-lines log is intentionally incremental rather than an
atomic snapshot. Every write and the final close are checked; interrupted runs
may retain a partial log, which must not be treated as a finished recording.
Cooperative inference cancellation, dropped blocks and stale GUI-job protection
retain the contracts in [Streaming](streaming.md), [Live](live.md) and
[Editing/playback](editing-playback.md).

## Regression evidence and interpretation

The existing CLI/export smoke now covers matching invalid CLI/config values,
duplicate configuration, UTF-8/BOM and space-containing input/config/output
paths, all five export types, input/output collisions, missing parents,
empty/truncated/overflowing/malformed/unsupported WAV headers and preservation
of previous exports on decode failure. On POSIX it also checks `/dev/full`
failure for the incremental log. MIDI is decoded, not just checked for `MThd`:
track lengths, balanced note-on/off events, pitch sets, canonical event counts
and 480-tick-quarter timing are verified.

Existing tests also cover score/project serialization and reusable undo/redo,
semantic MusicXML voices/ties/rests/tuplets/tempo/meter/key, ASCII/XML tuning
and fret agreement, bounded chunk stitching, cancellation, model reuse,
memory budgets, long-WAV boundaries and capture-ring stress. The browser
workflow uses installed Edge/Chrome when available (CTest skip 77 otherwise).
It is not an automated native WebView2/device session.

MIDI currently emits all voices on one channel. Overlapping same-pitch events
are preserved as balanced events, but synthesizers can differ in their unison
voice/note-off behavior. MIDI also has finite tick/time resolution and does not
preserve all score-project notation/evidence. An XML parser accepting the file
or a MIDI header being present is not semantic correctness.

## Troubleshooting and unqualified configurations

| Symptom | Action |
|---|---|
| Unsupported audio format | Use supported PCM/float WAV, or build with FFmpeg. A `.wav` suffix does not make a compressed/corrupt WAV compatible. |
| WAV RIFF/chunk/encoding error | Re-export the source as standard PCM/float WAV; do not patch its length header to mask missing audio. |
| `Could NOT find PkgConfig` | Install `pkg-config` and FFmpeg development packages for the compiler/architecture, or keep `TRANSCRIBE_ENABLE_FFMPEG=OFF`. A standalone decoder executable is insufficient. |
| Model load/tensor-contract error | Select the matching registered adapter and review [model contracts](model-inference.md); arbitrary ONNX pitch tensors are not necessarily polyphonic models. |
| Requested provider unavailable | Install/build the matching provider/runtime and drivers, or explicitly request `--provider cpu`. No automatic provider fallback is promised. |
| Safe output/replacement error | Check the parent directory, free space, permissions and file locks. Use distinct normal file destinations, not input files, links or devices. |
| Sanitizer cannot link/run | Install matching runtime libraries or use the Linux preset. Windows runs here disabled leak detection; other sanitizer platforms still need qualification. |
| Missing GUI assets/runtime | Use the complete extracted package with its `ui` directory, loader and WebView2 Runtime. See the package checks and [release checklist](release-checklist.md). |
| Evaluation identity changed | Stop editing/building, then rerun the affected evaluation. Do not disable the identity check. |

The model SHA-256, upstream commit, adapter, objective, license and leakage
warning are in [the model provenance](../python/models/basic_pitch.provenance.json).
Source and extracted-package checks verify the actual model bytes. The digest
records this reviewed local artifact; it is not independent cryptographic
attestation of upstream authorship. Exact optional dependency notices and
redistribution terms still require [release review](dependency-licenses.md).

Linux, MSVC, hardware ASIO/device-loss, GPU providers, physical playback latency,
Verovio-enabled native packaging, clean-machine GUI startup, real power loss,
disk exhaustion on Windows and multi-hour end-to-end runs need separate release
qualification when not explicitly recorded in the measured matrix.

## Executed local release-readiness matrix

Environment: Windows, LLVM-MinGW Clang 23.1.1, C++20, Python 3.12.10,
ONNX Runtime 1.20.1 CPU. Release production targets used
`TRANSCRIBE_WARNINGS_AS_ERRORS=ON`. These are local results, not claims that the
remote CI jobs ran. Existing uncommitted work from the preceding improvements
was retained.

| Configuration/check | Executed outcome |
|---|---|
| Fresh default dependency-light Release, tests OFF (`build-release-readiness`) | Configure and build passed. |
| Dependency-light Release tests (`build-evaluation-quality`) | Full CTest: **176 passed, 2 skipped / 178 registered**, 123.70 s. |
| ONNX CPU + ASIO Release (`build-onnx-asio-mingw`) | Build and full CTest: **183 passed, 1 skipped / 184 registered**, 215.81 s. ASIO coverage is hardware-independent. |
| WebView2 + ONNX CPU + ASIO Release (`build-webview-onnx-asio-mingw`) | Strict build passed; extracted ZIP payload/model/CLI smoke passed. Native GUI startup and device interaction were not automated. |
| Dependency-light Debug ASan + UBSan (`build-release-sanitizer`) | Build and `ctest -LE evaluation`: **166 passed, 2 skipped / 168 selected**, 180.31 s. No final sanitizer finding; leak detection disabled. |
| Dependency-light clang-tidy (`build-release-analysis`) | Production static-analysis build passed, analyzer findings treated as errors. |
| Default Release ZIP | CPack and extracted CLI help/WAV/export/license smoke passed. |
| Optional GUI/ONNX/ASIO ZIP | CPack and extracted UI-file, model SHA-256, notice, CPU inference and export smoke passed. Verovio was OFF. |
| Headless installed Edge workflow | Passed in the full Release and sanitizer selections. UI asset smoke passed; optional Node.js syntax check was skipped because Node.js was absent. |
| FFmpeg configuration (`build-ffmpeg-long-validation`) | **Could not configure:** `PKG_CONFIG_EXECUTABLE` missing. No FFmpeg compile/runtime test claimed. |
| Linux GCC/Clang, MSVC, GPU providers, ASIO hardware, Verovio-enabled GUI | **Not run locally.** Existing CI covers Linux/MSVC defaults, Linux FFmpeg/CPU ONNX, Linux sanitizers/static analysis, and Windows WebView2/Verovio packaging. GUI CI now also selects the browser workflow, CLI release regressions and dependency checks. |

The two model-free/sanitizer skips were the ONNX-only bounded Basic Pitch
test and the opt-in `real_audio_smoke` fixture. ONNX/ASIO skipped only the
unconfigured real-audio fixture. The ten evaluation-labeled tests were
intentionally excluded from the sanitizer selection; the Release suites ran
their corresponding evaluation selections. No new multi-hour measurement or
annotated real-audio accuracy claim is made in this pass.

Failures found and resolved during validation:

1. Strict production builds exposed missing initializers for fields added by
   the earlier feature work. Explicit/default initialization restored strict
   builds without disabling warnings.
2. The new Unicode regression exposed a remaining native-path-to-narrow-string
   conversion. UTF-8 serialization/diagnostics now use a shared conversion;
   Unicode audio/config and all five exports pass.
3. ASan identified a **test-only** use-after-free in the recovery fixture:
   it inspected a fake backend after its owning replay session destroyed it.
   A live load-observer replaced the dangling read; the complete selected
   sanitizer suite subsequently passed.
4. One initial ONNX live evaluation rejected a source identity change while
   this pass was editing files. A complete rerun with stable source passed.
   This was an effective reproducibility guard, not a suppressed regression.

### Remaining release sign-off blockers

- Obtain passing supported Linux/MSVC CI and qualify optional artifacts on
  clean machines; local LLVM-MinGW results do not replace that matrix.
- Supply FFmpeg development/runtime packages to qualify that backend.
- Review exact dependency redistribution terms/notices, especially the local
  RtAudio/ASIO SDK and FFmpeg builds. Model-byte checks are not legal clearance.
- Manually qualify WebView2 Runtime setup/startup, physical playback, ASIO
  capture/device loss and any claimed GPU execution provider.
- Retain the documented early-product accuracy, expressive-notation, unison
  MIDI and inferred-fingering limitations. This pass did not change quality
  thresholds or establish a new independent real-audio holdout.
