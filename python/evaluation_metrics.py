"""Dependency-free event/occupancy metrics shared by evaluation workflows."""

from __future__ import annotations

import math
import bisect
from dataclasses import asdict, dataclass
from typing import Any


def event_calibration_samples(report: dict) -> tuple[list[float], list[int]]:
    tolerances = Tolerances(**report["tolerances"])
    scores, targets = [], []
    for clip in report["clips"]:
        matched = {p for _, p in event_pairs(
            clip["references"], clip["predictions"], require_offset=True, tolerances=tolerances)}
        for index, prediction in enumerate(clip["predictions"]):
            score = prediction.get("confidence")
            if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
                raise ValueError("Calibration requires finite heuristic event confidence, not missing values")
            scores.append(float(score))
            targets.append(int(index in matched))
    return scores, targets


def fit_event_calibration(report: dict) -> dict:
    """Isotonic event correctness, fitted only on explicitly separated validation recordings."""
    clips = report["clips"]
    if not clips or any(clip.get("dataset_split") != "validation" for clip in clips):
        raise ValueError("Confidence fitting requires validation-only clips; never train/test/all")
    scores, targets = event_calibration_samples(report)
    if not scores or len(set(targets)) != 2:
        raise ValueError("Calibration requires both matched and unmatched predicted events")
    grouped: dict[float, list[int]] = {}
    for score, target in zip(scores, targets):
        grouped.setdefault(score, []).append(target)
    blocks = []
    for score, labels in sorted(grouped.items()):
        blocks.append([score, len(labels), sum(labels)])
        while len(blocks) > 1 and blocks[-2][2] / blocks[-2][1] > blocks[-1][2] / blocks[-1][1]:
            right = blocks.pop()
            left = blocks.pop()
            blocks.append([right[0], left[1] + right[1], left[2] + right[2]])
    return dict(schema_version=1, method="isotonic-event-correctness",
                target="one-to-one pitch+onset+offset match; conditional on predicted events",
                configuration={k: v for k, v in report["configuration"].items() if k != "dataset_split"},
                model=report["model"], evaluator=report["evaluator"],
                tolerances=report["tolerances"],
                corpus=report["corpus"],
                validation_audio_sha256=sorted({clip["audio_sha256"] for clip in clips}),
                validation_performers=sorted({clip["performer_id"] for clip in clips
                                              if clip.get("performer_id") is not None}),
                samples=len(scores), positives=sum(targets),
                upper_bounds=[block[0] for block in blocks],
                probabilities=[block[2] / block[1] for block in blocks],
                warning="Validation-fit estimates, not evidence of calibration on independent test data. "
                        "Possible model training-data overlap remains; no recall guarantee.")


def calibrated_event_confidence(score: float, calibration: dict) -> float:
    bounds, probabilities = calibration["upper_bounds"], calibration["probabilities"]
    if (calibration.get("schema_version") != 1 or
        calibration.get("method") != "isotonic-event-correctness" or not bounds or
        len(bounds) != len(probabilities) or
        any(not math.isfinite(v) for v in bounds + probabilities) or
        any(a >= b for a, b in zip(bounds, bounds[1:])) or
        any(not 0 <= p <= 1 for p in probabilities) or
        any(a > b for a, b in zip(probabilities, probabilities[1:])) or
        not math.isfinite(score)):
        raise ValueError("Invalid isotonic calibration artifact or score")
    return probabilities[min(bisect.bisect_left(bounds, score), len(bounds) - 1)]


def apply_event_calibration(report: dict, calibration: dict) -> dict:
    calibrated_event_confidence(0, calibration)
    for key in ("configuration", "model", "tolerances", "evaluator"):
        value = report[key]
        if key == "configuration":
            value = {k: v for k, v in value.items() if k != "dataset_split"}
        if value != calibration[key]:
            raise ValueError(f"Calibration {key} differs from evaluation")
    if not report["clips"] or any(clip.get("dataset_split") != "test" for clip in report["clips"]):
        raise ValueError("Calibration assessment requires test-only clips")
    if set(calibration["validation_audio_sha256"]) & {
        clip["audio_sha256"] for clip in report["clips"]
    }:
        raise ValueError("Calibration validation/test audio overlap")
    if set(calibration["validation_performers"]) & {
        clip.get("performer_id") for clip in report["clips"]
    }:
        raise ValueError("Calibration validation/test performer overlap")
    if (report["corpus"]["name"] != calibration["corpus"]["name"] or
        report["corpus"].get("version") != calibration["corpus"].get("version") or
        report["corpus"].get("provenance") != calibration["corpus"].get("provenance")):
        raise ValueError("Calibration corpus differs from evaluation")
    scores, targets = event_calibration_samples(report)
    probabilities = [calibrated_event_confidence(score, calibration) for score in scores]
    for clip in report["clips"]:
        for event in clip["predictions"]:
            event["calibrated_confidence"] = calibrated_event_confidence(event["confidence"], calibration)
    bins = []
    for index in range(10):
        items = [(p, y) for p, y in zip(probabilities, targets)
                 if min(int(p * 10), 9) == index]
        if items:
            bins.append(dict(count=len(items), mean_confidence=sum(p for p, _ in items) / len(items),
                             accuracy=sum(y for _, y in items) / len(items)))
    count = len(targets)
    return dict(events=count, bins=bins,
                brier=sum((p - y) ** 2 for p, y in zip(probabilities, targets)) / count if count else None,
                expected_calibration_error=sum(
                    b["count"] * abs(b["mean_confidence"] - b["accuracy"]) for b in bins
                ) / count if count else None,
                note="No events removed; raw heuristic confidence retained; recall is unchanged.")


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
    require_onset: bool = True,
    require_overlap: bool = False,
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
            if require_onset and onset > tolerances.onset_seconds + 1e-12:
                continue
            if require_offset and offset > max(
                tolerances.offset_seconds, tolerances.offset_ratio * reference["duration"]
            ) + 1e-12:
                continue
            if require_overlap and max(prediction["start"], reference["start"]) >= min(
                prediction["start"] + prediction["duration"],
                reference["start"] + reference["duration"],
            ):
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
        return dict(count=0, mean=None, median=None, p90=None, p95=None, p99=None, maximum=None)
    n = len(ordered)
    median = (ordered[(n - 1) // 2] + ordered[n // 2]) / 2
    percentile = lambda fraction: ordered[max(0, math.ceil(fraction * n) - 1)]
    return dict(count=n, mean=sum(ordered) / n, median=median,
                p90=percentile(.90), p95=percentile(.95), p99=percentile(.99),
                maximum=ordered[-1])


def score_events(references, predictions, duration: float,
                 tolerances: Tolerances = Tolerances()) -> dict[str, Any]:
    if not math.isfinite(duration) or duration < 0:
        raise ValueError("Duration must be finite and nonnegative")
    validate_events(references)
    validate_events(predictions)
    strict = event_pairs(references, predictions, require_offset=True, tolerances=tolerances)
    onset = event_pairs(references, predictions, tolerances=tolerances)
    timing = event_pairs(references, predictions, require_pitch=False, tolerances=tolerances)
    pitch = event_pairs(references, predictions, require_onset=False, require_overlap=True,
                        tolerances=tolerances)
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
    fragmented_references = 0
    extra_fragments = 0
    for reference in references:
        overlapping = [
            prediction for prediction in predictions
            if prediction["midi"] == reference["midi"]
            and max(prediction["start"], reference["start"]) <
            min(prediction["start"] + prediction["duration"],
                reference["start"] + reference["duration"])
        ]
        if len(overlapping) > 1:
            fragmented_references += 1
            extra_fragments += len(overlapping) - 1
    merged_predictions = 0
    extra_reference_notes = 0
    for prediction in predictions:
        overlapping = sorted(
            (reference for reference in references
             if reference["midi"] == prediction["midi"]
             and max(prediction["start"], reference["start"]) <
             min(prediction["start"] + prediction["duration"],
                 reference["start"] + reference["duration"])),
            key=lambda reference: reference["start"],
        )
        if len(overlapping) < 2:
            continue
        previous_end = overlapping[0]["start"] + overlapping[0]["duration"]
        distinct_notes = 1
        for reference in overlapping[1:]:
            if reference["start"] >= previous_end - 1e-12:
                distinct_notes += 1
            previous_end = max(previous_end, reference["start"] + reference["duration"])
        if distinct_notes > 1:
            merged_predictions += 1
            extra_reference_notes += distinct_notes - 1
    strict_false_positives = len(predictions) - len(strict)
    return {
        "note_event": checked_metrics(len(strict), len(predictions) - len(strict),
                                      len(references) - len(strict)),
        "onset_pitch": checked_metrics(len(onset), len(predictions) - len(onset),
                                       len(references) - len(onset)),
        "pitch_event": checked_metrics(len(pitch), len(predictions) - len(pitch),
                                       len(references) - len(pitch)),
        "frame": checked_metrics(*frame_counts(references, predictions, duration,
                                               tolerances.frame_seconds)),
        "pitch_accuracy": {"correct": correct_pitch, "matched": len(timing),
                           "value": correct_pitch / len(timing) if timing else None},
        "chord_note_recall": {"correct": chord_hits, "reference": len(chord_indices),
                              "value": chord_hits / len(chord_indices) if chord_indices else None},
        "onset_error_seconds": distribution(onsets),
        "offset_error_seconds": distribution(offsets),
        "note_fragmentation": {
            "fragmented_reference_notes": fragmented_references,
            "extra_predicted_fragments": extra_fragments,
            "rate": fragmented_references / len(references) if references else None,
        },
        "note_merging": {
            "merged_predictions": merged_predictions,
            "extra_reference_notes": extra_reference_notes,
            "rate": merged_predictions / len(predictions) if predictions else None,
        },
        "false_positives": strict_false_positives,
        "false_positives_per_minute": (
            strict_false_positives * 60 / duration if duration > 0 else None
        ),
        "missed_notes": len(references) - len(strict),
    }


if __name__ == "__main__":
    import argparse
    import hashlib
    import json
    from pathlib import Path
    import sys

    parser = argparse.ArgumentParser(description="Fit event correctness calibration on validation reports only")
    parser.add_argument("--fit-report", required=True, type=Path)
    parser.add_argument("--output-json", required=True, type=Path)
    args = parser.parse_args()
    try:
        source = args.fit_report.read_bytes()
        artifact = fit_event_calibration(json.loads(source))
        artifact["validation_report_sha256"] = hashlib.sha256(source).hexdigest()
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(artifact, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Calibration failed: {error}", file=sys.stderr)
        sys.exit(1)
