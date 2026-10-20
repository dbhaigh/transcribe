# Measured evaluation records

## Synthetic corpus v1

These snapshots measure the **existing engine**, not accuracy improvements:

- [YIN streaming](synthetic-v1-windows-clang23-yin-streaming.json)
- [Spectral full-buffer](synthetic-v1-windows-clang23-spectral-full.json)
- [YIN realtime](synthetic-v1-windows-clang23-yin-realtime.json)

Each contains 16 clips, 24 reference notes, predictions, per-clip metrics,
timings and reproducibility metadata. All three use the identical PCM16 mono
22,050 Hz corpus with seed 2026 and SHA-256
`183c7f68643d6ac54a1b15a26af195e16590f115c5194f25c139366844f6fca9`.

Measured on Windows 11 AMD64, Intel family 6/model 165 with 12 logical CPUs,
Python 3.12.10, LLVM-MinGW Clang 23.1.1, C++20 Release (`-O3 -DNDEBUG`).
Optional integrations were disabled. Each clip had one discarded warmup and
three measured repetitions, with a fresh process for every invocation.
The three benchmark configurations ran sequentially, not against one another.
The checkout was dirty because this evaluation framework was not committed;
the reports preserve its source digests, base revision and executable digest.

Strict events require exact MIDI pitch, onset within 50 ms and offset within
`max(50 ms, 20% of reference duration)`. Frame occupancy is scored separately
at 10 ms cell centers. Counts are micro-aggregated, not averaged clip F1.

| Backend / mode | Event TP / FP / FN | Event precision | Event recall | Event F1 | Onset+pitch F1 | Frame F1 | Chord recall |
|---|---|---|---|---|---|---|---|
| YIN / streaming | 13 / 17 / 11 | 43.33% | 54.17% | 48.15% | 62.96% | 78.33% | 20.00% |
| Spectral / full | 16 / 80 / 8 | 16.67% | 66.67% | 26.67% | 35.00% | 81.01% | 100.00% |
| YIN / realtime | 16 / 6 / 8 | 72.73% | 66.67% | 69.57% | 82.61% | 80.65% | 20.00% |

| Backend / mode | Median onset error | Median offset error | Median engine time | Median process time | Corpus engine RTF |
|---|---|---|---|---|---|
| YIN / streaming | 26.62 ms | 47.66 ms | 40.18 ms | 51.72 ms | 0.01451 |
| Spectral / full | 24.60 ms | 53.24 ms | 15.17 ms | 27.38 ms | 0.00664 |
| YIN / realtime | 22.00 ms | 11.56 ms | 9.82 ms | 21.10 ms | 0.00358 |

Timing medians pool clips of different lengths (including empty input).
RTF weights by corpus audio duration; it is not the median of per-clip RTFs.
Onset/offset errors include only onset+pitch matches: they do not replace recall.
Realtime input-time onset latency had median/p95 **49.98 ms**, pooled over
66 emitted onset events across repetitions. Samples were fed as fast as
possible: this is not device latency or a wall-clock hardware guarantee.
Timing depends on this machine, scheduling, caches and load.

These simple oscillator/effect fixtures do not establish real-instrument,
ensemble, confidence-calibration or score-engraving accuracy. The strict/frame
gap is intentionally visible. A separate short-chunk onset sweep also exposed
pre-existing held-note fragmentation (two events for one note near a seam).
Structural regressions do not assert that fragmentation is absent; event
scoring penalizes extra/unmatched predictions.

### Reproduce and compare

Follow the [framework build instructions](../../docs/evaluation.md), then:

```powershell
python .\python\evaluate.py `
  --runner .\build-evaluation\transcriber-evaluate.exe `
  --build-info .\build-evaluation\evaluation-build-Release.json `
  --backend yin --mode streaming --repetitions 3 --warmups 1 `
  --compare .\python\evaluation\synthetic-v1-windows-clang23-yin-streaming.json `
  --max-event-f1-drop 0.01 `
  --output-json .\build-evaluation\evaluation\candidate.json `
  --output-text .\build-evaluation\evaluation\candidate.txt
```

Change backend/mode and baseline filename together for the other snapshots.
Comparisons reject different corpus hashes, configurations, models, metric
tolerances or timing protocols. Toolchain/binary/environment differences are
reported, not hidden. Cross-platform synthesis can differ at PCM rounding
boundaries; a changed waveform hash needs its own baseline.

An actual same-build repeat comparison returned zero event/frame F1 delta
and passed a zero-drop gate. Timing varied, as expected.
The snapshots were refreshed after fixing equal-onset chord pitch matching
and generating CLI metadata without requiring tests. All three refreshed
runs also passed zero-drop gates against their previous snapshots, with
unchanged event/frame metrics. Metadata changes are disclosed by the
comparison's build-identity flag.

## Validation performed

- Default Release: **40 passed, 1 skipped** across 41 registered CTests.
  The skipped test needs an external real-audio clip.
- Optional ONNX/ASIO Release: **42 passed, 1 skipped** across 43 registered
  CTests, including the existing Basic Pitch chord test and CPU ONNX synthetic
  evaluation. The skipped test also requires external recorded audio.
- Full WebView2/ONNX/ASIO Release GUI and CLI rebuilt successfully with tests
  disabled; compiler metadata was generated without fetching test dependencies.
  The legacy evaluator also completed real CLI ONNX/YIN invocations against
  a tiny local synthetic archive using auto-detected metadata. This checks
  compatibility, not real-recording accuracy.
- Framework tests exercise metric definitions, deterministic corpus identity,
  report/comparison contracts and failure propagation. Archive/provenance
  tests use tiny local fixtures with test-only pinned checksums and mocked
  legacy inference; they are not real-dataset accuracy measurements.
  The Python suite contains 13 tests; all run through CTest with matching
  build paths, while standalone execution intentionally skips three integrations.
- Python syntax checks passed. Catch2 emits Clang `__COUNTER__` extension
  warnings; the builds were not warning-free.
- Actual GuitarSet archives, external recorded audio, ASIO hardware, GPU
  providers, interactive GUI behavior and Linux CI were **not executed** here.
  CI is configured to upload its evaluation reports when it runs.

CPU ONNX also completed all 16 synthetic clips locally (one repetition,
no warmup), with empty-input inference explicitly skipped. That smoke report
is a build artifact, not one of the three repeated baselines above.

## Opt-in temporal decoding records

The original legacy records above are unchanged. New full-buffer, opt-in
canonical-event baselines use the same corpus/tolerances and one warmup plus
three measured fresh-process repetitions:

- [YIN](synthetic-v1-windows-clang23-yin-temporal-full.json):
  15 TP / 7 FP / 9 FN; strict event F1 65.22%, frame F1 80.95%.
- [Spectral](synthetic-v1-windows-clang23-spectral-temporal-full.json):
  20 TP / 3 FP / 4 FN; strict event F1 85.11%, frame F1 97.74%.
- [Basic Pitch CPU ONNX](synthetic-v1-windows-clang23-onnx-temporal-full.json):
  20 TP / 3 FP / 4 FN; strict event F1 85.11%, frame F1 98.26%.

These include accepted canonical events, raw per-event evidence, sample-time
provenance and explicit null calibrated confidence/velocity. They describe an
uncommitted implementation through source and executable digests, not a clean
release commit. The optional record identifies the bundled model by SHA-256;
the built-in runs require no models or datasets. Private recorded-audio
predictions are not included.

See [decoder methods, same-mode legacy comparisons, ablations and
limitations](../../docs/decoding.md). Do not use a temporal snapshot as
`--compare` for legacy output; configurations intentionally differ.

## Versioned model-interface records

These full-buffer records use contract version 1 and preserve the legacy
outputs on the same synthetic-v1 corpus. They do not enable temporal decoding:

- [YIN](synthetic-v1-windows-clang23-yin-interface-full.json):
  strict event F1 48.15%, frame F1 78.43%.
- [Spectral](synthetic-v1-windows-clang23-spectral-interface-full.json):
  strict event F1 26.67%, frame F1 81.01%.
- [Basic Pitch CPU](synthetic-v1-windows-clang23-onnx-interface-full.json):
  strict event F1 53.13%, frame F1 95.57%.

Each quality record uses one fresh-process warmup plus three measured runs.
Per-run metadata includes the actual input/output tensor descriptors, analysis
rate, pitch range, windows, activation semantics, provider and runtime.

The separate [persistent-session CPU benchmark](model-interface-windows-clang23-cpu.json)
uses four in-process calls per clip, with load time separated from cold/warm
inference. Its 60 workloads include the corpus plus low-note, dense-chord and
90-second stress fixtures. All legacy note comparisons matched exactly;
empty ONNX inference was explicitly skipped. No private recording is included.
Only CPU was available; no GPU result is implied.

See [model contracts, measurements, reproduction and limits](../../docs/model-inference.md).
These reports record dirty-source/binary digests, not a clean release commit.
Do not compare interface snapshots against temporal/streaming configurations.

## Historical real-audio record

[guitarset-v1.1.0-results.json](guitarset-v1.1.0-results.json) is the existing
360-recording GuitarSet result, preserved unchanged: Basic Pitch strict event
F1 **14.58%**, frame F1 **72.97%**. It was **not rerun** for this change and
predates the new build/timing metadata. Do not invent missing provenance.

GuitarSet v1.1.0 is CC BY 4.0; see the
[licensed workflow and limitations](../../docs/evaluation.md#licensed-real-audio-workflow)
for attribution, archive identities and commands. Possible Basic Pitch
training-data overlap remains unresolved. Neither that historical guitar-only
result nor synthetic scores establish independent held-out generalization.
