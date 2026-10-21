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

Onsets are snapped only when they fall within the configured
`onset_quantization_tolerance_seconds`; otherwise their score positions remain
unsnapped. Rhythm matching uses a documented tolerance and can identify dotted
values and 3:2, 5:4 and 7:4 tuplets. Swing is a separate hypothesis. The layer
does not quantize detector timestamps or claim that a fine grid implies swing
or tuplets.

## MusicXML and CLI

The interpretation-aware `write_musicxml` overload exports a selected
hypothesis using its score positions rather than the legacy sixteenth-note
snapper. It writes variable-duration measures, implicit pickup measures,
voices, chord members, rests, barline ties, key spelling, tempo-map directions,
available velocity/articulation evidence and optional written transposition.
Tempo directions are omitted while tempo remains ambiguous, unless it was
explicitly constrained.
MusicXML divisions are 960 per quarter note; off-grid timing is retained to
that resolution. Tuplet timing is encoded with `<time-modification>`. The
exporter omits an uncertain key or time signature and writes an explicit
uncertainty direction, while still using the selected meter internally to
determine measure boundaries.

The CLI accepts:

```text
--tempo <40..240>
--meter <2/4|3/4|4/4|5/4|2/8|3/8|5/8|6/8|7/8|9/8|12/8>
--key-fifths <-7..7> --mode <major|minor>
--pickup <quarter-note-length>
--written-transposition <-48..48>
```

`--preserve-expressive-timing` adds an onset-residual annotation to the
MusicXML; it does not alter note positions or audio-time JSON note fields. MIDI
and JSON metadata use the selected top-ranked interpretation. The CLI currently
exports the highest-ranked hypothesis; applications using the API can select
another hypothesis by index.

## Current limits

Meter and key rankings are heuristic and should be evaluated on annotated
scores before being treated as reliable. Tempo-change segmentation and local
key regions can be ambiguous. MusicXML output does not yet serialize all
alternative score hypotheses, full tuplet bracket grouping, instrument
catalogue transpositions, swing glyph conventions, or user-reviewed score
edits. Written transposition is an explicit semitone setting, not an inferred
instrument property. Articulation and velocity are emitted only when present
on the source event; no score dynamics are inferred from raw amplitude.

Deterministic regression fixtures for score constraints, ambiguity, rhythm,
swing, tempo changes, voices and MusicXML output are in
[`tests/interpretation_tests.cpp`](../tests/interpretation_tests.cpp).
