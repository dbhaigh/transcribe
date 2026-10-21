# Engineering audit

## 1. Scope, baseline, and interpretation

Audit date: 2026-10-20. Source baseline:
`0561397b3705aeb8763726ea4b73fd83b4012aac` (Transcriber 0.1.4).
The worktree was clean at the start. This phase changes documentation only;
it does not implement fixes, replace the architecture, or retune the model.
The implementation sequence is in [the roadmap](roadmap.md).

This is a point-in-time audit of that baseline, not a current inventory of
later implementation or CI changes. Release and workflow status is summarized
in [the release checklist](release-checklist.md) and the current
[README](../README.md); audit findings remain evidence about the named source
baseline unless explicitly updated in a later change.
The project MIT license and WebView2-only graphical frontend were added after
this baseline audit.

This is an engineering/correctness audit, not a claim of commercial AMT
quality, a penetration test, or a legal opinion.

Evidence labels used below:

- **Confirmed:** a behavior follows directly from the inspected source.
- **Reproduced:** also exercised by a bounded local check during this audit.
- **Risk:** a plausible failure path supported by source, not reproduced.
- **Limitation/gap:** an intentional restriction or missing assurance, not
  necessarily a bug or a contradiction of the documentation.

Severity: **High** means potential lost/corrupted results, unsafe lifetime,
or a material blocker to reliable transcription/distribution. **Medium**
means incorrect conditional behavior, significant accuracy/resource limits,
or an important validation gap. **Low** means maintainability or
reproducibility work with less immediate user impact. Priority is not a claim
that every documented limitation must be eliminated.

### Inspection inventory

The following text sources were read, including both Windows front ends:

| Area | Inspected files |
|---|---|
| Build and CI | [CMakeLists.txt](../CMakeLists.txt), [CI workflow](../.github/workflows/ci.yml), [.gitignore](../.gitignore) |
| All public headers | [audio.hpp](../include/transcribe/audio.hpp), [asio_capture.hpp](../include/transcribe/asio_capture.hpp), [transcriber.hpp](../include/transcribe/transcriber.hpp), [musicxml.hpp](../include/transcribe/musicxml.hpp), [guitar_tab.hpp](../include/transcribe/guitar_tab.hpp) |
| Engine and decoders | [transcriber.cpp](../src/transcriber.cpp), [audio_file.cpp](../src/audio_file.cpp), [ffmpeg_audio.cpp](../src/ffmpeg_audio.cpp) |
| Capture/concurrency | [asio_capture.cpp](../src/asio_capture.cpp), [audio_ring_buffer.cpp](../src/audio_ring_buffer.cpp), realtime implementation in [transcriber.cpp](../src/transcriber.cpp) |
| CLI and exporters | [main.cpp](../src/main.cpp), [musicxml.cpp](../src/musicxml.cpp), [guitar_tab.cpp](../src/guitar_tab.cpp) |
| Windows GUIs | [gui.cpp](../src/gui.cpp), [gui_webview.cpp](../src/gui_webview.cpp), [gui_transcription.hpp](../src/gui_transcription.hpp), [gui_transcription.cpp](../src/gui_transcription.cpp) |
| Web UI | [index.html](../ui/index.html), [styles.css](../ui/styles.css), [app.js](../ui/app.js), [editor.js](../ui/editor.js) |
| Tests and fixtures | [Catch2 tests](../tests/transcribe_tests.cpp), [CLI export smoke](../tests/cli_export_smoke.py), [UI smoke](../tests/ui_smoke.py), [real-audio smoke](../tests/real_audio_smoke.py), [sample generator](../samples/generate_samples.py), [sample provenance](../samples/README.md), both expected-pitch fixture files |
| Evaluation/provenance | [project README](../README.md), [Python README](../python/README.md), [evaluator](../python/evaluate_guitarset.py), [model README](../python/models/README.md), [model license](../python/models/LICENSE-basic-pitch.txt), structured summaries and per-recording count aggregation from [stored GuitarSet results](../python/evaluation/guitarset-v1.1.0-results.json) |

The bundled model was hashed, not reverse-engineered or re-exported. The
large evaluation JSON was inspected structurally and aggregated, not treated
as evidence that the evaluation was rerun. External dataset archives,
upstream license histories, and current GitHub CI run results were not
downloaded or independently validated.

## 2. Dependency and execution map

### Build-time and runtime dependencies

```text
CMake (>=3.20), C++20 compiler
|
+-- transcriber_core [STATIC, public include directory, C++20]
|   +-- audio_file / audio_ring_buffer / transcriber / musicxml / guitar_tab
|   +-- asio_capture stub by default
|   +-- optional RtAudio ASIO + Windows driver; MinGW winpthread
|   +-- optional ONNX Runtime + model
|   |   +-- optional CUDA / DirectML / ROCm providers and matching SDK/runtime
|   +-- optional FFmpeg avformat/avcodec/avutil/swresample via pkg-config
|
+-- transcriber [CLI] -> transcriber_core
+-- Windows transcriber_gui_backend -> transcriber_core
|   +-- transcriber-gui [native Win32] -> Win32 system libraries
|   +-- OR transcriber-gui [WebView2] -> loader + Evergreen runtime + local ui/
|
+-- optional transcriber-tests -> core + fetched Catch2 3.7.1
    +-- optional Python CTest integrations
        +-- UI syntax checks additionally use Node when available
```

See [target definitions/options](../CMakeLists.txt#L4) and
[optional dependency linkage](../CMakeLists.txt#L158).
All feature switches, GPU providers, and tests default to OFF. The default
WAV/YIN core has no FFmpeg, ONNX, Python, Node, or WebView dependency.
Windows' default GUI uses operating-system Win32 APIs.

C++20 is propagated with `target_compile_features`. Production targets and
tests receive `/W4 /permissive-` on MSVC or
`-Wall -Wextra -Wpedantic` otherwise. There is no project-wide sanitizer,
warning-as-error, LTO, or benchmark policy. Those omissions alone are not
correctness defects. Release optimization comes from the selected generator
and configuration, not a project-specific optimization flag.

### Audio-to-output dependencies

```text
CLI parse_options / config                         GUI Request / message parsing
             |                                         |
             +--------------------+--------------------+
                                  |
WAV -> read_wav_blocks -> mono float blocks -> transcribe_wav_streaming
 |                            |                   |
 +-> read_wav -> AudioBuffer   |              overlapped chunk/core merge
Other audio -> FFmpeg -> mono 44.1 kHz AudioBuffer       |
ASIO callback -> recording SPSC ring -> full recording  |
             -> analysis SPSC ring -> realtime YIN     |
                         -> stable Onset/Offset events |
                                  |                    |
                                  +--------------------+
                                                       |
built-in offline: preprocess -> onsets/FFT -> resample -> YIN
                         OR cached spectrogram -> harmonic salience/greedy pitches
ONNX generic: full input -> 16 kHz -> one Run -> generic v1 pitch frames
ONNX Basic Pitch: full input -> 22.05 kHz -> overlapped fixed-window Runs
                                             -> stitched frame probabilities
                                                       |
                           frame grouping -> vector<Note> in seconds
                                                       |
                 infer_score_metadata (tempo/key/meter/swing heuristics)
                                                       |
         +--------------------+-------------------+----------------------+
         |                    |                   |                      |
   MusicXML voices,      guitar fingering,     MIDI events,         CLI JSON,
   quantization/ties     ASCII tab/alternates  480 PPQ              GUI messages
         |
   notation + tab parts share selected positions

WebView completion/live messages -> editor note/position state -> canvas/Web Audio
WebView edited export -> parsed notes/positions -> same C++ writers
```

There is no common persistent quantized score/voice graph: public `Note`
objects hold performance times, while MusicXML constructs its own private
voice/token graph. MIDI uses continuous-time-derived ticks; the canvas
positions notes in seconds. These are different representations, not
automatically equivalent round trips.

### Ownership and synchronization

- [The ring](../src/audio_ring_buffer.cpp#L13) uses monotonic atomic indices
  and release/acquire publication for one producer and one consumer. Its
  operations do not allocate after construction. It is not an MPMC queue.
- [Capture](../src/asio_capture.cpp#L23) has separate recording and analysis
  rings. The RtAudio callback only writes samples/atomics; the capture caller
  drains recording and delivers callbacks; a dedicated `jthread` drains
  analysis. Analysis exceptions are transferred after joining.
- [RealtimeTranscriber](../src/transcriber.cpp#L1166) locks its public
  operations and copies incoming samples. Its public type is distinct from
  a streaming ONNX session.
- [Offline FFT workers](../src/transcriber.cpp#L291) own reusable workspaces;
  the single-entry spectrogram cache and ONNX session map use mutexes.
- Both GUIs marshal worker results via posted window messages and normally
  join before deferred destruction. Successfully posted heap payloads pass
  to the window procedure; failed posting retains `unique_ptr` ownership.
  Startup COM callbacks have a separate lifetime concern (F19).

### Build variants and CI evidence

| Variant | Repository CI configuration | Local audit evidence |
|---|---|---|
| Default/native Windows | Windows runner configures tests, builds Release, runs CTest | LLVM-MinGW Release: build and default suite run |
| WebView2 Windows | Separate WebView2 build and ZIP; tests run against the default build | WebView2 + ONNX + ASIO Release build succeeded; no interactive GUI check |
| Linux GCC/Clang, FFmpeg OFF/ON | Four matrix combinations, CTest, compressed FLAC smoke, TGZ | Not run on this Windows host |
| CPU ONNX | Linux ONNX Runtime 1.20.1, tests | Existing Windows ONNX + ASIO Release build rebuilt; two selected tests run |
| ASIO hardware | No ASIO job | Compiled only; device/driver capture not exercised |
| CUDA/DirectML/ROCm | No provider job | Not built or executed |

Local build directories are caches, not authoritative feature presets. Some
have old `transcribe*` artifacts alongside current `transcriber*` targets;
the incomplete NMake release directory and unoptimized older directories do
not establish a working Release variant.

### Licensing and provenance inventory

| Component | Evidence and qualification |
|---|---|
| Repository code | No tracked top-level license file was present. Do not infer a license from public GitHub availability. |
| Basic Pitch model | Pinned upstream commit `fa5997af0a8210982619003269994a1be25eddf3`; Apache-2.0 license included. Current 230,444-byte model SHA-256 is `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`, matching the stored evaluation identity. Exact export reconstruction is not documented. |
| GuitarSet | Documentation identifies v1.1.0, DOI, CC BY 4.0, archive MD5s, and possible model-training overlap. No recordings are redistributed in the repository. Archive checksums were not rerun here. |
| Synthetic WAV fixtures | Deterministic generator and expected pitches are present; these are not recorded-audio accuracy evidence. A project license still needs to establish repository-wide reuse terms. |
| WebView2 | SDK version 1.0.4258.31 in CI; loader license/notice and Evergreen bootstrapper are copied/installed. Bootstrapper URL is mutable; download validates TLS/status/size, not a pinned digest. |
| ONNX Runtime | CI downloads 1.20.1; local configured SDK is 1.20.1. Runtime/provider binaries and their notices need a release-payload audit; the model's Apache license is not the runtime's license. |
| RtAudio / ASIO | CMake finds an externally installed package without a version constraint. Inspected local header identifies RtAudio 6.0.1. Verify RtAudio and the specific ASIO SDK distribution terms separately; no license violation is asserted. |
| FFmpeg | External development libraries, optional only. License/redistribution requirements depend on the actual configured FFmpeg build and linked codecs; the repository does not record those choices. |
| Catch2 | Test-only FetchContent dependency pinned to tag v3.7.1; not a mandatory production dependency. |

## 3. Validation actually executed

Host: Windows x64; CMake 4.4.4, Python 3.12.10, LLVM-MinGW Clang 23.1.1.
Node was not available on the command PATH.

| Check | Outcome | What it does not prove |
|---|---|---|
| VS Code unit-test runner for the Catch2 source | No tests discovered | Not a test pass; CTest was used instead |
| `cmake --build .\build-release-mingw --parallel` and `ctest --test-dir .\build-release-mingw -C Release --output-on-failure` | Build succeeded; **30 passed, 1 skipped**, 31 registered; CTest reported 1.23 s | External real-audio smoke skipped; no hardware, GUI interaction, or wall-clock performance qualification |
| Rebuild optional ONNX/ASIO directory; CTest selector `Basic Pitch ONNX\|cli_export_validation` | Build succeeded; **2 selected tests passed**, 1.25 s | Not the entire optional suite or an ASIO/GPU run |
| `cmake --build .\build-webview-onnx-asio-mingw --parallel` | CLI and modern GUI targets succeeded | No visual/browser/COM-shutdown behavior exercised |
| Install full-featured build to an isolated prefix; inspect PE imports with `llvm-readobj --coff-imports` | Only WebView2 loader DLL installed; imports include missing ONNX and LLVM-MinGW DLLs (F02) | No clean-machine launch or redistribution test |
| CLI malformed numeric probes with `--help` | `--asio nan`, `--tempo 96junk`, `--device-id 1.5` all exited 0 (F04) | No invalid ASIO hardware invocation attempted |
| Isolated C++20 harness linked to current default Release core | Reproduced YIN range/aliasing, MusicXML duration mismatch, invalid tab alternatives, and lost explicit override (F07/F08/F13/F15) | Synthetic probes, not a real-audio benchmark |

The optional rebuild emitted 59 Catch2 macro `__COUNTER__` extension warnings
under this newer compiler. It succeeded; this is not described as a
warning-free build or a production-source failure.

The UI smoke prints that JS syntax checking was skipped when Node is absent.
A passing asset test therefore did not validate JavaScript execution here.
The GuitarSet dataset evaluation, Linux jobs, packaged-app clean-machine
execution, sanitizer runs, capture hardware, GPU inference, and interactive
GUI tests were **not run**.

Temporary probes and the isolated install were kept outside the repository
and removed after their results were recorded.

## 4. Accuracy investigation: frame F1 is not note-event F1

Evidence: [event matching](../python/evaluate_guitarset.py#L80),
[frame occupancy scoring](../python/evaluate_guitarset.py#L129), and
[stored evaluation](../python/evaluation/guitarset-v1.1.0-results.json).
The 360 per-recording count sums were checked against the stored aggregate
counts. Percentages below were calculated from those counts, not from a new
transcription run.

| System / metric | TP | FP | FN | Precision | Recall | F1 |
|---|---:|---:|---:|---:|---:|---:|
| Basic Pitch onset + pitch | 28,748 | 82,292 | 33,728 | 25.89% | 46.01% | 33.14% |
| Basic Pitch strict note event | 12,646 | 98,394 | 49,830 | 11.39% | 20.24% | **14.58%** |
| Basic Pitch 10 ms frame | 1,288,384 | 349,271 | 605,393 | 78.67% | 68.03% | **72.97%** |
| YIN onset + pitch | 10,124 | 18,143 | 52,352 | 35.82% | 16.20% | 22.31% |
| YIN strict note event | 1,931 | 26,336 | 60,545 | 6.83% | 3.09% | 4.26% |
| YIN 10 ms frame | 263,918 | 170,208 | 1,629,859 | 60.79% | 13.94% | 22.67% |

Strict matching requires exact MIDI pitch, an onset within 50 ms, and an
offset within `max(50 ms, 20% of reference duration)`. Matches are
maximum-cardinality one-to-one using augmenting paths, not naive greedy
pairing. Onset-only matching removes the offset constraint.

Frame scoring compares Boolean active-pitch occupancy on a rounded 10 ms
grid. It ignores the identity of repeated same-pitch events. A fragmented
long note or incorrectly merged repetition can occupy largely correct
frames while failing event matches. The final partial-duration grid cell
also counts as one frame rather than a fraction.

There are **111,040 predicted Basic Pitch events for 62,476 references**.
Adding offset eligibility reduces matched events from 28,748 to 12,646.
These facts support substantial unmatched events and an additional
boundary/duration problem; they do not prove a unique cause such as a
particular threshold, resampler, or adapter window seam. Both onset and
offset behavior need investigation; blaming only offsets would be wrong.

The 180 comp recordings have Basic Pitch strict F1 14.00% / frame F1 73.23%;
the 180 solo recordings have 15.93% / 72.29%. The issue is not limited to
the chordal subset. YIN is a monophonic baseline on material that includes
polyphony, not a competing general polyphonic model.

This is a diagnostic dataset result, not an independent held-out
generalization result: possible GuitarSet overlap with Basic Pitch training
is explicitly acknowledged. Future decoder changes need frozen validation
protocols, recording-level splits, onset/offset diagnostics, repeated-note
tests, and a separate real-world holdout before making quality claims.

## 5. Findings

### F01 - Release selection differs by generator

- **Severity / impact:** Medium; confirmed build/documentation gap. Linux CI
  and single-config users can believe an unoptimized build is Release.
- **Source / evidence:** [README build instructions](../README.md#L35) and
  [Linux/ONNX workflow](../.github/workflows/ci.yml#L66) configure without
  `CMAKE_BUILD_TYPE` and then use `--config Release`. Makefile generators
  choose optimization at configure time; that build flag does not select it.
  Existing local default/native/test caches have an empty build type.
- **Proposed solution:** Specify Release when configuring single-config
  release/benchmark jobs; document both generator forms. Keep Debug usable.
- **Dependencies / regression risks:** None; optimization can expose latent
  UB and changes benchmark comparability. Do not change every developer
  build to Release implicitly.
- **Acceptance:** Single-config release CI records Release and optimized
  flags; multi-config Release still builds; benchmark reports identify both.

### F02 - Installed full-featured Windows payload omits required DLLs

- **Severity / impact:** High; reproduced incomplete payload. Clean-machine
  startup is not assured and PATH can supply a mismatched ONNX runtime.
- **Source / evidence:** [ONNX linkage](../CMakeLists.txt#L175) links a raw
  library path; [install rules](../CMakeLists.txt#L274) install applications,
  WebView2 assets, and model, but not ONNX or compiler runtime DLLs.
  The audited GUI imports `onnxruntime.dll`, `libc++.dll`, `libunwind.dll`,
  and `libwinpthread-1.dll`; none appeared in the isolated install.
- **Proposed solution:** Explicitly stage/install supported redistributable
  dependency closures per toolchain/feature set, with matching notices.
  Validate installed artifacts, not just the build tree.
- **Dependencies / regression risks:** F03 licensing inventory; provider-
  specific dependencies and MSVC versus MinGW rules differ. Do not copy
  arbitrary system DLLs or resolve releases through a developer PATH.
- **Acceptance:** Default and ONNX/WebView2 packages launch and transcribe
  in a clean Windows environment without development-tool paths; enabled
  providers use the intended runtime version or fail with a clear message.

### F03 - Distribution licensing and reproducible provenance are incomplete

- **Severity / impact:** High release-readiness gap, not an asserted legal
  violation. Users cannot determine repository reuse terms or the complete
  dependency notice set from a release.
- **Source / evidence:** No top-level license in the tracked inventory.
  [Installation](../CMakeLists.txt#L274) includes WebView2 and model license
  files, not an inventory for RtAudio/ASIO, ONNX, compiler runtimes or FFmpeg.
  [Model provenance](../python/models/README.md) pins upstream but gives no
  exporter environment/opset/command to reproduce the ONNX bytes.
- **Proposed solution:** Obtain the owner's project-license decision; record
  actual dependency versions/build options/licenses; bundle required
  notices. Document the model's artifact origin or reproducible export.
- **Dependencies / regression risks:** Owner/legal decisions and actual
  packaged dependencies; do not invent licenses or require optional SDKs
  in default builds. Notice obligations vary by distribution.
- **Acceptance:** Every distributed non-system binary/model maps to version,
  origin, digest and license/notice; project licensing is explicit; a pinned
  model artifact is verifiable or its reproducible conversion is documented.

### F04 - Numeric parsing accepts trailing junk and NaN capture duration

- **Severity / impact:** Medium; reproduced validation defect. Invalid
  input is accepted, and NaN can reach floating-to-size conversion in ASIO.
- **Source / evidence:** [parse_options/apply_config](../src/main.cpp#L210)
  use `stoi/stod/stoul` without checking full consumption.
  [ASIO checks](../src/main.cpp#L461) and
  [capture_asio](../src/asio_capture.cpp#L67) test `<= 0` / `> 300`, not
  finiteness. The three malformed `--help` probes in section 3 exited 0.
- **Proposed solution:** Reuse strict numeric parsers with finite/range
  validation in CLI/config and enforce the same invariant at capture entry.
- **Dependencies / regression risks:** None; malformed strings previously
  accepted will intentionally become errors. Preserve valid option names,
  units and CLI-over-config precedence.
- **Acceptance:** CLI/config reject `nan`, infinities, trailing text and
  fractional integers with nonzero exit and an option-specific error;
  `capture_asio(NaN)` rejects before device access.

### F05 - ASIO exception teardown can outlive callback state

- **Severity / impact:** High; source-backed lifetime risk, not hardware-
  reproduced. Exceptions from user callbacks/allocation can leave an active
  callback referring to destroyed stack state.
- **Source / evidence:** [capture_asio](../src/asio_capture.cpp#L73) constructs
  `RtAudio audio` before `CaptureState`; explicit stop/close is reached only
  on the normal path at lines 163-166. Progress/live/audio callbacks and
  audio-preview allocation occur while streaming. During unwinding, state is
  destroyed before the earlier-constructed RtAudio object.
- **Proposed solution:** Establish an exception-safe stream guard whose
  stop/close happens before worker and callback-state teardown. Preserve
  the original exception and make cleanup errors observable.
- **Dependencies / regression risks:** F06 clarifies RtAudio return semantics.
  Stop/join ordering must not deadlock the analysis worker or discard normal
  final events. No change to the callback's real-time allocation policy.
- **Acceptance:** Fault-inject every external callback and preview allocation;
  a fake stream verifies stop/close before state destruction on every exit.
  Hardware/ASan-assisted validation shows no callback after teardown.

### F06 - RtAudio errors and device overflow status are not propagated

- **Severity / impact:** High for undiagnosed recording failure/data gaps;
  confirmed unchecked paths, hardware behavior not exercised.
- **Source / evidence:** [capture_callback](../src/asio_capture.cpp#L35)
  ignores `RtAudioStreamStatus`; its overflow flag covers application rings
  only. [Stream operations](../src/asio_capture.cpp#L93) ignore return values
  from open/start/stop. The installed RtAudio 6.0.1 header returns
  `RtAudioErrorType` and exposes `getErrorText()`; it does not require these
  failures to throw. Capture also hardcodes 44.1 kHz/default channel.
- **Proposed solution:** Define/test a supported RtAudio API version,
  check operation results, and carry device overflow/discontinuity status
  out of the callback through atomics and explicit diagnostics.
- **Dependencies / regression risks:** Coordinate with F05 and a fake-stream
  seam; do not log, allocate or invoke UI handlers from the audio callback.
  Device/rate selection is separate optional work, not a required rewrite.
- **Acceptance:** Inject open/start/stop errors and input overflow; caller
  receives the original cause or explicit incomplete-recording status, never
  an apparently complete uninterrupted success.

### F07 - Linear downsampling aliases high-frequency energy into pitches

- **Severity / impact:** High transcription correctness; reproduced.
- **Source / evidence:** [resample_linear](../src/transcriber.cpp#L171)
  interpolates samples without a target-Nyquist antialias filter; it serves
  YIN and both ONNX adapters. A 1 s, 44.1 kHz, 10,585 Hz sine transcribed as
  MIDI 69 at confidence 0.9999, identical to a 440 Hz control.
- **Proposed solution:** Introduce a small dependency-free band-limited
  downsampling path (or adequate prefilter), shared by the analysis rates.
- **Dependencies / regression risks:** First pin fixtures and current
  outputs. Filtering changes model activations, boundaries, delay and CPU;
  do not attribute the stored event F1 entirely to this defect.
- **Acceptance:** At 44.1 -> 11.025 kHz, specified stopband test tones are
  attenuated at least 40 dB, 440 Hz passband gain stays within 0.1 dB,
  pitch error within 1 cent where estimated, and the alias fixture no
  longer produces a confident A4. Report delay/CPU and model metric deltas.

### F08 - YIN's effective lag range is narrower than its accepted MIDI range

- **Severity / impact:** Medium; reproduced register limitation and
  high-confidence subharmonic error.
- **Source / evidence:** [yin_with_workspace](../src/transcriber.cpp#L73)
  bounds lags around 65-1000 Hz, then accepts MIDI 28-96 at line 134.
  With adequate-length 11.025 kHz synthetic frames, MIDI 28 returned no
  pitch and MIDI 96 returned 77/confidence 0.972156; 40/69/83 were correct.
- **Proposed solution:** Make the supported frequency bounds explicit and
  consistent with lag/window choices; extend offline range only with
  adequate windows. Expose shorter realtime-window limits honestly.
- **Dependencies / regression risks:** F07 and register fixtures. Wider
  searches can increase CPU and octave confusion; a 20 ms window cannot
  promise the same low-register behavior as long offline frames.
- **Acceptance:** Published per-mode bounds match tested behavior;
  representative boundary pitches are within one semitone inside those
  bounds, and unsupported registers are not advertised as supported.

### F09 - Event accuracy is inadequate; its exact failure decomposition is missing

- **Severity / impact:** High quality limitation, not a new claim that the
  documented diagnostic result was hidden.
- **Source / evidence:** Section 4 and
  [Basic Pitch decoding](../src/transcriber.cpp#L777): 14.58% strict F1,
  33.14% onset F1, 72.97% frame F1 and 111,040 predictions versus 62,476
  references. Current synthetic chord test only requires each expected
  pitch to appear, not correct boundaries or few false positives.
- **Proposed solution:** Add event-boundary/repetition diagnostics and
  validation splits before isolated decoder experiments. Investigate
  activation hysteresis, gaps, onsets/offsets and window seams separately;
  do not replace the model or assert an unmeasured root cause.
- **Dependencies / regression risks:** Frozen provenance/protocol (F23),
  F07 and meaningful tests (F25). Avoid tuning on the reported evaluation
  set and advertising it as independent generalization.
- **Acceptance:** Recomputed baseline counts agree exactly; diagnostics
  identify FP/FN/duration/repetition categories on a fixed validation set.
  Promote a decoder change only with declared event-F1 improvement and
  bounded frame-F1/precision regression on recording-separated data.

### F10 - ONNX remains full-recording in memory; Basic Pitch has avoidable work

- **Severity / impact:** Medium; confirmed resource limitation plus a
  specific redundant operation.
- **Source / evidence:** [transcribe_with_onnx](../src/transcriber.cpp#L1978)
  resamples all input to 16 kHz before Basic Pitch dispatch; Basic Pitch
  independently resamples to 22.05 kHz at line 713. The generic adapter
  runs one full `[1, samples]` tensor (lines 2006-2017). Basic Pitch uses
  43,844-sample windows but retains stitched 69-pitch double probabilities
  and decoded frame data (lines 720-810). Audio decode is also full-buffer.
  One hour of 16 kHz float input alone occupies 230.4 MB (decimal).
- **Proposed solution:** First dispatch before the unused 16 kHz resample.
  Then incrementally decode bounded Basic Pitch windows with overlap state.
  Keep generic full-input semantics unless a model explicitly permits
  chunking; add preflight/resource diagnostics.
- **Dependencies / regression risks:** F09 seam/boundary fixtures and memory
  measurement. Arbitrary generic-model chunking can invalidate context and
  output timing; reducing inference window size alone does not bound decode.
- **Acceptance:** Removing the redundant resample leaves note outputs
  unchanged. For a later bounded Basic Pitch mode, peak working memory
  excluding returned notes meets a documented duration-independent budget
  and seam event times match the reference within one model frame.

### F11 - Confidence values have incompatible, uncalibrated meanings

- **Severity / impact:** Medium API/UX limitation; already acknowledged in
  the README, but thresholds/percentages remain easy to misinterpret.
- **Source / evidence:** [YIN confidence](../src/transcriber.cpp#L138) is
  `1 - best_value`; [spectral confidence](../src/transcriber.cpp#L452)
  is relative salience (strongest candidate can be 1.0); ONNX uses model
  activations. [CLI JSON](../src/main.cpp#L502) and
  [MIDI velocity](../src/musicxml.cpp#L507) consume the same `confidence`
  concept, although it is not a cross-model probability.
- **Proposed solution:** Document score semantics and source; retain legacy
  fields while adding optional provenance/calibration metadata. Evaluate
  per-backend threshold curves before any calibration claim.
- **Dependencies / regression risks:** F09/F23 for labeled evaluation;
  consumers may rely on current thresholds or velocity mapping.
- **Acceptance:** Output/UI label heuristic scores correctly; each backend
  has precision/recall-versus-threshold evidence. No percentage is described
  as probability without a held-out calibration result.

### F12 - Rhythm inference and quantization lack a common musical-time model

- **Severity / impact:** Medium documented feature limitation; not a demand
  for a replacement score architecture.
- **Source / evidence:** [infer_score_metadata](../src/transcriber.cpp#L1412)
  infers global tempo from intervals, meter from interval/phrase grouping,
  swing from ratios and key from pitch profiles.
  [ticks_for_time](../src/musicxml.cpp#L77) snaps independent absolute
  starts/durations to nearby grids, then assigns greedy overlap voices.
  No beat/downbeat map, tempo changes or persistent voice identity exists.
- **Proposed solution:** First characterize pickup, syncopation, swing,
  tuplets and crossing/overlapping voices with fixtures. Add explicit
  musical-time annotations locally where needed, preserving seconds-based
  APIs and the current default unless an improvement is validated.
- **Dependencies / regression risks:** F13 must first ensure internally
  consistent notation. New snapping/voice choices change visible scores
  and must not silently alter raw MIDI/JSON performance times.
- **Acceptance:** Each supported rhythmic fixture has expected measures,
  per-voice duration sums, ties and bounded quantization displacement.
  Unsupported meter/tempo structures remain clearly labeled, not guessed
  with a new unsupported confidence claim.

### F13 - MusicXML one-tick tokens disagree with their written note type

- **Severity / impact:** Medium; reproduced semantic export defect.
- **Source / evidence:** [rhythm_values](../src/musicxml.cpp#L40) assigns one
  tick to `256th` with divisions 48. A 0.01 s note at 120 BPM exported
  `<duration>1</duration><type>256th</type>` without time modification.
  `1/48` of a quarter is not a 256th note (`1/64` of a quarter).
  Arbitrary residual ticks can also appear in rests/decomposed durations.
- **Proposed solution:** Use a representable timing lattice or exact
  type/dot/time-modification arithmetic for every token, then test semantic
  duration equality rather than only XML parsing.
- **Dependencies / regression risks:** Preserve measure totals and ties;
  any grid adjustment must bound/export its timing displacement.
- **Acceptance:** For every note/rest, notated type, dots and time
  modification equal `duration/divisions` exactly; all voices sum to the
  measure duration in adversarial off-grid and cross-bar fixtures.

### F14 - Public score/API invariants are weaker than CLI validation

- **Severity / impact:** Medium API robustness risk; source-based, not
  invalid-floating-point crash-tested.
- **Source / evidence:** [public score/export APIs](../include/transcribe/musicxml.hpp)
  accept arbitrary notes/metadata. [write_musicxml](../src/musicxml.cpp#L355)
  clamps tempo but does not reject NaN and multiplies unrestricted positive
  meter numerators; [write_midi](../src/musicxml.cpp#L467) rounds times/
  confidence and clamps pitches/ticks. Public analysis thresholds also
  have inconsistent finite/range checks. CLI checks do not protect library
  users or all edited inputs.
- **Proposed solution:** Define small shared validators for finite
  timestamps/durations/confidence/tempo, MIDI ranges and bounded meters.
  Reject invalid values explicitly; document any deliberate clamping.
- **Dependencies / regression risks:** Preserve public overloads and valid
  outputs. Invalid inputs previously normalized may now throw; specify
  behavior rather than hiding it in a new silent fallback.
- **Acceptance:** Table-driven library tests cover NaN/infinities, negative
  times/durations, out-of-range pitches and overflow-size meters. Invalid
  input fails before creating/truncating destination files.

### F15 - Tab alternatives omit constraints; fallback can discard explicit edits

- **Severity / impact:** Medium; reproduced fingering/export defects.
- **Source / evidence:** [primary assignment](../src/guitar_tab.cpp#L274)
  passes held strings and fixed positions to `assign_group`;
  [alternative generation](../src/guitar_tab.cpp#L329) does not.
  With E4 held on string 2/fret 5, the next G4's first alternative used
  string 2/fret 8 during that hold. In a simultaneous B3/E4/unplayable
  MIDI-20 group, fallback assigned B3 to string 2 and dropped an explicit
  E4 override on string 2/fret 5, returning unassigned without an error.
- **Proposed solution:** Reuse one constraint-aware solution set for
  primary/alternatives; reserve explicit positions before partial fallback
  and report unplayable/dropped notes distinctly.
- **Dependencies / regression risks:** Preserve valid existing fingerings;
  heuristic rankings may change. Partial playability is useful, but must
  not silently overrule a validated explicit edit.
- **Acceptance:** Every alternative obeys held-string/override constraints;
  valid explicit positions survive unrelated unplayable notes or cause a
  precise error. Property tests verify pitch/tuning/capo/string consistency.

### F16 - Saved unavailable model settings can disable usable GUI workflows

- **Severity / impact:** Medium; confirmed conditional capability bug.
- **Source / evidence:** [native preferences/validation](../src/gui.cpp#L231)
  restores a model path, disables its controls without ONNX, then rejects
  the nonempty path. [WebView preferences](../ui/app.js#L68) similarly
  restore disabled model/polyphony values, which
  [native message validation](../src/gui_webview.cpp#L731) rejects.
- **Proposed solution:** Make submitted effective settings capability-aware;
  ignore/clear unavailable active choices while preserving preferences for
  a later capable build.
- **Dependencies / regression risks:** Test migration both directions;
  do not permanently erase useful model settings merely by opening the
  lightweight build.
- **Acceptance:** Both GUIs with saved ONNX settings can transcribe a WAV
  in a non-ONNX build without editing preference files; ONNX builds retain
  their compatible preferences.

### F17 - Edited export uses stale tuning/capo and other output options

- **Severity / impact:** Medium; confirmed request-state mismatch.
- **Source / evidence:** [editor export payload](../ui/editor.js#L842)
  sends notes/positions/metadata/paths, not current tuning, capo,
  alternative count or expressive timing. Controls remain active and
  tuning/capo can change positions. [export-edits](../src/gui_webview.cpp#L916)
  copies `last_request` and writers use those old options at lines 983-995.
- **Proposed solution:** Snapshot and validate current export settings in
  the message, or explicitly freeze unsupported changes. Prefer one
  authoritative request snapshot per export.
- **Dependencies / regression risks:** F14/F15 validation and UI state
  tests; stale tuning can turn a previously valid position into a mismatch.
- **Acceptance:** Change each export option after transcription, export,
  and verify current values in MusicXML/tab; unchanged-state output
  remains compatible. Reject invalid combinations before writing anything.

### F18 - Editing history and live-update fingering can become stale

- **Severity / impact:** Medium; confirmed conditional editor bugs.
- **Source / evidence:** [live render updates](../ui/editor.js#L692)
  recalculate positions only when note count changes. Same-count changed
  pitches/order can retain old positions. Alternative-pitch edits at
  lines 450-455 and keyboard edits at 932-945 push undo snapshots but do
  not clear redo history, unlike other edit paths.
- **Proposed solution:** Recompute affected positions for incoming
  transcription batches; centralize edit commits so every divergent edit
  invalidates redo. Do not overwrite deliberate post-transcription edits.
- **Dependencies / regression risks:** Separate live state from committed
  user state; F17 exports rely on coherent note/position snapshots.
- **Acceptance:** Same-count changed-pitch/order updates yield valid
  positions; Undo -> alternate/keyboard edit always disables Redo and
  cannot restore an abandoned future.

### F19 - GUI cancellation/liveness and asynchronous startup need explicit contracts

- **Severity / impact:** Medium confirmed liveness limitations; High
  potential lifetime impact for the unverified startup race.
- **Source / evidence:** [run_transcription](../src/gui_transcription.cpp#L29)
  checks stop at entry and passes it to capture, but not file decode/
  streaming/offline analysis. Close defers until completion.
  [WebView startup callbacks](../src/gui_webview.cpp#L125) retain
  `AppState&`, while `WinMain` owns state on its stack (line 1412).
  If completion posting fails, ownership is safe but `busy`/deferred close
  have no completion transition.
- **Proposed solution:** Add bounded cancellation points without changing
  successful outputs; give async startup an independently safe lifetime/
  cancellation mechanism; handle failed completion delivery explicitly.
- **Dependencies / regression risks:** Shutdown ordering, COM thread
  affinity, F05 capture cleanup. Do not infer UAF from normal posted
  worker completion: that path joins correctly.
- **Acceptance:** Automated close during startup, decode, analysis,
  capture and export terminates without late state access, indefinite busy
  state or lost error; cancellation response budget is declared/measured.

### F20 - Native polyphonic checkbox can silently run monophonic analysis

- **Severity / impact:** Medium; confirmed native/WebView validation drift.
- **Source / evidence:** [native checkbox submission](../src/gui.cpp#L371)
  permits checked "Polyphonic ONNX" without a model in an ONNX build.
  [run_transcription](../src/gui_transcription.cpp#L99) selects built-in
  analysis without propagating `AnalysisOptions.polyphonic`. WebView
  explicitly rejects polyphony without a model at lines 737-739.
- **Proposed solution:** Add common model-required validation for this
  ONNX-labeled control. Exposing built-in spectral polyphony is a separate,
  intentional UI feature, not a silent reinterpretation of the checkbox.
- **Dependencies / regression risks:** F16 and capability tests. Both
  GUIs intentionally label their current control as model-only; the README
  documents model-free polyphony for the CLI, not GUI parity.
- **Acceptance:** Both GUIs reject checked/no-model consistently or offer
  an explicitly selected built-in mode; valid model requests still reach
  ONNX with the requested polyphony.

### F21 - Custom canvas notation and exported notation are not equivalent

- **Severity / impact:** Medium documented limitation and UX assurance gap.
- **Source / evidence:** [noteX](../ui/editor.js#L113) positions in
  seconds. [drawStaff/glyph selection](../ui/editor.js#L137) draws a fixed
  staff, heuristic noteheads/flags and sharp-only spelling, without
  persistent voices, ties, tuplets or key-aware accidental carry.
  MusicXML performs separate quantization, ties and voice construction.
- **Proposed solution:** Keep the lightweight editor, label performance-
  time versus notation views, and add small rendering/semantic fixtures
  for the supported subset. Share only necessary spelling/timing metadata.
- **Dependencies / regression risks:** F12/F13 and DOM/canvas behavior tests.
  Do not replace the UI with an engraving framework in this phase.
- **Acceptance:** Supported fixtures render/edit with stable note identity
  and pitch; unsupported ties/tuplets/key spelling are disclosed. Export
  differences are testable and do not masquerade as an exact score preview.

### F22 - Multiple output files can leave partial or overwritten results

- **Severity / impact:** Medium for partial sets; High for conditional
  batch data loss. Confirmed write ordering/naming, not fault-injected here.
- **Source / evidence:** [CLI writes](../src/main.cpp#L696),
  [shared GUI writes](../src/gui_transcription.cpp#L131), and
  [edited writes](../src/gui_webview.cpp#L983) write sequentially without a
  transaction. [process_batch](../src/main.cpp#L809) uses only `stem()`
  for every output: `take.wav` and `take.flac` collide with FFmpeg enabled.
- **Proposed solution:** Preflight collisions before processing; preserve
  legacy names when unique. Stage writes to sibling temporary files and
  report exact partial completion; use a tested promotion/rollback policy.
- **Dependencies / regression risks:** F14 validators and filesystem
  semantics on Windows; a group of renames is not inherently atomic.
  Preserve existing successful-file recovery in batch mode.
- **Acceptance:** Same-stem inputs never overwrite each other silently;
  injected failure on each output leaves prior destinations unchanged or
  explicitly reports the precise partial set. No false overall success.

### F23 - Evaluation identity can misattribute a supplied model

- **Severity / impact:** Medium reproducibility defect; confirmed.
- **Source / evidence:** [evaluator model metadata](../python/evaluate_guitarset.py#L398)
  always records Spotify Basic Pitch and its pinned commit, while
  `--model` accepts another file. SHA-256 is recorded, but binary version/
  hash/source revision for the transcriber is not.
- **Proposed solution:** Restrict the named benchmark to the bundled
  digest or accept explicit model identity; record executable identity,
  effective flags, build configuration and evaluator revision.
- **Dependencies / regression risks:** Preserve/version result JSON
  schema; do not relabel an arbitrary replacement as the pinned model.
- **Acceptance:** Bundled runs retain verified attribution; different
  models fail clearly or have supplied/unknown provenance. Every new
  report identifies the binary and can reproduce its effective settings.

### F24 - FFmpeg flush errors can become success; decode allocation is repetitive

- **Severity / impact:** Medium error-propagation defect; Low performance
  opportunity. Not locally exercised with an FFmpeg-enabled build.
- **Source / evidence:** [resampler flushing](../src/ffmpeg_audio.cpp#L197)
  breaks on `converted_samples <= 0`, treating a negative FFmpeg error
  like normal completion, unlike the checked conversion in the decode loop.
  Conversion vectors are allocated per received frame and flush iteration
  (lines 153/193); the final decoded buffer remains full-input.
- **Proposed solution:** Distinguish negative error from zero drain result;
  then reuse conversion capacity after measuring allocations. Preserve
  packet/frame RAII and complete decoder/resampler flushing.
- **Dependencies / regression risks:** FFmpeg-only fixtures/error injection
  and actual library-version coverage. Do not change default WAV behavior.
- **Acceptance:** Injected flush errors yield the FFmpeg error text and
  failure; supported compressed fixtures have expected final sample count.
  Buffer reuse measurably reduces allocation count without output changes.

### F25 - Existing tests do not gate the most important accuracy/UX contracts

- **Severity / impact:** Medium assurance gap; tests passing is insufficient
  evidence of event accuracy, rendering, concurrency or release readiness.
- **Source / evidence:** [unit tests](../tests/transcribe_tests.cpp#L74)
  emphasize synthetic presence/substring checks; MIDI checks only `MThd`.
  [CLI smoke](../tests/cli_export_smoke.py#L94) parses XML/compares pitch
  sets but not complete durations/voices or MIDI events.
  [UI smoke](../tests/ui_smoke.py) checks assets/wiring and optional syntax,
  not behavior. [real-audio smoke](../tests/real_audio_smoke.py) is optional,
  merely requires notes, and has no annotation-based quality threshold.
  CMake silently omits Python integrations when Python is missing.
- **Proposed solution:** Add focused semantic, event, editor, decoder and
  concurrency regression tests before fixes; make CI's expected test
  inventory explicit. Keep labeled accuracy/performance evaluation optional
  for ordinary dependency-light development, but gate quality claims on it.
- **Dependencies / regression risks:** F01 fixes optimization comparability.
  Avoid hardware/internet requirements or fragile wall-clock limits in the
  default unit suite.
- **Acceptance:** CI checks expected test registration and skip reasons;
  deterministic fixtures cover each confirmed defect. MIDI parsing checks
  note order/times/metadata; threaded SPSC stress and sanitizer jobs pass;
  real-audio reports include event and frame metrics with provenance.

### F26 - Manual metadata overrides are labeled as inferred at confidence 1

- **Severity / impact:** Medium semantic/API ambiguity; confirmed.
- **Source / evidence:** [CLI overrides](../src/main.cpp#L674) set
  `*_inferred=true` and confidence 1.0 for user tempo/key/meter choices.
  JSON has only the boolean/confidence, so consumers cannot distinguish
  user assertions from successful heuristic inference.
- **Proposed solution:** Add optional source fields (`default`, `inferred`,
  `user`) and document legacy booleans rather than breaking existing JSON.
  Apply the same provenance policy to GUI exports.
- **Dependencies / regression risks:** F11/F14; consumers/tests may rely on
  the existing booleans and override precedence.
- **Acceptance:** User overrides, inferred values and defaults are
  distinguishable in output; old keys/values remain compatible unless a
  separately versioned schema change is explicitly approved.

### F27 - ONNX session caching does not reload changed model files

- **Severity / impact:** Medium long-lived GUI correctness/resource risk;
  confirmed cache behavior, not a demonstrated current-user failure.
- **Source / evidence:** [load_onnx_session](../src/transcriber.cpp#L583)
  caches sessions by normalized path/provider/device in a static map with
  no eviction or content/mtime identity. Replacing a file at the same path
  does not replace the cached session; selecting many models retains them.
- **Proposed solution:** Define cache identity/lifetime explicitly; add
  bounded retention and a reload/invalidation path when model identity
  changes. Keep reuse for unchanged models.
- **Dependencies / regression risks:** Concurrent in-flight inference must
  retain its own session safely. File timestamps alone may not establish
  identical content; hashing large files on every inference is also costly.
- **Acceptance:** Same model reuses a session; replacing it at the same
  path reloads deterministically; retained sessions meet a documented limit
  without invalidating any active inference.

### F28 - Duplicated policies and unmeasured DSP costs invite drift

- **Severity / impact:** Low maintainability/performance gap; no broad
  rewrite or unmeasured speedup is justified.
- **Source / evidence:** [CLI processing](../src/main.cpp#L543) and
  [GUI processing](../src/gui_transcription.cpp#L29) separately construct
  analysis/model options, override metadata and sequence outputs.
  Fingering is recomputed for notation/tab/alternatives (F15).
  [FFT](../src/transcriber.cpp#L198) recomputes stage roots; uncached
  [spectrogram calls](../src/transcriber.cpp#L291) and YIN calls launch
  per-call workers. Cache keys scan samples; FFT/YIN workspaces are already
  reused per worker, and the spectrogram cache is already bounded at 64 MiB.
- **Proposed solution:** Extract only small demonstrated shared policies/
  validators; profile allocation, hashing, FFT and worker overhead before
  changing scheduling. Do not add an unneeded framework, thread pool or
  third-party FFT dependency.
- **Dependencies / regression risks:** F25 baselines; callback order and
  progress semantics differ between front ends. Caching twiddles or changing
  parallelism can alter memory, rounding and interactive latency.
- **Acceptance:** Each extraction preserves CLI outputs/errors; any
  optimization reports representative Release timings, peak memory and
  output equality, with a predeclared improvement threshold.

### F29 - Public buffer and callback contracts are undocumented

- **Severity / impact:** Medium integration/concurrency risk, not evidence
  that current internal SPSC use is wrong.
- **Source / evidence:** [audio.hpp](../include/transcribe/audio.hpp) does
  not describe SPSC ownership or the lifetime of `AudioBlockView.samples`;
  [transcriber.hpp](../include/transcribe/transcriber.hpp) and
  [capture callbacks](../include/transcribe/asio_capture.hpp) do not state
  callback threads, borrowing, exception/cancellation behavior or safe
  concurrent usage. The block span points at a per-callback decoded vector.
- **Proposed solution:** Document ownership/lifetime/threading and supported
  parameter ranges beside existing APIs; use small validators/tests rather
  than changing the public representation.
- **Dependencies / regression risks:** F05/F14 must make promised error
  behavior true. Do not promise concurrent destruction, MPMC access or
  callbacks retaining borrowed spans.
- **Acceptance:** Every public span/callback has an explicit lifetime/
  thread contract; SPSC stress preserves exact sequence across wrap/full/
  empty transitions, and examples copy data when retaining it.

## 6. Documentation alignment and preserved strengths

| Documentation statement | Implementation alignment / action |
|---|---|
| Default build is lightweight; optional integrations explicit | Accurate for WAV/YIN; preserve it. |
| Release examples use `--config Release` | Generator-dependent; fix F01. |
| ONNX consumes full input; Basic Pitch uses overlapping windows | Accurate but easy to conflate. Windowed `Run` is not bounded end-to-end memory; explain F10. |
| Two-second GUI and 15-second CLI WAV chunks preserve final offline behavior | They preserve the analysis/export pipeline, not proven event-for-event equivalence to full-buffer or each other. Seam merge uses 50 ms matching and can discard unmatched overlap candidates; add boundary tests before asserting equivalence. |
| Built-in spectral polyphony works without ONNX | Accurate for CLI. Both GUI toggles are explicitly model-only; do not imply GUI mode parity. |
| Pitch alternatives/confidences and metadata are heuristic | Accurately disclosed. Retain warnings; add explicit source semantics (F11/F26). |
| MusicXML supports basic tuplets/voices; renderer is not engraving | Accurately qualified; F13 is a genuine arithmetic defect, while F12/F21 are broader limits. |
| Capture ring overflow is explicit | Application-ring overflow is explicit; device-reported overflow is not yet propagated (F06). |
| CI creates ZIP/TGZ packages | True as workflow configuration, not proof of a self-contained full-featured distribution (F02). |
| Do not commit model weights | A bundled licensed model already exists. Clarify the intentional reviewed exception rather than pretending weights are absent. |

Important correct behavior to retain:

- WAV reads bound block allocations, validate truncation/alignment, support
  stated PCM widths/float32, downmix channels, and sanitize nonfinite float
  samples. Extensible WAV is explicitly rejected, not silently decoded.
- FFmpeg packet/frame/codec/resampler resources use RAII and detailed errors
  on most paths; decoder and resampler are flushed.
- Real-time callback work avoids FFT/YIN/GUI operations and dynamic sample
  allocation. Recording/analysis consumers are separated.
- Realtime methods are synchronized; normal capture transfers analysis
  exceptions after joining. Normal GUI worker-close paths join safely.
- ONNX output shape/type checks, Basic Pitch output-head validation and
  unavailable-provider errors are explicit. No unguarded output tensor
  indexing defect was established by this audit.
- Spectrogram workspaces are reused and the cache has a 64 MiB retention cap.
  Session reuse already exists; proposing a session cache from scratch would
  duplicate existing functionality.
- MusicXML and primary ASCII tab share selected fingering; existing overloads
  preserve simple callers. MIDI sorts same-tick note-offs before note-ons
  and guards four-byte variable-length delta limits.
- Batch mode continues after individual failures and returns failure for a
  partial batch. Preserve successful per-file results while fixing collisions.
- The README already discloses low event F1, heuristic metadata/confidence,
  approximate playback, rendering limits, full-buffer ONNX and possible
  training overlap. Do not report these disclosures as concealment.

## 7. Remaining verification limits

Prioritize concrete defects over speculative rewrites. GUI startup lifetime,
device error/overflow paths, stream exceptions, generic-model resource
limits, actual FFmpeg failure injection, concurrency sanitizers and
clean-machine packages still need targeted validation. No GPU capability,
hardware latency guarantee, calibration accuracy, independent real-world
AMT quality, export-schema conformance, or visual correctness is certified
by the tests run for this document.
