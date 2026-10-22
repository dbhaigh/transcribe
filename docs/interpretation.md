# Musical interpretation

The musical interpretation layer converts raw note events into one or more
score hypotheses. It is separate from pitch detection and keeps detector
timestamps intact. The C++ API is declared in
[`include/transcribe/interpretation.hpp`](../include/transcribe/interpretation.hpp);
the implementation is in [`src/interpretation.cpp`](../src/interpretation.cpp).

## Timing and evidence

`ScoreInterpretation::source_events` contains immutable copies of the supplied
`CanonicalNoteEvent` values. `onset_seconds`, `offset_seconds`, sample
positions, detector evidence and confidence are not overwritten by musical
interpretation. `InterpretedNote` stores score onset and duration in quarter
notes, together with onset/offset residuals in seconds and the source-event
index. These score coordinates are hypotheses, not replacements for measured
audio time.

Tempo maps, beats, meter, key, local tonal regions, rhythm spellings, rests,
voices, chord groups, pitch spellings and written transposition are recorded
on each ranked `ScoreHypothesis`. When evidence is weak, hypotheses retain
ambiguity flags and alternatives instead of presenting heuristic confidence as
a probability of correctness. Tempo, meter, key, pickup, instrument name and
written transposition can be constrained through `InterpretationOptions`.

### Quantization policies

The default `QuantizationMode::Legacy` retains the existing independent onset
snapper and duration matcher. The new policies are opt-in; no detector threshold
or note-event filtering changes.

| `InterpretationOptions` field | Default | Meaning |
| --- | --- | --- |
| `quantization_mode` | `Legacy` | `Legacy`, `EvidenceAware` or `Expressive` |
| `quantization_strength` | `1.0` | blend measured score coordinates toward the proposed grid, in [0,1] |
| `minimum_rhythm_evidence` | `0.65` | heuristic support gate for `EvidenceAware`, in [0,1] |
| `onset_quantization_tolerance_seconds` | `0.04` | maximum onset correction; converted using the local tempo |
| `rhythm_quantization_tolerance` | `0.10` | relative duration fit tolerance, with a 0.03-quarter floor |
| `chord_onset_tolerance_seconds` | `0.04` | maximum measured attack separation for chord candidates |
| `tempo_change_threshold` | `0.15` | minimum relative local tempo change |
| `detect_tempo_changes` / `infer_local_keys` | `true` / `true` | enable unconstrained structural hypotheses |

`EvidenceAware` ranks passage grids of 1, 1/2, 1/4, 1/3, 1/5, 1/7 and 1/8
quarter notes. Candidate support uses onset alignment (65%) and measured duration
fit (35%), with penalties for fine grids and tuplets; chord members sharing an
attack within 1 ms do not each add onset evidence. Support is discounted by
`attacks / (attacks + 2)` so a sparse passage cannot establish a confident grid.
Per-note evidence combines that passage support with onset fit (60%) and duration
fit (40%). A note is quantized only if its onset is within tolerance, its duration
has a compatible symbolic spelling, and evidence meets the configured gate.
Otherwise **both boundaries remain measured**, the rhythm is nonsymbolic,
alternatives remain available, and an uncertainty is recorded. Detector
confidence/loudness is not used to suppress quiet notes.

Grid fit is `clamp(1 - 0.5 * absolute_error / tolerance, 0, 1)` (exact matches
only for zero tolerance). Candidate complexity subtracts
`0.035 * log2(1 / grid_step)` and another `0.04` for tuplet grids. These are
explicit heuristics, not learned/calibrated probabilities.
`InterpretedNote::confidence` becomes `ScoreNote::interpretation_evidence`;
it is never copied into `calibrated_confidence`.

`Expressive`, or strength zero in any mode, bypasses onset/duration quantization
and swing straightening. Strengths between zero and one interpolate onset and
duration independently, which also interpolates the endpoint. A partial-strength
duration is not labelled symbolic unless it really matches a symbolic value.
The detector events remain immutable in every mode. These controls are stored
as WebView preferences; canonical projects retain resulting positions, residuals,
evidence and uncertainty using the existing schema, not the full inference-options
recipe. Keep the CLI config alongside a project to reproduce that recipe.

Rhythm candidates include dotted/double-dotted values and 3:2, 5:4 and 7:4
tuplets. Swing remains a separate hypothesis, not proof of tupleting.

### Tempo, voices and spelling

Tempo still ranks beat candidates and retains alternatives. Local changes use
four-interval blocks, regularity/confidence and the configured change threshold.
In evidence-aware mode, half/double subdivision changes are normalized against the
beat candidate rather than automatically creating tempo doubling; legacy mode
retains its original segmentation. Fixed user tempo disables automatic
tempo changes. Score/beat-to-seconds conversion extrapolates from the final
segment's **start**, including trailing bar-filling rests.

Equal-release simultaneous notes share a voice/chord. Different releases use
separate voices so a moving melody can continue above a sustained bass. Overlaps
use available voices deterministically; this is interval separation, not a
learned contrapuntal/instrument identity model. Expressive/partial-strength and
uncertain evidence-aware notes with different score onsets do not become
export-time simultaneous chords.

Pitch spelling minimizes deviations from the selected key, including E-sharp,
B-sharp, C-flat and F-flat with the correct octave. Explicit key overrides
disable local-key replacement; disabled local inference produces no tonal
regions. MusicXML computes accidental state per letter/octave and measure,
chronologically across voices, including naturals, repeated accidentals and
bar resets. Tie continuations do not acquire a new attack accidental. A score
edit that clears a spelling is respelled using the active key at export.
`PitchSpelling::accidental` describes deviation from the signature, not the
final measure-dependent display decision.

Tempo, meter and key retain `user_constrained` independently of heuristic support.
An override with support 1 means "requested", not "detected with certainty";
automatically interpreted notes remain inference-origin notes.

## MusicXML and CLI

The interpretation-aware `write_musicxml` overload exports a selected
hypothesis using its score positions rather than the legacy sixteenth-note
snapper. It writes variable-duration measures, implicit pickup measures,
voices, chord members, rests, barline ties, key spelling, tempo-map directions,
available velocity/articulation evidence and optional written transposition.
Tempo directions are omitted while tempo remains ambiguous, unless it was
explicitly constrained.
MusicXML divisions remain 960 per quarter note, increasing to 6,720 only when
selected septuplets need a factor of seven. Off-grid timing is retained to that
resolution. Note types/dots/tuplet ratios must agree with emitted durations to
within half a tick; unquantized notes do not acquire speculative types.
Tuplet timing is encoded with `<time-modification>`. The
exporter omits an uncertain key or time signature and writes an explicit
uncertainty direction, while still using the selected meter internally to
determine measure boundaries.
Inferred rests exactly fill uncovered voice intervals rather than rounding their
lengths independently. Bar-crossing notes are split into playback and notation
ties. A tempo change inside a held note/rest uses a sounding direction offset
at its actual score position, not the next note boundary. The historical
`write_musicxml(vector<Note>, ...)` overload keeps its legacy export path;
the correctness fixes here apply to interpretation/canonical-score exports.

The CLI accepts:

```text
--tempo <40..240>
--meter <2/4|3/4|4/4|5/4|2/8|3/8|5/8|6/8|7/8|9/8|12/8>
--key-fifths <-7..7> --mode <major|minor>
--pickup <quarter-note-length>
--written-transposition <-48..48>
--quantization <legacy|evidence|expressive>
--quantization-strength <0..1>
--minimum-rhythm-evidence <0..1>
```

`--preserve-expressive-timing` adds an onset-residual annotation to the
MusicXML; it does not alter note positions or audio-time JSON note fields. MIDI
and JSON metadata use the selected top-ranked interpretation. The CLI currently
exports the highest-ranked hypothesis; applications using the API can select
another hypothesis by index.
Use `--quantization expressive` to actually preserve measured timing instead of
merely annotating residuals. Canonical-score MIDI follows interpreted score
coordinates; audio-note JSON timestamps remain measured. The new options also
accept same-named config-file keys. WebView advanced transcription settings
expose all three, validate them and lock them during a job; the native minimal
GUI retains the shared backend's legacy defaults.

```powershell
.\build\transcriber.exe take.wav --tempo 96 --meter 6/8 `
  --quantization evidence --quantization-strength 1 --minimum-rhythm-evidence 0.65
.\build\transcriber.exe rubato.wav --tempo 96 --meter 4/4 --quantization expressive
```

Default output contracts, raw events and project schema remain unchanged.
Intentional notation corrections apply even with legacy quantization: exact gap
lengths, independent voices for unequal releases, key-respecting spelling and
accidental cancellation, honest duration types and correctly placed tempo changes.

## Annotated-reference validation

The test harness extends the repository's Catch2/CTest and Python-standard-library
test setup. It does not replace the existing audio quality evaluation. References
are original, hand-authored evaluation-only passages (MIT project license) in
[`tests/fixtures/interpretation/passages.json`](../tests/fixtures/interpretation/passages.json),
plus exact rational tuplet and tempo cases in
[`tests/interpretation_semantics.py`](../tests/interpretation_semantics.py).
No third-party composition or dataset is downloaded.

The 18 passages cover dotted notes, syncopation, uncovered rests, tied barlines,
sustained bass/moving melody, equal-release chords, accidentals, extreme
enharmonics, pickups, expressive timing, ambiguous rolled attacks, partial strength,
3:2/5:4/7:4 tuplets, inferred tempo changes and mid-note tempo directions.
They compare both parts of both direct and serialized canonical-round-trip
exports. The parser reconstructs absolute onset/duration with `<backup>` and
chord semantics, checks per-voice bar coverage, reconnects ties, checks rests do
not overlap notes, and verifies symbolic duration ratios, key, meter, tempo
positions, spelling and accidentals. Measured source times and override flags
are checked separately. Valid XML alone is not a pass.

```powershell
cmake -S . -B build-notation -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-notation -j 4
ctest --test-dir build-notation --output-on-failure `
  -R "interpretation|rhythm|score|project|review|MusicXML|cli_export|ui_asset|notation_fixture"
```

Use the repository's documented compiler environment/generator for your platform.
CTest writes `evaluation/notation-semantics.json` and
`evaluation/notation-semantics.md` within the build directory, with per-passage
onset/duration errors, voice mismatches and runner/fixture/test hashes.

Measured comparison on Windows, Clang 23.1.1, Release: all 18 selected-mode
passages matched reference pitch, onset, duration and voice (XML rounding within
one tick). Selected-mode onset/duration MAE in the score representation was zero
to the report's six decimal places. Illustrative **separate** legacy-control
errors, in quarter notes:

| Passage | Legacy onset MAE | Selected onset MAE | Legacy duration MAE | Selected duration MAE | Legacy / selected voice mismatches |
| --- | ---: | ---: | ---: | ---: | ---: |
| Triplet passage | .004630 | 0 | 0 | 0 | 1 / 0 |
| Quintuplet passage | .045000 | 0 | 0 | 0 | 3 / 0 |
| Septuplet passage | .038690 | 0 | 0 | 0 | 4 / 0 |
| Expressive off-grid | .040833 | 0 | .033333 | 0 | 0 / 0 |
| Ambiguous sparse note | .026000 | 0 | .048000 | 0 | 0 / 0 |
| Ambiguous rolled chord | .040000 | 0 | 0 | 0 | 1 / 0 |

The control is **legacy mode in the same build**, not a preserved pre-change
binary. These tiny annotated examples validate semantics and expose failure
modes; they do not establish real-world score-reading accuracy or a stable
aggregate quality baseline. No production detection thresholds were tuned.

Final validation: the command above passed all 45 selected CTests in both
model-free and ONNX/ASIO builds. The WebView/ONNX/ASIO GUI built successfully.
Browser mock-host tests verified control values reach the native request,
controls lock while busy, invalid strength is rejected before submission and
no page errors occur. UI source smoke passed; Node's standalone syntax check
was unavailable, and existing aggregate-initializer/compiler warnings remain.
No ASIO hardware or acoustic dataset is involved in these notation tests.

## Current limits

Meter and key rankings are heuristic; validation above is not an annotated
real-recording/large-score corpus study. The evidence grid is passage-wide:
mixed subdivisions or free rubato can correctly remain nonsymbolic but are not
automatically segmented into optimal notation regions. Beat phase is anchored
to recording-relative time, with explicit pickup support, not a general
downbeat/metrical-phase estimator. Rests and notes that need compound within-bar
decomposition may remain nonsymbolic rather than receiving invented tuplets.

Tempo segmentation is coarse and cannot reliably distinguish a genuine 2x tempo
change from a subdivision change; accelerando/rubato and very short changes need
review. Fixed tempo overrides cannot specify a changing map from the CLI
(the score representation/API can retain one). Key changes remain heuristic.
Written transposition is an explicit semitone setting, not an inferred instrument
property; callers must provide the intended written key when constraining it.

MusicXML does not yet serialize all alternative hypotheses or full tuplet bracket
grouping/nested tuplets, instrument-catalogue transpositions or swing glyph
conventions. Simultaneous chromatic collisions may need courtesy accidentals;
voice IDs do not assert an instrument/staff or stable contrapuntal identity.
Canonical user edits remain supported through the existing score export, but
arbitrary manual same-voice overlaps still require correction. Articulation and
velocity are emitted only when present on the source event; no score dynamics
are inferred from raw amplitude.

Deterministic regression fixtures for score constraints, ambiguity, rhythm,
swing, tempo changes, voices and MusicXML output are in
[`tests/interpretation_tests.cpp`](../tests/interpretation_tests.cpp).
