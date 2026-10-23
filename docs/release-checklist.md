# Release checklist

Use this checklist for each tagged release. Record links to the exact commit,
CI run, build artifacts, benchmark reports, and any approved exceptions in the
release notes. A local compile is not evidence that an artifact was tested on a
clean machine.

The latest [local reliability matrix](reliability.md#executed-local-release-readiness-matrix)
records executed builds/tests, explicit skips, the FFmpeg dependency failure
and remaining platform/hardware sign-off blockers. It does not substitute for
CI or clean-machine qualification.
The automated [release evidence report](release-evidence.md) combines CTest,
per-fixture quality gates, quality/performance reports, provenance, and manual
acceptance. A CI-generated `pending` report is not a release sign-off.

## Current repository facts

- Project version is 0.1.7; the CLI release banner is generated from the CMake
  project version.
- The default C++20 build is dependency-light and reads WAV audio. FFmpeg,
  ONNX Runtime, Windows ASIO capture, WebView2, and Verovio are optional.
- CPack produces ZIP/TGZ archives, not a managed installer. Uninstall means
  closing the application and removing its extracted directory. Updating is a
  manual replacement; back up project files first. The application does not
  register an updater or uninstaller.
- CI is configured to extract artifacts outside the build tree, check required
  payload files, and run `--help` plus WAV transcription/export. The default
  Linux archive is configured to run in a network-disabled Ubuntu runtime
  container without compiler tools. MSVC packages are configured for compiler
  runtime DLL checks; WebView2/Verovio packages are checked for their
  executable, assets, loader, bootstrapper, and notices. The GUI is not
  launched by that smoke test.
- The root [`LICENSE`](../LICENSE) applies the MIT License to Transcriber
  source and is installed into CPack packages. Third-party components retain
  their separate licenses and notices.
- The CPU ONNX install rules stage the runtime library and its notices when
  present in the supplied runtime distribution. GPU provider dependencies are not
  bundled or hardware-tested. FFmpeg shared libraries are not bundled; an
  FFmpeg package needs a compatible runtime installation on its target system.
- CI does not build ASIO because its SDK and hardware/runtime requirements are
  not supplied on hosted runners. A successful default build is not an ASIO
  qualification.
- Current benchmark and feature limits are described in
  [Evaluation](evaluation.md), [Performance](performance.md), and
  [README accuracy notes](../README.md#accuracy-and-known-limitations).

## Per-release sign-off

### Source, model, data, and licensing

- [ ] Confirm the release commit and source tree; record any dirty-tree or
  generated-file state.
- [ ] Verify the root MIT license is included in source and package artifacts.
  Confirm any newly contributed code is authorized for distribution under
  that license; do not apply the project license to bundled third-party work.
- [ ] Verify the bundled Basic Pitch source commit, model file, Apache-2.0
  license, and model/runtime contract. Record a model SHA-256 in the release
  record.
- [ ] Confirm external evaluation dataset provenance, license, attribution,
  annotation limitations, and archive checksums. Do not redistribute the
  dataset unless separately authorized.
- [ ] Review possible training-data overlap between Basic Pitch and evaluation
  recordings. State what is known and unknown; do not call a potentially
  overlapping set an independent holdout.
- [ ] Audit the actual optional dependency versions and notices in every
  artifact. In particular, inspect ONNX Runtime notices, WebView2 notices,
  Verovio LGPL materials, MSVC redistributable terms, and the exact FFmpeg
  build/license configuration.
- [ ] Verify that model weights, evaluation audio, credentials, and local
  project files are absent from release archives.

### Build, tests, and package qualification

- [ ] Confirm CI passed on Windows Debug and Release and Linux GCC/Clang
  Debug and Release for the dependency-light default.
- [ ] Confirm the sanitizer and static-analysis jobs passed; review and
  disposition any suppression or warning.
- [ ] Confirm the optional FFmpeg, CPU ONNX Runtime, and WebView2/Verovio jobs
  passed for the relevant artifacts. Mark ASIO, GPU, and other untested
  configurations as unqualified.
- [ ] Review Catch2, CLI/export, UI asset, project serialization, and synthetic
  evaluation CTest results and all three v1 per-fixture regression gates.
  Record skipped tests with the reason; an absent real recording or audio
  device is not a pass.
- [ ] Extract each release archive into a clean directory/environment and run
  its package smoke test. Verify executables, sample fixtures, model files,
  runtime DLL/shared libraries, UI assets, and third-party notices.
- [ ] Launch the GUI on a clean supported Windows installation. Verify the
  WebView2 Runtime setup path, missing-runtime message, file transcription,
  editing, save/reopen, and exports. The automated payload smoke is not a
  substitute for this manual GUI test.
- [ ] Record manual acceptance for live input/recording, real-time notation,
  tablature synchronization, editing and undo/redo, playback-cursor
  synchronization, and export round trips. Exercise the UI while audio is
  arriving, then pause/stop/retranscribe; confirm canceled/stale jobs do not
  overwrite edits. Save the signed result using the schema in
  [Release evidence](release-evidence.md).
- [ ] Test optional-feature runtime dependencies on a machine without
  development tools or developer `PATH` entries. Record any packages users
  must install separately and verify failures are actionable.
- [ ] Verify upgrade/replacement and manual removal instructions. Preserve
  user project/audio files; do not overwrite them as part of an application
  update.

### Product claims and documentation

- [ ] List supported inputs accurately: WAV in the default build; additional
  formats only in a build with compatible FFmpeg libraries.
- [ ] State device support accurately: ASIO capture is optional Windows-only
  and requires a matching RtAudio/ASIO SDK and supported device. Do not infer
  hardware support from compilation.
- [ ] State acceleration requirements accurately. CPU ONNX is the qualified
  neural path only when the matching runtime is installed/staged; CUDA,
  DirectML, and ROCm require provider-specific binaries, drivers, and
  compatible hardware.
- [ ] Include known event-level transcription, timing, polyphony, rhythmic
  interpretation, fingering, and engraving limitations. Do not present
  heuristic evidence as calibrated probability.
- [ ] Run the documented synthetic and licensed real-audio evaluation on the
  release binary. Preserve JSON/text reports with compiler, build type,
  model/provider, sample rate, and settings. Report frame and note-event
  metrics separately.
- [ ] Run cold and warm Release benchmarks using the documented protocol.
  Compare against a maintained baseline only when hardware, build
  configuration, provider, model, input, and measurement protocol match.
  Keep shared-runner timings descriptive; do not gate releases on one noisy
  timing sample or claim unmeasured speedups. Review p50/p95/p99 sample
  counts and memory limitations.
- [ ] Verify project schema/version and migration behavior, and include a
  backup warning for manual application replacement. Document unsupported
  future project formats and lossy export conversions.
- [ ] Review installation and troubleshooting instructions against the exact
  package. Include the WebView2 Runtime and optional shared-library
  prerequisites.
- [ ] Review README and release notes for claims supported by executed tests,
  and identify configurations that were only inspected or compiled.

## Reproducible CI commands

The presets below use CMake 3.20+ and configure tests with strict warnings on
production targets. Python 3.10+ is required for integration/evaluation tests;
Node.js 22 is used by UI checks when available.

```text
Windows: cmake --preset windows-release
         cmake --build --preset windows-release --parallel
         ctest --preset windows-release

Linux GCC: cmake --preset linux-gcc-release
           cmake --build --preset linux-gcc-release --parallel
           ctest --preset linux-gcc-release

Linux Clang: cmake --preset linux-clang-release
             cmake --build --preset linux-clang-release --parallel
             ctest --preset linux-clang-release

Sanitizers: cmake --preset linux-sanitizer
            cmake --build --preset linux-sanitizer --parallel
            ctest --preset linux-sanitizer
```

For direct CPack archive verification, run
`python tests/package_smoke.py <archive> --isolated-runtime`; add
`--expect-ffmpeg`, `--expect-model`, `--expect-webview2`, or
`--expect-verovio` only for matching builds. FFmpeg's system runtime is still
an external prerequisite even when the FFmpeg package smoke passes.
