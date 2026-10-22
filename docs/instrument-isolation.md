# Optional instrument isolation for mixed recordings

## Status and scope

The default audio-to-note path is unchanged. Selecting an instrument is a
**prior**, not source recognition: it cannot identify which simultaneous
instrument played a pitch. Isolation is a separate, explicit offline step.
No separator weights are bundled, downloaded at startup, or selected implicitly.
No new production separation model is recommended on the evidence below.

There are two supported entry points:

1. Supply a reviewed, aligned target WAV from a local separator, DAW or recording
   stem. This works with the standard dependency-light C++ build.
2. Run the optional local waveform-ONNX adapter, then review its output through
   the same entry point. This is a strict exported-model contract, **not** a
   generic loader for Demucs `.th`, RoFormer checkpoints or MDX spectrogram masks.
   Such models require their own reviewed architecture-specific adapter or
   external separation tool.

This change establishes an operational preprocessing/review/evaluation workflow,
not a demonstrated improvement from a newly integrated learned separator.
Real multi-instrument note accuracy, production separator sizes, startup times,
RAM/VRAM and throughput remain **unmeasured** without approved checkpoints and
licensed annotated recordings. Synthetic results must not be substituted for them.

## Candidate review (2026-10-21)

Upstream source/metadata was inspected, not just model names. Code licenses do
not automatically establish checkpoint redistribution rights or training-data
rights. No third-party recordings or new separation checkpoints were downloaded.

| Candidate | Maintenance/license evidence | Contract/providers and target fit | Decision |
|---|---|---|---|
| [Audio Separator](https://github.com/nomadkaraoke/python-audio-separator) | MIT runtime; GitHub reported a push on 2026-10-19. [License](https://github.com/nomadkaraoke/python-audio-separator/blob/main/LICENSE). | Maintained host for MDX/VR/Demucs/MDXC/RoFormer families. Upstream documents CPU/CUDA, Apple acceleration and experimental DirectML, with architecture-specific differences. Source names vary by checkpoint. | Suitable external local stem producer after reviewing the exact weights. Its model catalog and automatic acquisition behavior are not imported into the application. Runtime MIT does not clear every catalog model. No checkpoint sizes/resources/quality measured here. |
| [Music Source Separation Training / MSST](https://github.com/ZFTurbo/Music-Source-Separation-Training) | MIT code; GitHub reported a push on 2026-10-15. [License](https://github.com/ZFTurbo/Music-Source-Separation-Training/blob/main/LICENSE). | Actively developed RoFormer, SCNet, MDX23C and other families. PyTorch/config/checkpoint contracts; some separate ONNX/TensorRT exports exist. | Candidate for an architecture-specific adapter. No blanket approval of community weights or their training datasets. No checkpoint benchmark run. Training configurations mentioning 48 GB GPUs are **not inference hardware requirements**. |
| [Demucs v4 / htdemucs_6s](https://github.com/facebookresearch/demucs) | [MIT](https://github.com/facebookresearch/demucs/blob/main/LICENSE); upstream README explicitly says no longer maintained; successor fork only receives important fixes. | Four stems cover bass/vocals but not dedicated guitar/piano; experimental six-stem model adds both. Native PyTorch, not a drop-in pitch ONNX model. | Not selected as the new actively maintained default. Authors explicitly report piano bleeding/artifacts. Their published speed/memory estimates are not measurements on this host. |
| [Basic Pitch](https://github.com/spotify/basic-pitch) | Existing pinned Apache-2.0 model; upstream metadata last push 2025-11-13. Exact provenance/checksum already recorded in the [model catalog](../python/models/basic_pitch.provenance.json). | Instrument-agnostic polyphonic pitch/onset/contour model, not instrument recognition. Upstream says it works best one instrument at a time. Existing CPU ONNX path measured below; provider availability remains explicit. | Retained for transcription after optional isolation. 230,444-byte pinned ONNX file; no new download. Do not describe it as a separator or claim fresh active development from its star count. |
| [MT3](https://github.com/magenta/mt3) | Apache-2.0 code; README calls it an unsupported research project. | T5X autoregressive multi-instrument note/program tokens, not the existing activation tensor interface. | Research candidate, not a lightweight drop-in and not integrated. Weight rights/maintenance/runtime qualification outstanding. |
| [ByteDance piano transcription](https://github.com/ByteDance/piano_transcription) | README targets Python 3.7/PyTorch 1.4 and solo MAESTRO piano. | Piano-specific note/velocity/pedal inference; CPU/CUDA PyTorch. It does not establish mixed-recording piano isolation. | No new integration or maintenance/weight-license claim made. Existing Basic Pitch remains the measured neural transcription baseline. |

Before accepting a new model: pin source revision, exact checkpoint hash/bytes,
weight license and license-text hash; verify source-head names, channel layout,
sample rate, context, temporal delay and training objective; benchmark it on
validation data; then evaluate a separate artist/song/performer-disjoint test set.
Record cases where isolation loses quiet notes or invents attacks, even when SDR
improves. An architecture name or many output channels is not proof of target
instrument support.

## CLI and instrument profiles

```powershell
.\build\transcriber.exe mixture.wav --instrument guitar `
  --isolated-audio guitar-aligned.wav --musicxml guitar.musicxml `
  --tab guitar.txt --score-project guitar.json
```

Profiles are `guitar`, `bass`, `piano`, `voice` (or `none` without preprocessing).
The added Bass prior restricts the built-in spectral candidate range to MIDI
28-72 and at most four simultaneous candidates. This is not a new bass-trained
model. YIN remains monophonic; ONNX objectives/heads remain model-specific.
Low-B five-string bass below MIDI 28 is outside the built-in spectral range.
Profiles do not automatically change tuning or written transposition. For
four-string bass tablature, configure e.g. `--string-tuning 43,38,33,28`.

`isolated-audio` and `preprocess-fallback` are also configuration-file keys.
Missing stems fail by default. `--preprocess-fallback` explicitly permits using
the **original mixture** when a stem/backend/provider is unavailable; the CLI
prints a warning even under `--quiet`. It does not conceal corrupt audio,
bad alignment, invalid tensors, failed inference or cancellation. The optional
Python runner fails explicitly rather than silently switching GPU providers.
To recover, select a supported provider, supply another stem, use the explicit
mixture fallback, or disable preprocessing.

An isolated WAV is only supported for one offline WAV recording, not ASIO or
CLI batch input. Turning preprocessing off performs no extra file scan, model
initialization or Python import. All existing direct WAV/FFmpeg paths remain
available when preprocessing is disabled.

## WebView review

Under **Optional instrument isolation**, choose a local aligned stem, select
Guitar/Bass/Piano/Voice, listen to both mixture and isolated audio, and confirm
the target and alignment. The audition position selects a 30-second region;
inspect several positions to catch source changes, bleed, missing releases and
transients. Audition uses the actual C++ Windows WAV playback backend, not
browser-generated placeholder audio. Audition is unavailable during processing.

Only after confirmation does **Transcribe** use the isolated file. A changed
source/profile invalidates confirmation. The analysis waveform/spectrogram
shows the analyzed stem; **original audio** playback remains the mixture on the
same timebase. Completion displays fallback/isolation warnings. Existing
proposal acceptance protects an already edited score from re-transcription.

Isolation enablement is session-only and is cleared on project open: reselect
and review the stem rather than automatically trusting an old local path.
Projects retain the original source identity and an isolation/provenance warning
in score uncertainties. Manual edits, note timing and exports still use one
canonical score. The normal Stop audition control works before transcription.

## Alignment and preprocessing API

[`AudioPreprocessor`](../include/transcribe/preprocessing.hpp) is an optional
owner-thread file-preparation interface outside DSP, engines, UI state and audio
callbacks. A backend returns a local stem; `prepare_audio` validates it and
returns a `PreparedAudio` path plus explicit diagnostics. Backend-owned
temporary files must outlive analysis. There is no unbounded work queue.

The original and isolated WAV must have **exactly the same sample rate and frame
count**, with sample zero representing the same recording origin. Channel count
may differ; existing decoding downmixes for transcription. No implicit
resampling, time stretching, silence trimming, latency estimation or content
cross-correlation occurs. Known separator delay/padding must be corrected
explicitly by its adapter before admission. Matching rate/length cannot prove
acoustic alignment or that a stem contains the requested source; user review
and annotation comparison remain necessary.

Validation scans the stem in 16,384-frame blocks, checks finite decoded samples,
and checks cancellation between blocks. Input size/mtime snapshots are rechecked
after preprocessing and before score publication; mutation fails rather than
publishing a result for another source. This is a race-detection check, **not a
cryptographic snapshot** against same-size, same-mtime replacement. Use immutable
input copies for strict reproducibility; Python evaluation also records SHA-256.
Outputs are prevented from overwriting the mixture or stem.

The file adapter never changes timestamps. Leading/trailing silence, gaps and
source disappearance/reappearance stay in place. A model's selected head is
fixed across windows, including silent windows; it is never replaced by the
loudest head. Whether that model maintains semantic source identity is an
evaluation question, not guaranteed by tensor labels.

## Optional local waveform ONNX worker

Use a separate Python 3.10+ environment; it is not a CMake/product dependency:

```powershell
python -m venv .venv-separation
.\.venv-separation\Scripts\python.exe -m pip install -r python\requirements-separation.txt
.\.venv-separation\Scripts\python.exe python\separate.py mixture.wav `
  --manifest local-model.json --target guitar --provider cpu `
  --output guitar-aligned.wav --report guitar-separation.json
```

The installation above is explicit; the worker has no network/download code.
Do not run unreviewed weights. The single-file ONNX export must have one float32
input `[1, channels, window_frames]` and one float32 output
`[1, source_count, channels, window_frames]`, with static dimensions and
zero declared latency. One/two channels and 1-16 named source heads are supported.
External tensor files and local ONNX functions are rejected by this contract.

Example metadata (replace placeholders with reviewed values; do **not** simply
mark an unverified license as approved):

```json
{
  "schema_version": 1,
  "contract": "waveform-stems-v1",
  "model": "reviewed-waveform.onnx",
  "sha256": "<exact model SHA-256>",
  "source": "<upstream source URL and revision>",
  "version": "<checkpoint/export version>",
  "weights_license": "<verified SPDX identifier or exact license>",
  "weights_license_verified": false,
  "license_file": "WEIGHTS-LICENSE.txt",
  "license_sha256": "<exact license-text SHA-256>",
  "sample_rate": 44100,
  "channels": 2,
  "window_frames": 262144,
  "context_frames": 32768,
  "latency_samples": 0,
  "stems": ["guitar", "bass", "piano", "voice"]
}
```

PCM16 input must already match the model's rate/channels. No FFmpeg subprocess
or implicit conversion changes the original. Context windows are zero-padded at
recording edges, both context margins are trimmed, and the center advances by
`window_frames - 2*context_frames`. Only the last retained center is shortened.
All original frames are written exactly once. This is **windowed offline
inference**, not a truly streaming separator.

One session is reused across warmups/windows. CPU/CUDA/DirectML/ROCm are explicit
options; the installed runtime must expose the requested provider. The default
requirements install CPU only. GPU distributions/drivers require an explicit
separate installation and compatibility check; no GPU speed benefit is assumed.
The C++ transcription provider and Python separation provider are independent.

Defaults: one warmup, one inference thread, 1,024 MiB model-file limit and
128 MiB window-tensor budget. `--max-model-mib`, `--max-buffer-mib` and `--warmups`
configure these limits. The tensor estimate is
`4 * channels * window_frames * (source_count + 4)` bytes; it excludes ORT model,
graph-optimization/workspace, interpreter and driver allocations. These are
**not hard total RAM/VRAM limits**. Use an OS resource limit or process supervisor
when strict process memory/deadlines are required. No multi-hour model benchmark
or production GPU memory minimum has been established.

`--cancel-file <path>` cancels cooperatively when that file exists; Ctrl+C also
unwinds temporary output. Cancellation is checked before initialization and
between model windows/warmups, not inside an in-flight ORT call or session load.
Normal cancellation/failure deletes the partial WAV. Publication is exclusive
(existing stems are never replaced). Force-killing a process may leave a hidden
`.isolation-*.wav` temporary file. A report-write failure is an explicit error;
a fully published valid stem may remain for inspection.

Reports record model size/hash/license/version, actual provider/runtime,
session initialization, startup, processing/window median/p95/p99, real-time
factor, peak resident memory where available, clipping and audio hashes.
Window timings are warm in-process; repeat the command in separate processes
with distinct outputs to measure cold-start distributions. Compare complete
wall time as well as inference time; short clips can be dominated by startup.

## Paired quality evaluation and datasets

This extends [`evaluate.py`](../python/evaluate.py) and the existing shared
[`evaluation_metrics.py`](../python/evaluation_metrics.py), not a competing scorer.
For a clean model-free checkout:

```powershell
cmake -S . -B build-evaluation -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-evaluation --parallel
python python\evaluate.py --runner build-evaluation\transcriber-evaluate.exe `
  --build-info build-evaluation\evaluation-build-Release.json `
  --corpus mixed --backend all --mode full --warmups 1 --repetitions 3 `
  --output-json build-evaluation\evaluation\mixed.json `
  --output-markdown build-evaluation\evaluation\mixed.md
ctest --test-dir build-evaluation -R "separation_contracts|mixed_instrument_evaluation|cli_export_validation|ui_workflow_browser" --output-on-failure
```

On Linux use an installed generator and omit `.exe`; Visual Studio executables
are under `Release`, with `--config Release` for build and `-C Release` for CTest.
Without `--paired-manifest`, this generates deterministic harmonic **oracle and
deliberately damaged stems**, not learned separations or natural instruments.
The damaged cases have quiet interference, one deleted target note and a
hallucinated pitch, so regressions cannot be hidden by easy oracle gains.

For licensed recordings add `--paired-manifest pairs.json` and, for final
evaluation, `--dataset-split test`. Manifest schema:

```json
{
  "schema_version": 1,
  "name": "reviewed-mixed-corpus",
  "version": "1",
  "provenance": "<dataset revision/archive checksum>",
  "license": "<dataset license and attribution>",
  "training_data_warning": "<separator/transcriber training overlap or unknown>",
  "pairs": [{
    "id": "song-01-guitar",
    "target": "guitar",
    "original": "song-01-mixture.wav",
    "isolated": "song-01-guitar-estimate.wav",
    "reference_audio": "song-01-guitar-reference.wav",
    "notes": [{"midi": 64, "start": 0.25, "duration": 0.5}],
    "dataset_split": "test",
    "performer_id": "artist-or-song-group-01",
    "source": "<recording and annotation provenance>",
    "license": "<recording/annotation rights>",
    "separation": {
      "method": "<separator and configuration>",
      "learned_model": true,
      "model_sha256": "<64 lowercase hex characters>",
      "weights_license": "<verified license>",
      "model_source": "<checkpoint source>",
      "model_version": "<checkpoint/export version>",
      "provider": "<actual provider>"
    }
  }]
}
```

Paths are relative to the manifest; `reference_audio` is optional. Notes must
annotate only the target, in original recording seconds. Performer/song groups
and identical input/reference hashes may not cross train/validation/test splits.
Validation chooses settings; final test data must not tune them. Exact hashes
do not detect alternate masters/edits: perform dataset-level leakage review too.
One target instrument profile/configuration is used for both variants of a pair.

JSON retains all existing precision/recall/F1, frame activity, onset/offset
errors, fragmentation, merging, false-positive rate, timings and memory metrics
for each variant, and explicit per-pair deltas/degraded metric names. Markdown
lists pair deltas, rather than claiming an aggregate improvement. When a target
waveform is available, bounded-block zero-mean mono SI-SDR, RMS and downmixed
clipping diagnostics are added. Silence and exact-up-to-scale cases have an
explicit status and null dB rather than invalid JSON infinities. SI-SDR neither
proves correct notes nor verifies source identity.

Dataset suitability:

- [Slakh2100](https://github.com/ethman/slakh-generation): mixtures, stems and
  aligned MIDI make it suitable for guitar/bass/piano target-note comparisons.
  It is synthesized, not evidence for natural vocals or all real playing
  techniques. Obtain and verify the exact dataset's license/attribution and
  official splits; no archives were available or downloaded here.
- [MUSDB18/HQ](https://sigsep.github.io/datasets/musdb.html): real bass/vocals
  stems, academic-access restrictions, known bleed/label errors. Appropriate for
  separation artifacts, **not automatically note-event F1**: target note labels
  must be independently supplied. Four stems do not isolate guitar/piano.
- [MoisesDB](https://github.com/moises-ai/moises-db): broader real instrument
  stems/activity and bleed annotations; CC BY-NC-SA 4.0 according to upstream.
  Requires appropriate noncommercial rights and target note annotations for
  note-event comparisons. Not bundled or treated as a commercial training source.
- Existing GuitarSet and solo-piano datasets alone are not mixed-recording
  separation benchmarks. Basic Pitch/GuitarSet leakage warnings still apply.

## Measurements and remaining qualification

Windows 11, Python 3.12.10, Clang 23.1.1 Release; 8 synthetic pairs (16 files,
3 seconds each), one discarded process warmup and two measured process-cold
transcription repetitions. Timing is machine/load-dependent; no thresholds were
changed. Neural transcription used the existing 230,444-byte Basic Pitch model,
CPU ONNX Runtime 1.20.1, aligned event decoder.

**Event F1: original mixture -> supplied stem**

| Target / condition | YIN | Spectral | Basic Pitch |
|---|---:|---:|---:|
| Guitar / oracle | 0.2500 -> 0.8000 | 0.2857 -> 0.4000 | 0.6667 -> 1.0000 |
| Guitar / damaged | 0.4000 -> 0.3333 | 0.1176 -> 0.2000 | 0.5714 -> 0.5000 |
| Bass / oracle | 0.0000 -> 0.4000 | 0.0645 -> 0.2222 | 0.3333 -> 1.0000 |
| Bass / damaged | 0.5000 -> 0.4000 | 0.1818 -> 0.0741 | 0.6667 -> 0.5000 |
| Piano / oracle | 0.0000 -> 0.8000 | 0.2667 -> 0.4000 | 0.6667 -> 1.0000 |
| Piano / damaged | 0.5000 -> 0.3333 | 0.1111 -> 0.2000 | 0.5000 -> 0.5000 |
| Voice / oracle | 0.0000 -> 0.6667 | 0.0690 -> 0.4444 | 0.6667 -> 1.0000 |
| Voice / damaged | 0.4000 -> 0.2857 | 0.2000 -> 0.2222 | 0.6667 -> 0.5000 |

All damaged pairs reduced frame F1 for **every** tested engine, even where event
F1 improved or stayed equal. Basic Pitch frame-F1 deltas were -0.0797 guitar,
-0.0714 bass, -0.0772 piano and -0.0755 voice. Thus neither better event F1 nor
an aggregate oracle gain establishes that preprocessing helps a particular clip.
These are controlled artifact-sensitivity demonstrations, not measurements of
Demucs/RoFormer/SCNet separation accuracy.

| Transcription engine, all 16 files | Median decode+analysis | p95 | Median peak RSS |
|---|---:|---:|---:|
| YIN | 23.27 ms | 32.12 ms | 5.30 MB |
| Spectral | 18.49 ms | 29.50 ms | 8.19 MB |
| Basic Pitch | 159.09 ms | 183.28 ms | 39.22 MB |

For the **158-byte generated identity ONNX graph only** (not a separator), real
CPU ORT 1.31.0 execution was verified on a 1,301-frame/8 kHz test input:
5.40 ms session initialization, 7.84 ms window processing including WAV I/O,
0.0482 processing RTF, 68.84 MB peak RSS, and 14 windows after one warmup
(median 0.0295 ms, p95/p99 0.0465 ms per invocation). This measures worker
wiring/runtime overhead, not production model requirements; CPU model-free C++
and optional Python ORT versions are intentionally reported separately.
CUDA/DirectML/ROCm were unavailable and their explicit rejection was tested,
not their performance.

Tests cover sample-exact trimming/edge padding, late impulses/source changes,
stereo and short final windows, fixed target heads including silence, bounded
buffers, clipping, wrong rates/shapes, nonfinite outputs, cancellation/failure
cleanup, missing providers, checksums, source mutation, annotation/split checks,
CLI/config fallback, file protection and browser review protocol. Physical
Windows audition hardware and real learned-separator inference were not tested.
Do not enable preprocessing by default until a vetted checkpoint demonstrates
per-instrument benefit on independently annotated mixed recordings.

### Validation performed

- Release preprocessing/engine tests: 3,749 assertions in 15 cases passed.
- AddressSanitizer/UndefinedBehaviorSanitizer preprocessing tests: 49 assertions
  in 4 cases passed; Windows leak detection was disabled.
- Six selected CTests passed: mixed-backend evaluation, separation contracts,
  existing evaluation framework, CLI export/configuration, browser workflow and
  dependency inventory. The mixed suite verifies identical corpus identity
  across engines even when fixtures are generated in different directories.
- Optional isolated Python environment: all 14 tests passed, including actual
  CPU ONNX single/multi-head inference, malformed-model initialization failure,
  explicit unavailable-provider rejection and cleanup. The standard Python
  environment skips the optional real-ONNX case rather than installing packages.
- Strict WebView2 + ONNX + ASIO + Verovio GUI build passed. This is compile
  coverage, not ASIO hardware, audible playback or GPU validation.

No production separator checkpoint, real annotated mixed-recording corpus or
GPU hardware benchmark was run. Those remain qualification requirements rather
than silently skipped evidence of success.
