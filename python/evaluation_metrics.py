"""Dependency-free event/occupancy metrics shared by evaluation workflows."""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass
from typing import Any


@dataclass(frozen=True)
class Tolerances:
    onset_seconds: float = 0.05
    offset_seconds: float = 0.05
    offset_ratio: float = 0.2
    frame_seconds: float = 0.01

    def __post_init__(self) -> None:
        for name, value in asdict(self).items():
            if not math.isfinite(value) or value < 0 or (name == "frame_seconds" and value == 0):
                raise ValueError(f"Invalid metric tolerance {name}: {value}")


def checked_metrics(tp: int, fp: int, fn: int) -> dict[str, float | int]:
    if min(tp, fp, fn) < 0:
        raise ValueError("Metric counts must be nonnegative")
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else 0.0
    return dict(tp=tp, fp=fp, fn=fn, precision=precision, recall=recall, f1=f1)


def validate_events(events: list[dict[str, Any]]) -> None:
    for event in events:
        pitch = event["midi"]
        start, duration = event["start"], event["duration"]
        if isinstance(pitch, bool) or not isinstance(pitch, int) or not 0 <= pitch <= 127:
            raise ValueError(f"Invalid MIDI pitch: {event!r}")
        if not all(math.isfinite(v) for v in (start, duration)) or start < 0 or duration <= 0:
            raise ValueError(f"Invalid event timing: {event!r}")


def event_pairs(
    references: list[dict[str, Any]],
    predictions: list[dict[str, Any]],
    *,
    require_offset: bool = False,
    require_pitch: bool = True,
    tolerances: Tolerances = Tolerances(),
) -> list[tuple[int, int]]:
    """Maximum-cardinality one-to-one matches: (reference, prediction) indices.

    Closeness orders eligible edges, not a minimum-total-error objective.
    Iterative augmenting paths avoid recursion limits on long performances.
    """
    validate_events(references)
    validate_events(predictions)
    candidates = []
    for prediction in predictions:
        matches = []
        for index, reference in enumerate(references):
            if require_pitch and prediction["midi"] != reference["midi"]:
                continue
            onset = abs(prediction["start"] - reference["start"])
            offset = abs(prediction["start"] + prediction["duration"]
                         - reference["start"] - reference["duration"])
            if onset > tolerances.onset_seconds + 1e-12:
                continue
            if require_offset and offset > max(
                tolerances.offset_seconds, tolerances.offset_ratio * reference["duration"]
            ) + 1e-12:
                continue
            matches.append((onset, prediction["midi"] != reference["midi"], offset, index))
        candidates.append([i for _, _, _, i in sorted(matches)])
    owners: dict[int, int] = {}
    for origin in sorted(range(len(predictions)), key=lambda i: len(candidates[i])):
        queue = [origin]
        parent: dict[int, tuple[int, int]] = {}
        seen_predictions = {origin}
        seen_references: set[int] = set()
        found = None
        for current in queue:
            for reference in candidates[current]:
                if reference in seen_references:
                    continue
                seen_references.add(reference)
                if reference not in owners:
                    found = (current, reference)
                    break
                owner = owners[reference]
                if owner not in seen_predictions:
                    seen_predictions.add(owner)
                    parent[owner] = (current, reference)
                    queue.append(owner)
            if found is not None:
                break
        if found is not None:
            current, reference = found
            owners[reference] = current
            while current != origin:
                current, reference = parent[current]
                owners[reference] = current
    return sorted(owners.items())


def match_events(references, predictions, *, require_offset: bool) -> int:
    return len(event_pairs(references, predictions, require_offset=require_offset))


def frame_counts(references, predictions, duration: float,
                 frame_seconds: float = 0.01) -> tuple[int, int, int]:
    """Score pitch occupancy at frame centers; merge intervals without dense arrays."""
    validate_events(references)
    validate_events(predictions)
    if not math.isfinite(duration) or duration < 0:
        raise ValueError("Duration must be finite and nonnegative")
    if not math.isfinite(frame_seconds) or frame_seconds <= 0:
        raise ValueError("Frame step must be positive and finite")
    count = max(1, math.ceil(duration / frame_seconds))

    def intervals(events):
        result: dict[int, list[tuple[int, int]]] = {}
        for event in events:
            start = max(0, min(count, math.ceil(event["start"] / frame_seconds - 0.5)))
            end = max(start, min(count, math.ceil(
                (event["start"] + event["duration"]) / frame_seconds - 0.5)))
            if end > start:
                result.setdefault(event["midi"], []).append((start, end))
        for pitch, spans in result.items():
            merged: list[tuple[int, int]] = []
            for start, end in sorted(spans):
                if merged and start <= merged[-1][1]:
                    merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
                else:
                    merged.append((start, end))
            result[pitch] = merged
        return result

    ref, pred = intervals(references), intervals(predictions)
    tp = 0
    ref_count = sum(end - start for spans in ref.values() for start, end in spans)
    pred_count = sum(end - start for spans in pred.values() for start, end in spans)
    for pitch in ref.keys() & pred.keys():
        i = j = 0
        while i < len(ref[pitch]) and j < len(pred[pitch]):
            a, b = ref[pitch][i], pred[pitch][j]
            tp += max(0, min(a[1], b[1]) - max(a[0], b[0]))
            if a[1] <= b[1]:
                i += 1
            else:
                j += 1
    return tp, pred_count - tp, ref_count - tp


def distribution(values: list[float]) -> dict[str, float | int | None]:
    ordered = sorted(values)
    if not ordered:
        return dict(count=0, mean=None, median=None, p95=None, maximum=None)
    n = len(ordered)
    median = (ordered[(n - 1) // 2] + ordered[n // 2]) / 2
    return dict(count=n, mean=sum(ordered) / n, median=median,
                p95=ordered[max(0, math.ceil(0.95 * n) - 1)], maximum=ordered[-1])


def score_events(references, predictions, duration: float,
                 tolerances: Tolerances = Tolerances()) -> dict[str, Any]:
    strict = event_pairs(references, predictions, require_offset=True, tolerances=tolerances)
    onset = event_pairs(references, predictions, tolerances=tolerances)
    timing = event_pairs(references, predictions, require_pitch=False, tolerances=tolerances)
    # Pitch-independent timing matches avoid making pitch accuracy tautologically 100%.
    correct_pitch = sum(references[r]["midi"] == predictions[p]["midi"] for r, p in timing)
    chord_indices = {
        i for i, left in enumerate(references)
        if any(i != j and left["midi"] != right["midi"]
               and max(left["start"], right["start"]) <
               min(left["start"] + left["duration"], right["start"] + right["duration"])
               for j, right in enumerate(references))
    }
    chord_hits = sum(r in chord_indices for r, _ in onset)
    onsets = [abs(predictions[p]["start"] - references[r]["start"]) for r, p in onset]
    offsets = [abs(predictions[p]["start"] + predictions[p]["duration"]
                   - references[r]["start"] - references[r]["duration"]) for r, p in onset]
    return {
        "note_event": checked_metrics(len(strict), len(predictions) - len(strict),
                                      len(references) - len(strict)),
        "onset_pitch": checked_metrics(len(onset), len(predictions) - len(onset),
                                       len(references) - len(onset)),
        "frame": checked_metrics(*frame_counts(references, predictions, duration,
                                               tolerances.frame_seconds)),
        "pitch_accuracy": {"correct": correct_pitch, "matched": len(timing),
                           "value": correct_pitch / len(timing) if timing else None},
        "chord_note_recall": {"correct": chord_hits, "reference": len(chord_indices),
                              "value": chord_hits / len(chord_indices) if chord_indices else None},
        "onset_error_seconds": distribution(onsets),
        "offset_error_seconds": distribution(offsets),
        "false_positives": len(predictions) - len(strict),
        "missed_notes": len(references) - len(strict),
    }
