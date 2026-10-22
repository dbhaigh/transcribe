# Guitar fingering suggestions

Pitch transcription and guitar fingering are separate operations. The fingering
solver receives MIDI pitches, measured onset/duration times, tuning, and capo;
it does not infer which string or fret the performer originally used. A
suggested position is one playable realization of the pitch, not evidence of
the performance's exact fingering.

`assign_fingerings` in `transcribe/fingering.hpp` returns the selected
string/fret for each note, ranked sequence alternatives, optional suggested
left-hand fingers, per-note candidate positions, ambiguity flags, correction
status, and decomposed costs. These are ergonomic suggestions, including when a
position was manually selected; selection alone does not establish performance.
The existing `assign_guitar_tab_positions` and tablature export APIs delegate
to this component. The UI and CLI provide six-string standard, drop-D, DADGAD,
open-D, and open-G presets. The instrument module also supports 1-12 independently
addressable strings, including reentrant or duplicate-pitch custom tunings.
String count is derived from tuning, not a separate potentially contradictory value.

The supported presets, listed highest string first, are:

| Tuning | Open MIDI pitches |
| --- | --- |
| Standard | 64, 59, 55, 50, 45, 40 |
| Drop-D | 64, 59, 55, 50, 45, 38 |
| DADGAD | 62, 57, 55, 50, 45, 38 |
| Open-D | 62, 57, 54, 50, 45, 38 |
| Open-G | 62, 59, 55, 50, 43, 38 |

The existing six-element C++ `tuning` array remains source compatible and must
descend strictly. A nonempty `string_tuning` vector overrides that array and
accepts 1-12 MIDI integers (0-127) in **string-number order, string 1 first**.
It need not descend. Thus a twelve-string configuration models twelve independent
strings, not automatically coupled courses or their picking technique.

Capo is 0-24 and maximum relative fret is 0-36 (default 24). Sounding MIDI =
open MIDI + capo + relative fret. Optional `physical_fret_count` additionally
requires capo + relative fret <= physical count (at most 36). No neck length
is guessed when absent. `minimum_midi_pitch`/`maximum_midi_pitch` constrain
suggestions without filtering or changing detected musical pitches.
Microtonal frets and independently capoed strings are not represented.

CLI and configuration-file keys:

```powershell
.\build\transcriber.exe guitar.wav --instrument guitar `
  --string-tuning 64,59,55,50,45,40,35 --capo 2 --fret-count 24 `
  --guitar-min-midi 35 --guitar-max-midi 88 --tab seven.txt --score-project seven.json
```

`--string-tuning` overrides a named `--tuning`; no spaces or empty entries are
accepted inside the comma-separated list. On the CLI, `--fret-count` makes all
frets up to that physical limit available, adjusting the preferred position
for very short necks. In C++, relative maximum fret and physical count are
independent limits: set both as appropriate. Score projects persist tuning,
capo and optional physical count, and validate manual assignments against them.
Old projects without physical count retain the previous behavior.
Search range/cost/budget settings are suggestion-generation settings, not
persisted performance facts. The GUI can open extended-string projects and edit
their strings; its tuning chooser still provides the five named six-string presets.

## Assignment and costs

Notes within 30 ms of the first onset in a group are treated as a simultaneous
chord by default. A bounded sequence search considers valid assignments across
groups, not just each note in isolation. A string remains occupied until its
note ends, and simultaneous notes cannot share a string. Chord positions with
fretted notes spanning more than five frets are excluded by default, including
notes still held from **earlier** onset groups; open
strings do not contribute to that span or move the hand to fret zero.
At most four fretting fingers are available by default. Equal frets may share a
barre unless an intervening open or lower-fretted string blocks it. This is a
conservative feasibility model, not a simulation of an individual guitarist.
If no complete playable assignment is
available, affected notes are explicitly returned with string `0` and fret
`-1`; the solver does not invent a position.

The sequence cost is a sum of these terms:

| Component | Default calculation | Effect |
| --- | --- | --- |
| Fret height | `0.12 × fret` | Slightly favors lower frets. |
| Preferred position | `0.75 × abs(fret - preferred_position)`; preferred position defaults to fret 5. | Favors the configured hand area. |
| Hand movement | `position_shift_cost × abs(current mean fret - previous mean fret)`; shift cost defaults to `1.4`. | Discourages large position changes between onset groups. |
| String preference | `0.1 × (string number - 1)` | A small deterministic tie-break favoring lower string numbers (not necessarily higher pitches in reentrant tuning). |
| Chord stretch | `1.5 × max(0, fret span - 4)` | Penalizes a wide chord reach before the hard span limit. |
| Repeated strings | `repeated_string_cost` per note using a string in the preceding onset group; default 0. | Optional penalty for repeated-string attacks. |
| String crossings | `string_crossing_cost × max(0, nearest previous string distance - 1)` per assigned note; default 0. | Optional penalty for skipping strings. |
| Awkward transitions | `awkward_transition_cost × abs(fret change) / max(0.05, onset delta seconds)` for strings shared with the preceding group; default 0. | Optional penalty for fast same-string shifts. |
| Unassigned notes | `unresolved_note_cost`, default `1000`, per note | Reports the unresolved cost; assignment count has priority over all costs. |

Preferred-position cost excludes open strings; hand movement uses the mean
of the newly attacked fretted positions and retains the previous hand position
for an all-open/unassigned group. Stretch includes all still-held frets.
Alternatives are ranked **first by fewer unresolved notes, then by total
sequence cost**, and include all component totals.
Even zero unresolved penalty or very high ergonomic weights cannot deliberately
buy a missing note instead of a retained complete assignment.
Alternatives have the same number of assigned notes as the selected sequence.
Per-note candidate costs report the cost of the best retained sequence
containing that position, not an independent per-note score. A note's ergonomic ranking is marked ambiguous when its two best candidate
sequence costs differ by no more than the default `0.5` margin.
Independently, multiple pitch-compatible positions carry an explicit warning
that audio pitch does not identify the performed string. These costs are solver
preferences, not probabilities or confidence that the original performer used
that position. `performed_position_unknown` remains true and
`calibrated_fingering_confidence` is absent; a clear ergonomic winner does not
resolve the recording's original position.

All weights are configurable in `FingeringOptions`: `fret_height_cost`,
`preferred_position_cost`, `position_shift_cost`, `string_preference_cost`,
`stretch_cost`, `repeated_string_cost`, `string_crossing_cost`,
`awkward_transition_cost`, and `unresolved_note_cost`. They must be finite and
nonnegative. Hard feasibility uses `maximum_chord_fret_span` and
`maximum_fretting_fingers` (1-4), rather than a cost that permits impossible
chords. Timing tolerances and ambiguity margin remain configurable.

Search budgets are `beam_width` (default 64, range 1-4096),
`local_solution_limit` (default 64, range 1-4096), and `search_node_limit`
(default 100,000, range 1-10,000,000, per incoming state/onset group).
Requests for more than 64 simultaneous events explicitly return unresolved
positions and a truncation diagnostic instead of deeply recursive search.
Memory scales with retained beam/path length and input note count, not with an
uncapped product of beam and local candidates.

`search_truncated` covers node exhaustion **and** local/beam pruning.
`visited_search_nodes` reports actual search work. `alternatives_complete`
is true only if neither pruning nor the requested alternative count removed
any best-assignment-count sequence. The API permits 0-4095 alternatives,
bounded by the retained beam; the existing CLI/UI still request at most five.
Every raw pitch-compatible string/fret is retained in per-note
`playable_positions`, independently of beam pruning. These are **not** all
guaranteed playable in the complete phrase; `candidates` describes the retained
sequence-supported choices. A bounded unresolved result is not proof that
no playable fingering exists.

Use the existing wrapper without replacing its legacy top-level settings:

```cpp
transcribe::GuitarTabOptions options;
options.tuning = {62, 57, 55, 50, 45, 38}; // DADGAD
options.capo_fret = 2;
options.maximum_fret = 22;
options.search.physical_fret_count = 24;
options.alternative_fingerings = 8;
options.search.maximum_chord_fret_span = 4;
options.search.repeated_string_cost = 0.5;
options.search.awkward_transition_cost = 0.1;
options.search.beam_width = 256;
const auto suggestions = transcribe::assign_guitar_tab_fingerings(notes, options);
```

The wrapper copies `search`, then applies its existing top-level tuning, capo,
fret limit, preferred position, shift cost, alternative count, overrides and
saved corrections. Set those on the top level, not in `search`. New ergonomic
weights/budgets are C++ API settings; no new CLI switches or GUI controls were
added for them.

Suggested finger 0 denotes an open string and 1-4 ascending fret/barre groups.
These are not persisted as performed-finger facts in the canonical score.
Fretting-finger suggestions are omitted for asynchronous overlapping fretted
attacks rather than renumbering a finger on a held note. The model does not
cover thumb-over playing, arbitrary partial barres, hand size, right-hand
technique, bends, muting or slide detection.

## Evidence-gated pitch gestures

`analyze_guitar_technique` in
[`guitar_analysis.hpp`](../include/transcribe/guitar_analysis.hpp) is an optional
instrument module, separate from pitch estimation, segmentation and fingering.
It never rewrites notes or affects piano/voice/general-purpose transcription.
Callers must explicitly enable it, select `InstrumentPrior::Guitar`, and attest
that the event has an isolated, continuous fractional-MIDI contour.
Integer note changes, chord activations or suggested same-string positions
are not sufficient evidence.

Supported observations:

| Observation | Evidence | Physical interpretation |
| --- | --- | --- |
| Vibrato-like modulation | Regular hysteretic crossings, default 3-9 Hz, bounded drift | Vibrato or external modulation; unresolved |
| Sustained pitch rise/fall | Continuous predominantly monotonic excursion, including a subsequent hold | Bend/release, slide or effect; unresolved |
| No inference | Steady pitch, abrupt jumps, insufficient/low-voicing evidence, overlapping/unisolated contour | Unknown, not an assertion that no technique occurred |

Defaults require at least 350 ms of reliable evidence spanning 80% of the
event, raw voicing >= 0.8, gaps <= 40 ms, excursion 0.35-4 semitones, and no
adjacent-frame jump above half a semitone. The duration, voicing, coverage,
gap, excursion, modulation-rate and frame-budget limits are configurable in
`GuitarTechniqueOptions`. Input is bounded to 8,192 frames by default; exceeding
the budget returns an explanation rather than analyzing a misleading partial
contour. Malformed nonfinite or nonmonotonic evidence throws an actionable error.
These are conservative rule thresholds, **not calibrated acoustic probabilities**.

The result keeps original optional pitch calibration separate from heuristic
gesture support and absent technique calibration. Fingering has its own absent
calibration. A sustained rise is deliberately not exported as a detected bend
or slide. Hammer-ons, pull-offs and harmonics are not detected: transient,
spectral or independently verified performance evidence is still needed.
Distortion, effects, polyphony, short ornaments, wide/fast gestures, tracking
errors and evidence truncation may prevent an observation.

In the WebView evidence inspector, retained isolated YIN evidence in a Guitar
score is reviewed through this module; overlapping source events and other
engines are not assumed to supply isolated contours. Enable the existing
evidence-review option to retain the needed frames. Live note-only summaries
do not establish a continuous contour. Unknown physical technique and confidence
remain visible. No new technique marks are written to MusicXML/MIDI or the
canonical score; source evidence remains available for later independent review.

## Manual corrections

Manual string/fret changes are hard constraints for the edited note and are
validated against its MIDI pitch, selected tuning, capo, and overlapping
string occupancy. The editor never changes MIDI pitch to make a fingering fit:
pitch edits remain an explicit, separate field. A manual correction is saved
with MIDI pitch, onset, duration, string, and fret, then matched one-to-one to
the nearest later event of the same pitch within 250 ms. A correction that no
longer matches the pitch/tuning, is ambiguous, or conflicts with a held string
is reported as rejected instead of being silently applied. The correction
remains available during later analyses in the current GUI session and is
included in subsequent score and tablature exports.

Corrections are event matches, not permanent note IDs. For repeated same-pitch
events close together, a correction may become ambiguous if a later analysis
cannot identify a unique onset match; the editor reports this so it can be
reviewed manually.

The inspector also offers ranked **whole-phrase** alternatives, so selecting
one changes all involved strings/frets in one undoable edit while preserving
pitch, onset, duration and standard notation. Alternatives are invalidated when
notes/timing/tuning/capo change. Individual candidate selection rejects an
occupied string instead of silently clearing another note. As before, the
backend validates the complete canonical transaction.

## Export and editing consistency

ASCII and MusicXML retain unresolved pitches; they never fabricate a string
to make an incomplete solution look playable. Diagnostic explanations
distinguish out-of-range pitches, constraint failure, and exhausted search.
Bad input/contradictory explicit overrides raise validation errors; a feasible
input with no retained solution returns unresolved positions with an
explanation. Supplied ASCII primary and alternative assignments are checked
for pitch, fret range, simultaneous/held-string collisions, stretch and fingers.

Canonical score assignments, including null/unresolved ones, are authoritative
in **both** exporters. MusicXML no longer silently re-solves a cleared tab
assignment. Stored frets above 24 are preserved. Musical pitch is independent
of string/fret: incompatible pitch edits clear stale assignments; string/fret
edits must match pitch. A duration/fingering edit causing same-string overlap
fails atomically and preserves the previous revision. Note timing and tied
fragments remain shared between standard notation and tablature.
Custom search costs/budgets, MIDI suggestion range and span limits are not persisted in the score
schema. They govern suggestion generation, not subsequent automatic re-solving.
Score validation enforces pitch and occupied-string consistency, not the
entire configurable ergonomic model on every manual edit.

ASCII headers label the realization as suggested, not known performed.
Automatic `h`, `p`, and `/` technique marks are now disabled by default:
short same-string transitions alone do not identify technique.
`GuitarTabOptions::suggest_legato_marks = true` opts into explicitly labelled
heuristic marks. MusicXML retains the existing string/fret technical structure;
transient heuristic left-hand fingers are available in API/GUI diagnostics and
ASCII note details, not serialized as performed MusicXML finger facts.

## Verification

`tests/fingering_tests.cpp` covers all five exposed tunings, capo use, MIDI
enharmonic equivalence, octave boundaries, simultaneous chords, sequence
movement costs, unplayable notes, overfull chords, manual correction matching,
and pitch-preserving string/fret overrides. Existing tablature and MusicXML
tests continue to exercise exports and held-string constraints.

Additional hand-checked regressions cover exhaustive tiny-sequence rankings,
complete versus truncated alternatives, held-bass stretch, open-string hand
position, relative capo/high-fret bounds, blocked barres, finger-count limits,
configurable repeated-string/transition costs, invalid supplied positions,
and transactional duration/pitch edits.

`tests/fingering_exports.py` reuses the notation runner and parses MusicXML
semantically. Its 17 cases compare standard/tab MIDI pitches, string/fret
technical elements and ASCII assignments for all five presets, custom tuning,
capo, high frets, a fixed same-string slide-like phrase, chords, impossible
assignments, and a pitch edit clearing a formerly playable assignment, plus seven/eight-string,
four-string reentrant and twelve-string configurations.
These are deterministic score/fingering references, **not** evidence of
recovering the original fingering from audio or of improved acoustic accuracy.

The new contour checks use five hand-verified mathematical traces (steady,
6 Hz vibrato, two-semitone rise/hold, release/hold, and abrupt jump) and three
synthetic harmonic audio signals processed through actual YIN (steady,
5 Hz modulation, rise/hold). Observed gesture labels matched **5/5 traces**
and **3/3 synthetic audio cases**. These tiny deterministic checks validate
integration and intended abstention, not generalization or calibrated accuracy.
They also cover low voicing, gaps, drift, evidence budgets, disabled/non-guitar
operation, and malformed timestamps.

**Real-recording validation gap:** no manually verified technique/string/fret
recording corpus was supplied for this change. Physical bend/slide/hammer-on/
pull-off/harmonic detection and original-fingering accuracy are **not measured**.
The existing [GuitarSet evaluator](../python/evaluate_guitarset.py) and
[archived note results](../python/evaluation/guitarset-v1.1.0-results.json)
remain the real-audio note-transcription baseline, not evidence for the new
gesture/fingering module. That evaluator currently consumes time/pitch notes,
not performed string/technique labels; no historical scores were presented as
new measurements. Before claiming physical-technique accuracy, acquire licensed
independently verified string/technique annotations, retain performer-separated
validation/test sets, measure abstentions and per-technique precision/recall,
and distinguish alternative playable positions from the performed position.

Reproduce from a clean checkout (C++20 toolchain, CMake and Python required):

```powershell
cmake -S . -B build-tab -DTRANSCRIBE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-tab -j 4
ctest --test-dir build-tab --output-on-failure
```

On Windows with MinGW, add `-G "MinGW Makefiles"` to configuration. For the
smallest new checks after building:

```powershell
.\build-tab\transcriber-tests.exe "[guitar],[technique],[fingering],[score],[musicxml]"
ctest --test-dir build-tab --output-on-failure -R "fingering_export_semantics|interpretation_semantics|cli_export_validation|ui_workflow_browser"
```

Earlier six-string solver validation (historical, not rerun as this change's full matrix):
model-free full CTest registered 174 tests:
172 passed and 2 skipped (optional ONNX window and external real-audio fixture).
The ONNX/ASIO build passed 36 selected score/export/integration tests. The
WebView GUI compiled and UI smoke checks passed; Node.js syntax checking was
unavailable. A browser mock-host check passed the new cost tooltip,
pitch-preserving candidate selection, explicit unresolved diagnostics, and
error-free loading. Final focused tests passed 172 assertions in 22 cases in
both model-free and ONNX/ASIO builds, plus the two semantic Python suites.

Extended-instrument/gesture validation: **341 assertions in 33 focused C++
cases passed**, in both model-free Release and ASan/UBSan builds (leak detection
disabled on this Windows toolchain). Six selected CTests passed, covering
interpretation, 17 fingering exports, CLI/config errors and positive custom
configurations, the 20-group browser editing workflow, UI assets and notation
fixtures. Strict WebView2/ONNX/ASIO/Verovio compilation passed. Independent
Verovio 6.3.0 imported **26 MusicXML files**, checked **735 pitched fragments**
and rendered **28 SVG pages**, including the new four/seven/eight/twelve-string
configurations. Node-only syntax validation was skipped because Node.js is
unavailable; browser execution ran successfully. No full CTest matrix, hardware
capture or manually annotated real-recording technique evaluation was run for
this extension.
