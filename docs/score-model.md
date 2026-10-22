# Canonical score model

The versioned `ScoreDocument` is the shared, editable representation for
notation, tablature, MIDI, and project serialization. It is declared in
[`include/transcribe/score.hpp`](../include/transcribe/score.hpp) and
implemented in [`src/score.cpp`](../src/score.cpp). The document is independent
of GUI widget state; adapters may project it for display, but edits are applied
to the document rather than kept only in a view.

## Time and pitch

`schema_version` is currently `1`. Source events are retained separately from
score notes. Their onset and offset are recording-relative seconds, and their
sample positions refer to the source sample rate recorded by the document.
Interpretation does not overwrite these measurements.

Each `ScoreNote` links to its source event IDs and stores measured audio
onset/offset beside interpreted onset and duration in quarter-note units.
The score-note ID is distinct from its source-event IDs; consumers should use
`source_event_ids` to reconcile live event identity with canonical notation
identity rather than reusing an ID across both entities.
Residuals preserve the timing difference between the two. A tempo map converts
score positions to recording-relative seconds; musical timing is therefore
not an alternate copy of the detector timestamps.

Sounding MIDI pitch is the pitch used for playback and tablature. Written MIDI
pitch includes the instrument's configured semitone transposition. Spelling,
accidentals, meter, key, beats, rests, voices, ties, tuplets, chord groups, and
rhythmic alternatives are retained as score data. Raw evidence and calibrated
confidence are separate; absent confidence is not replaced with an invented
probability. User-authored values carry provenance and a revision.

`ScoreTabNote` has its own stable ID and a `notation_note_id` relationship.
String/fret assignments are checked against sounding pitch, tuning, and capo.
Instrument tuning supports 1-12 strings in string-number order, including
reentrant tunings. Optional `physical_fret_count` constrains capo plus relative
fret; it is serialized as a nullable additive schema-1 field. Older schema-1
projects without the field retain the previous unknown-neck-length behavior.
Unknown assignments remain unassigned. Tablature positions are suggestions,
not evidence of the exact fingering used in the recording.

## Revisions and editing

`ScoreEditor` applies a `ScoreTransaction` only when its `base_revision`
matches the current document. A transaction is atomic: invalid commands or
invariant violations leave the accepted document unchanged. Supported commands
insert and delete notes/rests, change pitch or duration, split or merge notes,
change fingering, reinterpret rhythmic position/duration, and update score
context. Fingering edits do not change sounding pitch. Pitch changes clear
assignments that no longer produce that pitch.

Successful transactions increment the document revision and record actor,
reason, command ID, and origin. Undo and redo restore snapshots as new
revisions, and the bounded history retains at most 100 undo snapshots. Stale
transactions are rejected rather than silently applied to a different score.

## Serialization and migration

`serialize_score_document` writes schema-versioned JSON. Deserialization
validates the schema, IDs and relationships, pitches, timing, instrument
settings, evidence, tie groups, and revision history. The parser is
dependency-free, limits input to 64 MiB, and rejects malformed or unsupported
future schemas. Version-0 flat-note projects migrate to schema 1 with migration
provenance; migration cannot reconstruct musical interpretation or source
evidence that was not present in the old project.

The CLI option `--score-project <path>` writes this canonical project. The
existing `--json` output remains the compatibility-oriented raw detection
report; use the score project when editable musical interpretation and
notation/tab relationships are required. The GUI also exposes project
open/save and recovery through the shared project/score representation.

## Views and exports

The score-aware MusicXML and MIDI overloads validate and export a
`ScoreDocument`. MusicXML uses written pitch for notation and uses the linked
tab assignment, preserving explicitly unknown positions as unresolved;
MIDI uses sounding pitch and score-quarter timing. The score-aware ASCII tab
writer uses the document's string/fret assignments and instrument tuning.
CLI and GUI export paths use these score-aware writers, so accepted edits are
not replaced by a fresh detection pass at export time.

The GUI retains re-analysis as a separate proposal when a score is already
accepted, including an empty score. A proposal preview does not replace the
accepted revision or its exports. The GUI supports explicit proposal acceptance,
project open/save and pending-edit snapshots; proposal acceptance asks before
discarding unexported edits. See [Editing and playback](editing-playback.md)
for timebase mappings, transaction/history behavior, playback clocks and limits.

## Validation

The focused tests in [`tests/score_tests.cpp`](../tests/score_tests.cpp) cover
round trips, version-0 migration, revision conflicts, transactional edits,
undo/redo, source-time preservation, fingering validation, and consistency of
MusicXML, MIDI, and ASCII tablature with the accepted score revision. The CLI
project-export checks are in [`tests/cli_export_smoke.py`](../tests/cli_export_smoke.py).
