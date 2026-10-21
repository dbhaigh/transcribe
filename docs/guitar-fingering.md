# Guitar fingering suggestions

Pitch transcription and guitar fingering are separate operations. The fingering
solver receives MIDI pitches, measured onset/duration times, tuning, and capo;
it does not infer which string or fret the performer originally used. A
suggested position is one playable realization of the pitch, not evidence of
the performance's exact fingering.

`assign_fingerings` in `transcribe/fingering.hpp` returns the selected
string/fret for each note, up to five ranked sequence alternatives, per-note
candidate positions, ambiguity flags, correction status, and decomposed costs.
The existing `assign_guitar_tab_positions` and tablature export APIs delegate
to this component. Guitar tunings are six descending MIDI open pitches from
string 1 (highest) through string 6 (lowest); the UI and CLI provide standard,
drop-D, DADGAD, open-D, and open-G tunings. `FingeringOptions` also accepts a
custom descending tuning and capo.

## Assignment and costs

Notes within 30 ms of the first onset in a group are treated as a simultaneous
chord by default. A bounded sequence search considers valid assignments across
groups, not just each note in isolation. A string remains occupied until its
note ends, and simultaneous notes cannot share a string. Chord positions with
fretted notes spanning more than five frets are excluded by default; open
strings do not contribute to that span. If no complete playable assignment is
available, affected notes are explicitly returned with string `0` and fret
`-1`; the solver does not invent a position.

The sequence cost is a sum of these terms:

| Component | Default calculation | Effect |
| --- | --- | --- |
| Fret height | `0.12 × fret` | Slightly favors lower frets. |
| Preferred position | `0.75 × abs(fret - preferred_position)`; preferred position defaults to fret 5. | Favors the configured hand area. |
| Hand movement | `position_shift_cost × abs(current mean fret - previous mean fret)`; shift cost defaults to `1.4`. | Discourages large position changes between onset groups. |
| String preference | `0.1 × (string number - 1)` | A small deterministic tie-break favoring higher-pitched strings. |
| Chord stretch | `1.5 × max(0, fret span - 4)` | Penalizes a wide chord reach before the hard span limit. |
| Unassigned notes | `1000` per note | Strongly favors playable assignments while retaining an explicit unknown option. |

Alternatives are ranked by total sequence cost and include all component totals.
Per-note candidate costs report the cost of the best retained sequence
containing that position, not an independent per-note score. A note is marked
ambiguous when its two best candidate sequence costs differ by no more than
the default `0.5` margin. These costs are solver preferences, not probabilities
or confidence that the original performer used that position.

The search retains at most 64 sequence states and 64 local chord assignments
per group, and examines at most 100,000 local search nodes per chord. This
bounds work and memory while allowing phrase-level continuity. If the node
limit is reached, the result and exports explicitly flag that assignments need
review; the returned alternatives are the best retained sequences, not an
exhaustive enumeration of every possible fingering.

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

## Verification

`tests/fingering_tests.cpp` covers all five exposed tunings, capo use, MIDI
enharmonic equivalence, octave boundaries, simultaneous chords, sequence
movement costs, unplayable notes, overfull chords, manual correction matching,
and pitch-preserving string/fret overrides. Existing tablature and MusicXML
tests continue to exercise exports and held-string constraints.
