# Release evidence framework

Transcriber separates deterministic quality regression checks from
hardware-sensitive performance measurements and manual product acceptance.
The report generator combines those records without turning missing evidence
into a pass. It emits versioned JSON and readable Markdown; CI uploads the
report beside the underlying JSON evaluation and gate records.

## Clean-checkout deterministic run

The framework uses Python's standard library and the repository's existing
C++ evaluation runners. It does not download models or recordings for the
dependency-free tests.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --parallel
New-Item -ItemType Directory -Force out\build\windows-release\evaluation | Out-Null
ctest --preset windows-release `
  --output-junit out\build\windows-release\evaluation\ctest.xml
```

CTest writes the synthetic quality reports and three independent v1
regression-gate reports to `out/build/windows-release/evaluation`. To assemble
them:

```powershell
python python\release_report.py `
  --quality out\build\windows-release\evaluation\synthetic-yin.json `
  --quality out\build\windows-release\evaluation\synthetic-spectral.json `
  --quality out\build\windows-release\evaluation\synthetic-realtime.json `
  --quality out\build\windows-release\evaluation\quality-suite.json `
  --quality out\build\windows-release\evaluation\polyphonic-suite.json `
  --quality-gate out\build\windows-release\evaluation\release-gates\yin.json `
  --quality-gate out\build\windows-release\evaluation\release-gates\spectral.json `
  --quality-gate out\build\windows-release\evaluation\release-gates\realtime.json `
  --ctest-junit out\build\windows-release\evaluation\ctest.xml `
  --output-json out\build\windows-release\evaluation\release-report.json `
  --output-markdown out\build\windows-release\evaluation\release-report.md
```

Use the corresponding preset and paths for Linux. The CI report is expected to
remain `pending` until licensed real-recording measurements, a controlled
Release performance run, and manual acceptance are supplied. A pending report
is useful evidence; it is not a release sign-off.

## Deterministic quality gates

The checked-in corpus-v1 YIN-streaming, spectral-full, and realtime snapshots
are schema-v1 historical measurements. The current evaluator emits schema v2.
`quality_release_gate.py` explicitly validates their common semantics instead
of silently comparing entire reports across incompatible schemas. It checks
corpus and per-file audio digests, engine/model, all baseline settings and
scoring tolerances, then compares each of the 16 fixtures independently.

Defaults permit at most a 0.01 absolute drop per fixture in frame, onset-plus-
pitch, and note-event precision/recall/F1, and at most a 10 ms increase in
matched mean onset or offset error. An unavailable error distribution must
remain unavailable in both reports. The gate fails on configuration drift or
missing/changed fixtures; changed evaluation settings require a reviewed new
baseline, not a relaxed threshold. The historical snapshots were measured
over repeated runs and the current YIN sentinel reproduced their event/frame
metrics. These gates are regression sentinels for simple deterministic
synthetic signals; they do not establish accuracy on real instruments.

The v2/v3 cohort suites remain un-gated until a stable, independently reviewed
baseline and thresholds exist. Per-clip and per-scenario results remain in
their reports so an aggregate improvement cannot hide an individual result.
Structural fields such as fragmentation/merging are reported where available;
the schema-v1 snapshots do not contain a compatible fragmentation baseline,
so the gate does not invent one.

## Performance protocol

The performance runner now records, in order:

1. Process setup (`process_wall_seconds`, measured by the Python parent).
2. Runner startup through file decode/model initialization (`startup_seconds`).
3. One explicit cold inference.
4. The requested number of warmup inferences, excluded from reported samples.
5. The requested number of warm inferences in `runs`.

For every cold and warm invocation it records wall/CPU time, real-time factor,
audio throughput, first-window latency, buffered-audio use, and process
high-water resident memory. ONNX model load time, model hash, runtime version,
provider, availability and session/resource diagnostics are recorded
separately. Reports include compiler/build/features, CPU identity, logical
cores, GPU inventory when an installed vendor utility can report it, and
Python/platform identity. Failure to inventory a GPU is explicitly described;
it is not treated as proof that the machine has no GPU.

`performance.py` computes linearly interpolated p50/p95/p99 for warm samples.
The reports mark tail percentiles from fewer than 100 samples as descriptive,
not statistically stable. Use repeated measurements on a quiet, controlled
machine; compare only matching hardware, build configuration, provider, model,
audio, and measurement protocol. Process RSS is a lifetime high-water mark,
not a per-call allocation count. A measured GPU path is not assumed faster.
Shared CI does not impose a wall-clock gate.
When a report includes a matched-hardware comparison and any input is slower,
the release report records each affected input and remains `pending` until the
regression is reviewed and dispositioned. Deltas from mismatched systems are
marked incomparable, not converted into speedup claims.

Example for a local Release build:

```powershell
python python\performance.py `
  --runner out\build\windows-release\Release\transcriber-performance.exe `
  --build-info out\build\windows-release\evaluation-build-Release.json `
  --repetitions 30 --warmups 3 `
  --output out\build\windows-release\evaluation\performance.json
```

For a build whose targets are not under a `Release` subdirectory, adjust only
the runner path. Optional ONNX CPU or accelerator runs require the matching
enabled build, runtime, model, and `--model-interface --onnx-model` options.
Do not use an incompatible older performance schema as a baseline: protocol
version 2 is required for comparisons.

## Real-recording provenance and leakage

Use the existing GuitarSet evaluation workflow and its fixed performer split.
Preserve the dataset version, CC BY 4.0 attribution, archive/checksum records,
annotation limitations, selected split, engine/model hash and provider in the
quality report. Do not redistribute dataset audio or annotations unless
separately authorized. Do not tune thresholds on a final test split.

Synthetic scores do not establish generalization. Potential Basic Pitch
training-data overlap with GuitarSet or any other public recording set remains
unresolved unless verified independently. The generated release report carries
this warning for neural-model evidence and does not label the data an
independent holdout.

## Manual acceptance record

The report accepts a schema-v1 JSON file with one status per critical GUI
workflow. Values are `pass`, `fail`, or `not_run`; evidence notes are
recommended. All checks must pass for manual acceptance to pass.

```json
{
  "schema_version": 1,
  "operator": "release tester",
  "date": "YYYY-MM-DD",
  "checks": {
    "live_input_and_recording": {"status": "not_run", "evidence": ""},
    "real_time_notation": {"status": "not_run", "evidence": ""},
    "tablature_synchronization": {"status": "not_run", "evidence": ""},
    "editing_and_undo_redo": {"status": "not_run", "evidence": ""},
    "playback_cursor_synchronization": {"status": "not_run", "evidence": ""},
    "export_round_trip": {"status": "not_run", "evidence": ""}
  }
}
```

Supply it with `--manual-acceptance path\to\acceptance.json`. A malformed,
incomplete, or extra check is rejected.

## Report states and limitations

- **ready**: automated tests and all supplied deterministic gates passed;
  licensed real-data evidence, matched performance evidence and all manual
  checks are present.
- **pending**: no supplied evidence failed, but one or more required categories
  are missing or unrun. CI reports normally have this state.
- **blocked**: a supplied CTest result, deterministic gate or manual check
  failed.

The report is a decision aid, not a substitute for the signed release checklist.
It records exact report paths, corpus/license provenance, per-system metrics,
per-clip quality, timing samples, model/build/provider identity, and unresolved
limitations. Hardware-specific providers without an actual matching report
remain unqualified.
