# Evidence-based transcription review

The optional **Evidence review** mode retains detector evidence during offline
file transcription and makes it available in the note inspector. It is off by
default, applies only to file transcription, and does not add work to live
capture or the audio callback.

## Evidence and confidence

The canonical score keeps detector evidence separately from calibrated
confidence. An evidence sample records its time, pitch, raw detector strength,
voicing value, optional model activation, and semantics such as `heuristic-score`,
`relative-activation`, or `raw-activation`. These values describe what the
backend emitted; they are not probabilities of correctness.

The legacy `Note.confidence` API remains available for compatibility, but
values from that interface are tagged as heuristic evidence. They are not
copied into `calibrated_confidence`. The UI shows a calibrated correctness
estimate only when one is explicitly present in the score. No such calibration
model or representative held-out dataset is currently configured, so the
current transcription workflow reports no calibrated estimates and does not
assess out-of-distribution inputs.

`evaluate_calibration` provides diagnostics, not a fitted calibration model.
It reports Brier score and reliability bins only when the caller identifies a
sufficiently large labelled dataset that is held out from training,
representative of deployment, and has had training overlap reviewed. Its
default minimum is 100 examples. The dataset identity and eligibility are
caller-supplied and must be justified by the evaluation workflow; the helper
cannot verify provenance or dataset independence itself.

## Review findings and suggestions

Review findings are transparent heuristics, currently covering:

- Comparable retained pitch activations that conflict with the selected pitch.
- An isolated octave displacement between nearby notes in the same voice.
- A deep interior raw-strength valley that could indicate a merged repeated
  note.
- Missing detector evidence, unassigned guitar fingering, unusually long
  measured durations, and near-simultaneous notes without a shared chord group.

The inspector's review queue ranks up to 12 notes by the highest retained
finding or suggestion ranking score, with earlier measured onsets breaking
ties. Notes with an ambiguous assessment but no ranked finding are included
after ranked entries and labelled unranked. Selecting an entry jumps to that
score note and exposes its individual evidence, findings, correction controls,
and (for supported WAV files) source audio audition. The queue is a navigation
aid, not a calibrated measure of which passage is objectively most likely to
be wrong.

Pitch, onset, offset, fingering, rhythmic, and distribution assessments are
reported separately. Onset/offset uncertainty and distribution shift are
currently marked **not assessed**. Rhythm ambiguity uses the score's retained
tempo, meter, and rhythmic alternatives; absence of a warning is not a
correctness guarantee. Fingering and chord findings describe score consistency,
not the performance's original fingering or intended harmony.

Suggestion `ranking_score` is only a bounded ordering aid. Each suggestion
includes its `ranking_basis`; neither that value nor detector scores are
calibrated probabilities. Recommendations are intended to prioritize human
inspection, not make silent score corrections. Users can audition a WAV event
region, apply a pitch or split suggestion, or dismiss it. Applying a suggestion
marks the affected note as manually corrected. Decisions are recorded through
canonical score transactions and participate in editor undo/redo and project
serialization.

Event-region audition is currently available for WAV input in the Windows
WebView2 application. Other formats and live recordings are not supported by
that control; the UI reports that limitation rather than synthesizing a
replacement audio example.

## Bounded evidence retention

Evidence capture is opt-in. To bound memory use, the collector retains at most
2,048 deterministically sampled frames per backend evidence track. Each source
event retains at most 64 selected-pitch samples and 8 alternative-pitch
samples, subject to a total limit of 100,000 evidence samples per score.
Truncation is reported to the user. Alternative pitches are retained only
when the backend supplies a raw activation value; no missing candidate is
manufactured.

These limits are implementation safeguards, not claims that the retained
samples are statistically representative. Reservoir sampling is deterministic
for the current track order and sampling algorithm, but changing the backend,
frame order, or evidence schema can change which samples are retained.

## Reproduction and tests

Build with the test option enabled and run the focused review/score tests:

```powershell
cmake -S . -B build-review -DCMAKE_BUILD_TYPE=Release -DTRANSCRIBE_BUILD_TESTS=ON
cmake --build build-review --config Release --target transcriber-tests
ctest --test-dir build-review -C Release --output-on-failure -R "review|score|ui_asset_smoke"
```

The review tests cover absent evidence, conflicting raw pitch activations,
octave and repeated-note suggestions, calibration-data limitations, and
reversible score-decision history. The tests do not establish detector
accuracy, calibration quality, or the usefulness of suggestions on real
recordings; those require a separately documented labelled evaluation corpus.
