#!/usr/bin/env python3
"""Evaluate Transcribe against the real recordings and note labels in GuitarSet."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import random
import shutil
import subprocess
import sys
import tempfile
import wave
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any


DATASET_VERSION = "1.1.0"
MODEL_SOURCE_COMMIT = "fa5997af0a8210982619003269994a1be25eddf3"
AUDIO_MD5 = "275966d6610ac34999b58426beb119c3"
ANNOTATION_MD5 = "b39b78e63d3446f2e54ddb7a54df9b10"
ONSET_TOLERANCE_SECONDS = 0.05
FRAME_STEP_SECONDS = 0.01


def md5_file(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_metrics(tp: int, fp: int, fn: int) -> dict[str, float | int]:
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2.0 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {"tp": tp, "fp": fp, "fn": fn, "precision": precision, "recall": recall, "f1": f1}


def read_reference_events(jams_data: dict[str, Any]) -> list[dict[str, float | int]]:
    notes: list[dict[str, float | int]] = []
    for annotation in jams_data.get("annotations", []):
        if annotation.get("namespace") != "note_midi":
            continue
        for observation in annotation.get("data", []):
            try:
                start = float(observation["time"])
                duration = float(observation["duration"])
                pitch_value = float(observation["value"])
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(f"Malformed GuitarSet note annotation: {observation!r}") from error
            if not math.isfinite(start) or not math.isfinite(duration) or not math.isfinite(pitch_value):
                raise ValueError(f"Non-finite GuitarSet note annotation: {observation!r}")
            if duration <= 0.0:
                continue
            notes.append(
                {
                    "midi": math.floor(pitch_value + 0.5),
                    "start": max(0.0, start),
                    "duration": duration,
                }
            )
    return notes


def match_events(
    references: list[dict[str, float | int]],
    predictions: list[dict[str, float | int]],
    *,
    require_offset: bool,
) -> int:
    candidates: list[list[int]] = []
    for prediction in predictions:
        matches = []
        predicted_end = float(prediction["start"]) + float(prediction["duration"])
        for reference_index, reference in enumerate(references):
            if int(prediction["midi"]) != int(reference["midi"]):
                continue
            onset_error = abs(float(prediction["start"]) - float(reference["start"]))
            if onset_error > ONSET_TOLERANCE_SECONDS:
                continue
            if require_offset:
                reference_end = float(reference["start"]) + float(reference["duration"])
                offset_tolerance = max(0.05, 0.2 * float(reference["duration"]))
                if abs(predicted_end - reference_end) > offset_tolerance:
                    continue
            matches.append(reference_index)
        matches.sort(
            key=lambda index: (
                abs(float(prediction["start"]) - float(references[index]["start"])),
                abs(
                    float(prediction["start"]) + float(prediction["duration"])
                    - float(references[index]["start"]) - float(references[index]["duration"])
                ),
            )
        )
        candidates.append(matches)

    owners = [-1] * len(references)

    def assign(prediction_index: int, visited: set[int]) -> bool:
        for reference_index in candidates[prediction_index]:
            if reference_index in visited:
                continue
            visited.add(reference_index)
            if owners[reference_index] < 0 or assign(owners[reference_index], visited):
                owners[reference_index] = prediction_index
                return True
        return False

    order = sorted(range(len(predictions)), key=lambda index: len(candidates[index]))
    return sum(assign(index, set()) for index in order)


def frame_counts(
    references: list[dict[str, float | int]],
    predictions: list[dict[str, float | int]],
    duration: float,
) -> tuple[int, int, int]:
    frame_count = max(1, math.ceil(duration / FRAME_STEP_SECONDS))
    def active_frames(events: list[dict[str, float | int]]) -> dict[int, list[bool]]:
        changes: dict[int, list[int]] = {}
        for event in events:
            pitch = int(event["midi"])
            diff = changes.setdefault(pitch, [0] * (frame_count + 1))
            start = math.ceil(float(event["start"]) / FRAME_STEP_SECONDS - 0.5)
            end = math.ceil(
                (float(event["start"]) + float(event["duration"])) / FRAME_STEP_SECONDS - 0.5
            )
            start = min(frame_count, max(0, start))
            end = min(frame_count, max(start, end))
            if start < end:
                diff[start] += 1
                diff[end] -= 1
        result: dict[int, list[bool]] = {}
        for pitch, diff in changes.items():
            active = 0
            frames = []
            for change in diff[:-1]:
                active += change
                frames.append(active > 0)
            result[pitch] = frames
        return result

    reference_frames = active_frames(references)
    prediction_frames = active_frames(predictions)
    true_positive = false_positive = false_negative = 0
    for pitch in reference_frames.keys() | prediction_frames.keys():
        ref = reference_frames.get(pitch, [False] * frame_count)
        pred = prediction_frames.get(pitch, [False] * frame_count)
        for ref_active, pred_active in zip(ref, pred):
            if ref_active and pred_active:
                true_positive += 1
            elif pred_active:
                false_positive += 1
            elif ref_active:
                false_negative += 1
    return true_positive, false_positive, false_negative


def output_events(json_path: Path) -> list[dict[str, float | int]]:
    with json_path.open(encoding="utf-8") as source:
        data = json.load(source)
    events = []
    for note in data.get("notes", []):
        events.append(
            {
                "midi": int(note["midi"]),
                "start": float(note["start"]),
                "duration": float(note["duration"]),
            }
        )
    return events


def run_transcriber(
    executable: Path,
    model: Path | None,
    audio_path: Path,
    output_directory: Path,
    label: str,
) -> list[dict[str, float | int]]:
    json_path = output_directory / f"{label}.json"
    command = [
        str(executable),
        str(audio_path),
        "--json",
        str(json_path),
        "--musicxml",
        str(output_directory / f"{label}.musicxml"),
        "--tab",
        str(output_directory / f"{label}.txt"),
        "--quiet",
    ]
    if model is not None:
        command.extend(["--model", str(model), "--polyphonic"])
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join(
        [str(executable.parent), environment.get("PATH", "")]
    )
    result = subprocess.run(command, capture_output=True, text=True, env=environment)
    if result.returncode:
        message = result.stderr + result.stdout
        if "No stable pitches were detected" in message and not json_path.exists():
            return []
        raise RuntimeError(
            f"Transcribe failed for {audio_path.name} ({label}, exit {result.returncode}):\n"
            f"{message.strip()}"
        )
    if not json_path.is_file():
        raise RuntimeError(f"Transcribe completed without writing {json_path}.")
    return output_events(json_path)


def update_counts(
    aggregate: dict[str, list[int]],
    key: str,
    references: list[dict[str, float | int]],
    predictions: list[dict[str, float | int]],
    duration: float,
) -> dict[str, dict[str, float | int]]:
    onset_tp = match_events(references, predictions, require_offset=False)
    note_tp = match_events(references, predictions, require_offset=True)
    frame_tp, frame_fp, frame_fn = frame_counts(references, predictions, duration)
    counts = {
        "onset_pitch": (onset_tp, len(predictions) - onset_tp, len(references) - onset_tp),
        "note_event": (note_tp, len(predictions) - note_tp, len(references) - note_tp),
        "frame_10ms": (frame_tp, frame_fp, frame_fn),
    }
    per_clip = {}
    for metric, (tp, fp, fn) in counts.items():
        totals = aggregate[key + ":" + metric]
        totals[0] += tp
        totals[1] += fp
        totals[2] += fn
        per_clip[metric] = checked_metrics(tp, fp, fn)
    return per_clip


def evaluate(args: argparse.Namespace) -> dict[str, Any]:
    executable = args.transcribe.resolve()
    model = args.model.resolve()
    audio_archive = args.audio_zip.resolve()
    annotation_archive = args.annotations_zip.resolve()
    for path in (executable, model, audio_archive, annotation_archive):
        if not path.is_file():
            raise FileNotFoundError(f"Required evaluation input does not exist: {path}")
    if not args.skip_checksums:
        for path, expected in (
            (audio_archive, AUDIO_MD5),
            (annotation_archive, ANNOTATION_MD5),
        ):
            actual = md5_file(path)
            if actual.lower() != expected:
                raise ValueError(
                    f"Unexpected {path.name} checksum {actual}; expected {expected}. "
                    "Use --skip-checksums only for a verified equivalent archive."
                )

    with zipfile.ZipFile(annotation_archive) as annotation_zip, zipfile.ZipFile(
        audio_archive
    ) as audio_zip:
        audio_names = {
            PurePosixPath(name).name: name
            for name in audio_zip.namelist()
            if name.lower().endswith(".wav")
        }
        jam_names = sorted(
            name for name in annotation_zip.namelist() if name.lower().endswith(".jams")
        )
        if not jam_names:
            raise ValueError(f"No .jams annotation files found in {annotation_archive}.")
        if args.limit is not None:
            if args.limit < 1:
                raise ValueError("--limit must be at least 1.")
            if args.limit < len(jam_names):
                random.Random(args.seed).shuffle(jam_names)
                jam_names = sorted(jam_names[: args.limit])

        aggregate = {
            f"{label}:{metric}": [0, 0, 0]
            for label in ("basic_pitch", "yin")
            for metric in ("onset_pitch", "note_event", "frame_10ms")
        }
        per_clip: list[dict[str, Any]] = []
        modes: list[tuple[str, Path | None]] = [("basic_pitch", model)]
        if not args.skip_yin_baseline:
            modes.append(("yin", None))

        with tempfile.TemporaryDirectory(prefix="transcribe-guitarset-") as temporary:
            workspace = Path(temporary)
            for index, jam_name in enumerate(jam_names, start=1):
                annotation_stem = PurePosixPath(jam_name).stem
                audio_basename = annotation_stem + "_mic.wav"
                audio_name = audio_names.get(audio_basename)
                if audio_name is None:
                    raise FileNotFoundError(
                        f"No mono-microphone WAV matching {audio_basename} "
                        f"was found in {audio_archive.name}."
                    )
                jams_data = json.loads(annotation_zip.read(jam_name))
                references = read_reference_events(jams_data)
                if not references:
                    raise ValueError(f"No note_midi reference events in {jam_name}.")

                wav_path = workspace / audio_basename
                with audio_zip.open(audio_name) as source, wav_path.open("wb") as target:
                    shutil.copyfileobj(source, target)
                with wave.open(str(wav_path), "rb") as wav:
                    duration = wav.getnframes() / wav.getframerate()

                clip_result: dict[str, Any] = {
                    "track": annotation_stem,
                    "reference_notes": len(references),
                    "duration_seconds": duration,
                    "systems": {},
                }
                for label, selected_model in modes:
                    output_directory = workspace / label
                    output_directory.mkdir(exist_ok=True)
                    predictions = run_transcriber(
                        executable, selected_model, wav_path, output_directory, annotation_stem
                    )
                    clip_result["systems"][label] = {
                        "predicted_notes": len(predictions),
                        "metrics": update_counts(
                            aggregate, label, references, predictions, duration
                        ),
                    }
                per_clip.append(clip_result)
                print(
                    f"\rEvaluated {index}/{len(jam_names)}: {annotation_stem}",
                    end="",
                    file=sys.stderr,
                    flush=True,
                )
                wav_path.unlink()
        print(file=sys.stderr)

    systems: dict[str, Any] = {}
    for label, _ in modes:
        systems[label] = {
            metric: checked_metrics(*aggregate[f"{label}:{metric}"])
            for metric in ("onset_pitch", "note_event", "frame_10ms")
        }
    performance_types: dict[str, Any] = {}
    for performance_type in ("comp", "solo"):
        clips = [
            clip
            for clip in per_clip
            if clip["track"].endswith("_" + performance_type)
        ]
        grouped_systems = {}
        for label, _ in modes:
            grouped_systems[label] = {
                metric: checked_metrics(
                    *(
                        sum(
                            clip["systems"][label]["metrics"][metric][count]
                            for clip in clips
                        )
                        for count in ("tp", "fp", "fn")
                    )
                )
                for metric in ("onset_pitch", "note_event", "frame_10ms")
            }
        performance_types[performance_type] = {
            "recordings": len(clips),
            "reference_notes": sum(clip["reference_notes"] for clip in clips),
            "systems": grouped_systems,
        }
    return {
        "dataset": {
            "name": "GuitarSet",
            "version": DATASET_VERSION,
            "doi": "10.5281/zenodo.3371780",
            "recordings_evaluated": len(jam_names),
            "audio_archive_md5": AUDIO_MD5,
            "annotation_archive_md5": ANNOTATION_MD5,
            "audio_source": "audio_mono-mic.zip",
            "license": "CC BY 4.0",
        },
        "model": {
            "name": "Spotify Basic Pitch",
            "source_commit": MODEL_SOURCE_COMMIT,
            "sha256": sha256_file(model),
        },
        "evaluation": {
            "onset_pitch_tolerance_seconds": ONSET_TOLERANCE_SECONDS,
            "note_event_offset_tolerance": "max(50 ms, 20% of reference duration)",
            "frame_step_seconds": FRAME_STEP_SECONDS,
            "prediction_thresholds": "Transcribe defaults; not tuned on this dataset",
            "seed": args.seed if args.limit is not None else None,
            "python": platform.python_version(),
            "platform": platform.platform(),
        },
        "systems": systems,
        "by_performance_type": performance_types,
        "per_recording": per_clip,
    }


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description=(
            "Evaluate Basic Pitch and the monophonic YIN baseline on GuitarSet "
            "mono-microphone recordings."
        )
    )
    parser.add_argument("--transcribe", type=Path, required=True, help="Built transcribe executable")
    parser.add_argument(
        "--model",
        type=Path,
        default=repository / "python" / "models" / "basic_pitch.onnx",
        help="ONNX model (default: bundled Basic Pitch)",
    )
    parser.add_argument("--audio-zip", type=Path, required=True, help="GuitarSet audio_mono-mic.zip")
    parser.add_argument("--annotations-zip", type=Path, required=True, help="GuitarSet annotation.zip")
    parser.add_argument("--limit", type=int, help="Evaluate a seeded random subset (default: all recordings)")
    parser.add_argument("--seed", type=int, default=2026, help="Subset selection seed")
    parser.add_argument("--skip-yin-baseline", action="store_true", help="Run only the ONNX model")
    parser.add_argument("--skip-checksums", action="store_true", help="Skip the pinned Zenodo MD5 checks")
    parser.add_argument("--output-json", type=Path, help="Write full per-recording metrics to this path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        results = evaluate(args)
        for label, metrics in results["systems"].items():
            print(f"\n{label} ({results['dataset']['recordings_evaluated']} recordings)")
            for metric, values in metrics.items():
                print(
                    f"  {metric:12} F1 {values['f1']:.4f} "
                    f"(P {values['precision']:.4f}, R {values['recall']:.4f}; "
                    f"TP {values['tp']}, FP {values['fp']}, FN {values['fn']})"
                )
        if args.output_json:
            output_path = args.output_json.resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(
                json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            print(f"\nDetailed per-recording results: {output_path}")
    except (FileNotFoundError, OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print(f"Evaluation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
