# Incremental engineering roadmap

Baseline: Transcriber 0.1.4, commit
`0561397b3705aeb8763726ea4b73fd83b4012aac`.
This is a proposed implementation sequence, **not implemented work**.
Evidence, severity and limitations are in [the engineering audit](engineering-audit.md).
Finding IDs below link to that report's sections.

## Guardrails

1. Preserve the dependency-light C++20 WAV/YIN default and native Win32
   fallback. Keep ONNX, FFmpeg, ASIO, WebView2 and GPU providers opt-in.
2. Keep public overloads, CLI flags, default output names when unambiguous,
   valid config precedence, and existing JSON keys. Add optional provenance
   fields rather than silently changing old meanings.
3. Fix confirmed failures before tuning accuracy or changing scheduling.
   Add a focused regression fixture with each behavioral fix.
4. No architecture replacement, mandatory UI/FFT framework, hidden model
   download or arbitrary generic-ONNX chunking.
5. Make failures and intentional behavior changes explicit. Do not claim
   frame F1 is event accuracy, a heuristic score is calibrated probability,
   or input-time latency is measured device-to-screen latency.
6. Keep external recordings out of the repository unless their licensing,
   attribution and redistribution are explicitly reviewed.
7. Every change should be a reviewable, independently testable patch.
   Prerequisites are explicit below; unrelated rows can proceed independently.

## Acceptance protocol for every patch

- Record compiler, generator, configuration and enabled features.
- Run the smallest relevant Catch2/CTest/Python/JS selector first; run
  affected CLI/config compatibility and default-build checks before merging.
- A test not discovered, optional test skipped, or feature not built is
  recorded as such, never counted as a pass.
- Preserve valid-output semantics unless the patch explicitly fixes an
  identified bug. Save before/after metrics for transcription changes.
- Test both the enabled path and dependency-disabled path for optional work.
- Use deterministic semantic assertions, not only substrings/file headers.
- Keep timing gates out of ordinary unit tests when hardware noise makes
  them unstable; run controlled Release benchmarks separately.

## 1. Stabilize input, exports, build identity and release payloads

| ID / priority | Small change and findings | Actual prerequisites | Tests and measurable exit criterion |
|---|---|---|---|
| R01 / P1 | Correct single-config Release configure commands and record optimization/build identity. [F01](engineering-audit.md#f01---release-selection-differs-by-generator) | None | Makefile Release cache/compile flags show optimization; multi-config Release works; default options stay OFF. |
| R02 / P1 | Add strict finite/full-consumption numeric helpers for CLI/config and capture entry. [F04](engineering-audit.md#f04---numeric-parsing-accepts-trailing-junk-and-nan-capture-duration) | None | Reject NaN/infinity/trailing junk/fractional integers; direct NaN capture fails before device initialization. Existing valid config/CLI precedence passes. |
| R03 / P1 | Define shared note/metadata validation and fail before destination truncation. [F14](engineering-audit.md#f14---public-scoreapi-invariants-are-weaker-than-cli-validation) | None | Table-driven invalid finite/range/overflow cases fail explicitly; current valid overloads/exports remain compatible. |
| R04 / P1 | Repair MusicXML token duration/type arithmetic, with exact rational semantic checks. [F13](engineering-audit.md#f13---musicxml-one-tick-tokens-disagree-with-their-written-note-type) | None | Off-grid one-tick fixture no longer has inconsistent notation; every token's type/dots/tuplets equals duration/divisions; voices total correctly across bars. |
| R05 / P1 | Reject same-stem batch collisions before any writes. [F22](engineering-audit.md#f22---multiple-output-files-can-leave-partial-or-overwritten-results) | None | FFmpeg batch containing `take.wav`/`take.flac` never silently overwrites. Unique names retain current paths; partial-batch recovery still works. |
| R06 / P1 | Decide project licensing and produce dependency/model provenance inventory. [F03](engineering-audit.md#f03---distribution-licensing-and-reproducible-provenance-are-incomplete) | Owner approval for project license | Every distributed binary/model has identified version/origin/license/digest; no license is invented. Document bundled-model exception and artifact/export origin. |
| R07 / P1 | Stage/install matching Windows runtime dependencies and notices for supported builds. [F02](engineering-audit.md#f02---installed-full-featured-windows-payload-omits-required-dlls) | R06 | Inspect imports and test clean environment: default native and CPU-ONNX/WebView2 CLI+GUI launch; CLI transcribes with no developer PATH. No arbitrary system-DLL copying. |
| R08 / P2 | Add installed-package smoke CI, separate from build-tree tests. [F02](engineering-audit.md#f02---installed-full-featured-windows-payload-omits-required-dlls), [F25](engineering-audit.md#f25---existing-tests-do-not-gate-the-most-important-accuracyux-contracts) | R07 | Unpack artifact outside checkout, run help and sample export with clean dependency paths, verify assets/model/notices. MSVC and supported MinGW packaging policies are explicit. |

R04 should fix notation arithmetic without simultaneously adding tempo maps
or rewriting voice assignment. R07 should initially support the CPU runtime
actually shipped; provider distributions can be added independently later.

## 2. Make capture ownership and error paths testable

| ID / priority | Small change and findings | Actual prerequisites | Tests and measurable exit criterion |
|---|---|---|---|
| R09 / P1 | Document SPSC/span/callback contracts and add deterministic threaded ring stress. [F29](engineering-audit.md#f29---public-buffer-and-callback-contracts-are-undocumented) | None | Exact sequence preserved across many wrap/full/empty transitions with one producer/consumer; callback borrowing/thread rules are explicit; no MPMC promise. |
| R10 / P1 | Introduce the smallest capture stream test seam; check supported RtAudio operation results and device overflow. [F06](engineering-audit.md#f06---rtaudio-errors-and-device-overflow-status-are-not-propagated) | None | Fake open/start/stop errors and device overflow produce precise failure/incomplete-recording status. No logging/allocation in real-time callback. Compile supported RtAudio version explicitly. |
| R11 / P1 | Add exception-safe stream teardown before callback-state destruction. [F05](engineering-audit.md#f05---asio-exception-teardown-can-outlive-callback-state) | R10 test seam | Inject progress/audio/live callback failures and preview-allocation failure; stop/close precedes state teardown on all paths; final events and original errors remain correct. |
| R12 / P2 | Validate capture on hardware and separate input-time from wall-clock latency measurements. [F06](engineering-audit.md#f06---rtaudio-errors-and-device-overflow-status-are-not-propagated), [F25](engineering-audit.md#f25---existing-tests-do-not-gate-the-most-important-accuracyux-contracts) | R10, R11 | Document driver/device/rate/buffer, underruns/overruns and observed callback-to-delivery timing. Preserve the deterministic <=50 ms synthetic input-time test without presenting it as an end-to-end guarantee. |

Hardware work is an optional qualification lane, not a new dependency of the
default unit suite. R11 must preserve safe worker joins during stop, normal
finish, errors and GUI close.

## 3. Fix bounded GUI/editor defects before adding capabilities

| ID / priority | Small change and findings | Actual prerequisites | Tests and measurable exit criterion |
|---|---|---|---|
| R13 / P1 | Make effective GUI preferences capability-aware and share model-required request validation. [F16](engineering-audit.md#f16---saved-unavailable-model-settings-can-disable-usable-gui-workflows), [F20](engineering-audit.md#f20---native-polyphonic-checkbox-can-silently-run-monophonic-analysis) | None | Both GUIs transcribe without ONNX despite saved model preferences; checked/no-model ONNX polyphony is handled consistently. Capable-build preferences survive downgrade/upgrade. |
| R14 / P1 | Fix redo invalidation for alternative/keyboard edits. [F18](engineering-audit.md#f18---editing-history-and-live-update-fingering-can-become-stale) | None | Undo -> each new edit disables Redo; redo cannot resurrect the abandoned branch. Runtime JS test, not just syntax/wiring. |
| R15 / P1 | Refresh positions on changed same-count transcription batches. [F18](engineering-audit.md#f18---editing-history-and-live-update-fingering-can-become-stale) | None | Changed pitch/order with unchanged count yields pitch-valid positions; deliberate completed edits are not reset by unrelated rendering. |
| R16 / P1 | Use one constraint-aware fingering result for primary/alternatives and preserve explicit overrides during partial fallback. [F15](engineering-audit.md#f15---tab-alternatives-omit-constraints-fallback-can-discard-explicit-edits) | None | Reproduced held-string alternative and overridden-note fixtures pass; property checks cover capo/tuning/overlap/unplayable notes; meaningful unplayable status is explicit. |
| R17 / P1 | Send current export settings and validate one authoritative edited-export snapshot. [F17](engineering-audit.md#f17---edited-export-uses-stale-tuningcapo-and-other-output-options) | R03, R16 | Change tuning/capo/alternatives/expressive timing after transcription; exported artifacts use current settings and valid positions; unchanged settings remain compatible. |
| R18 / P1 | Own/cancel asynchronous WebView startup safely; test early-close and failed completion delivery. [F19](engineering-audit.md#f19---gui-cancellationliveness-and-asynchronous-startup-need-explicit-contracts) | None | Close repeatedly during environment/controller creation; no late state access, stuck busy flag or lost cleanup. Test with controlled callback ordering before claiming the suspected race fixed. |
| R19 / P2 | Add file/offline cancellation checkpoints without changing completed results. [F19](engineering-audit.md#f19---gui-cancellationliveness-and-asynchronous-startup-need-explicit-contracts) | R11 for capture-close coverage | Close/cancel each work phase within a declared measured budget; completed output unchanged; canceled work explicitly identified and destination preservation tested. |

R14 and R15 are deliberately separate small fixes, despite sharing a finding.
R13 does not silently enable built-in spectral polyphony in an ONNX-labeled
control. An explicit GUI analysis-mode selector can follow as a separately
specified feature after correctness work.

## 4. Establish accuracy evidence, then repair DSP and adapter costs

| ID / priority | Small change and findings | Actual prerequisites | Tests and measurable exit criterion |
|---|---|---|---|
| R20 / P1 | Version evaluation identity/protocol and add evaluator unit fixtures. [F23](engineering-audit.md#f23---evaluation-identity-can-misattribute-a-supplied-model), [F09](engineering-audit.md#f09---event-accuracy-is-inadequate-its-exact-failure-decomposition-is-missing) | None | Model overrides cannot inherit false attribution; binary/evaluator revision and settings recorded. Known event-matching/repetition/10 ms occupancy cases produce exact counts. Existing stored baseline aggregates remain reproducible. |
| R21 / P1 | Add deterministic pitch-register, alias, onset/offset/repetition and window-seam fixtures before DSP changes. [F07](engineering-audit.md#f07---linear-downsampling-aliases-high-frequency-energy-into-pitches), [F08](engineering-audit.md#f08---yins-effective-lag-range-is-narrower-than-its-accepted-midi-range), [F09](engineering-audit.md#f09---event-accuracy-is-inadequate-its-exact-failure-decomposition-is-missing) | None | Tests expose current alias and register failures; seam sweep covers CLI/GUI chunk and Basic Pitch window offsets. Expected failures are recorded explicitly, not merged as an unexplained permanently red suite. |
| R22 / P1 | Add shared dependency-free antialias downsampling with explicit delay accounting. [F07](engineering-audit.md#f07---linear-downsampling-aliases-high-frequency-energy-into-pitches) | R21 | >=40 dB attenuation at declared stopband probes; <=0.1 dB 440 Hz passband gain error; <=1 cent in-band pitch error where applicable. False A4 alias removed; report CPU, delay, event/frame changes. |
| R23 / P2 | Define and test consistent supported YIN frequency/window bounds. [F08](engineering-audit.md#f08---yins-effective-lag-range-is-narrower-than-its-accepted-midi-range) | R21 | Published per-mode range agrees with register sweeps within one semitone; low-register window limitations and unsupported pitches explicit. Measure octave errors/CPU before expanding range. |
| R24 / P2 | Dispatch Basic Pitch before generic 16 kHz resampling. [F10](engineering-audit.md#f10---onnx-remains-full-recording-in-memory-basic-pitch-has-avoidable-work) | R21 Basic Pitch fixtures | Same Basic Pitch note/pitch/time/confidence output; allocation/resample instrumentation shows unused full 16 kHz buffer eliminated. Generic v1 path unchanged. |
| R25 / P2 | Define ONNX cache identity, explicit reload and bounded retention. [F27](engineering-audit.md#f27---onnx-session-caching-does-not-reload-changed-model-files) | None | Unchanged model reuses session; file replacement/reload updates inference; in-flight sessions remain alive; configured retention bound tested. |
| R26 / P2 | Add resource preflight/measurement for full-input ONNX and long-file decode. [F10](engineering-audit.md#f10---onnx-remains-full-recording-in-memory-basic-pitch-has-avoidable-work) | R24 | Record peak memory and duration scaling for 1/10/60 minute inputs; generic full-input contract remains explicit; unsupported resource requests have actionable diagnostics. |
| R27 / P2 | Decode Basic Pitch frames incrementally with bounded overlap state, as a separate opt-in transition. [F10](engineering-audit.md#f10---onnx-remains-full-recording-in-memory-basic-pitch-has-avoidable-work) | R21, R24, R26 | Note events match reference within one model frame across seams; peak inference/postprocess memory stays within declared fixed budget excluding returned notes. End-to-end memory is not called bounded until decode is also bounded. |
| R28 / P2 | Add event error decomposition and run one decoder experiment per patch. [F09](engineering-audit.md#f09---event-accuracy-is-inadequate-its-exact-failure-decomposition-is-missing) | R20, R21 | Freeze recording-separated validation/holdout lists; report exact pitch/onset/offset/repetition FP/FN. Promotion gate proposed: >=2 percentage-point strict event-F1 gain, <=1 point frame-F1 drop, and no unexplained precision loss on declared validation data; verify holdout separately. |

The R28 promotion gate is a **future decision criterion**, not a predicted
improvement or a measurement already achieved. Change it only before running
the experiment, with recorded rationale. Recompute the baseline on the exact
evaluation binary after R22/R23 before comparing decoder experiments.
Do not attribute event-F1 improvements to the model if only postprocessing
changed, and do not call GuitarSet an independent holdout without resolving
possible training overlap.

## 5. Strengthen semantics, error reporting and performance incrementally

| ID / priority | Small change and findings | Actual prerequisites | Tests and measurable exit criterion |
|---|---|---|---|
| R29 / P2 | Add backward-compatible metadata/confidence provenance. [F11](engineering-audit.md#f11---confidence-values-have-incompatible-uncalibrated-meanings), [F26](engineering-audit.md#f26---manual-metadata-overrides-are-labeled-as-inferred-at-confidence-1) | R03 | Default/inferred/user distinguishable; heuristic backend score identified; existing CLI flags and JSON fields retained. No unmeasured calibration claim. |
| R30 / P2 | Stage multi-format output sets and implement explicit promotion/partial-failure policy. [F22](engineering-audit.md#f22---multiple-output-files-can-leave-partial-or-overwritten-results) | R03 | Inject failure at each writer/rename. Existing destinations preserved where promised; precise partial-success diagnostics otherwise; no orphan temporary files or false success. |
| R31 / P2 | Correct FFmpeg flush error handling. [F24](engineering-audit.md#f24---ffmpeg-flush-errors-can-become-success-decode-allocation-is-repetitive) | None | Negative drain return propagates original error; zero ends normally; compressed fixtures verify final samples. Default no-FFmpeg build unchanged. |
| R32 / P3 | Reuse measured FFmpeg conversion buffers. [F24](engineering-audit.md#f24---ffmpeg-flush-errors-can-become-success-decode-allocation-is-repetitive) | R31 | Reduced measured allocation count, identical decoded samples/counts, no unexpected peak-memory growth. |
| R33 / P2 | Add supported-rhythm semantic fixtures, then fix one quantization/voice case at a time. [F12](engineering-audit.md#f12---rhythm-inference-and-quantization-lack-a-common-musical-time-model) | R04 | Pickup/syncopation/swing/triplet/overlap fixtures have explicit expected measures/ties/voice totals and timing bounds. Raw JSON/MIDI seconds remain unchanged. |
| R34 / P2 | Clarify canvas performance-time semantics and add rendering/edit regression fixtures. [F21](engineering-audit.md#f21---custom-canvas-notation-and-exported-notation-are-not-equivalent) | None | Chords/accidentals/selection/drag/export identities verified at supported sizes; unsupported engraving explicitly labeled. Browser screenshots support, not replace, semantic tests. |
| R35 / P2 | Make CI's test inventory explicit; add semantic MIDI and concurrency/sanitizer lanes. [F25](engineering-audit.md#f25---existing-tests-do-not-gate-the-most-important-accuracyux-contracts) | R01 | Expected registrations checked; Python/Node skips visible; MIDI events/timing parsed; Linux ASan/UBSan and supported threaded stress lane run. Hardware/data jobs remain optional and labeled. |
| R36 / P3 | Extract only duplicated validation/options/serialization policy demonstrated by fixes. [F28](engineering-audit.md#f28---duplicated-policies-and-unmeasured-dsp-costs-invite-drift) | R02, R03, R13 | Existing CLI/GUI fixtures and option behavior unchanged; no mandatory GUI dependency enters core. One extraction per patch. |
| R37 / P3 | Profile and optimize a single DSP cost center only after correctness baselines. [F28](engineering-audit.md#f28---duplicated-policies-and-unmeasured-dsp-costs-invite-drift) | R01, R21 | >=10 warm measured runs on declared fixtures; report median/p95/peak memory and output equality. Select an optimization only if it exceeds a predeclared practical gain, e.g. >=10% in the targeted stage without material total-time/memory regression. |

R35 can be split into independent test-inventory, MIDI-semantics and sanitizer
patches. The row is a work package, not permission for an oversized combined
change. R37 must account for existing workspace reuse, spectrogram caching
and ONNX session reuse rather than reimplementing them.

## Suggested first merge sequence

1. R01, R02 and R04: make Release identity, malformed input and notation
   arithmetic unambiguous.
2. R03, R05 and R16: establish library/export invariants and avoid lost or
   conflicting results.
3. R06-R08: qualify an actual distributable release, with owner-approved
   licensing and clean-machine checks.
4. R09-R11: make real-time failure/teardown paths testable and safe.
5. R13-R18: repair preference/editor/export state and startup lifetime
   independently; no UI redesign.
6. R20-R24: freeze accuracy evidence, add regression cases, repair aliasing/
   range policy and remove the unused ONNX resample.
7. Proceed to bounded inference, decoder experiments, musical-time
   refinement and measured optimizations only after those gates.

This is a priority sequence, not a claim that unrelated tasks depend on
their preceding list item. For example, editor history fixes, FFmpeg error
handling, model-cache invalidation and owner licensing decisions can proceed
independently with their own focused evidence.

## Completion criteria for this roadmap, not just green builds

- Default build remains dependency-light and CLI-compatible.
- Every confirmed defect in the audit has a focused regression test.
- Release artifacts work outside development environments and include the
  required notices/provenance.
- Device/callback errors and cancellation never appear as successful,
  complete transcriptions.
- Accuracy reports distinguish frame activity, onset matching and strict
  events, with frozen input/binary identities and honest holdout caveats.
- Memory, performance and latency claims have specified workloads, actual
  measurements and explicit limits.
- Renderer/export differences and unsupported musical structures are
  visible and documented rather than hidden behind heuristic confidence.
