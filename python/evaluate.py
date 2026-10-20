#!/usr/bin/env python3
"""Reproducible synthetic/real-audio quality and process-cold performance evaluation."""

from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import random
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any
import wave
import zipfile

from evaluation_metrics import (
    Tolerances, checked_metrics, distribution, event_pairs, score_events, validate_events,
)
from synthetic_corpus import generate
from evaluate_guitarset import (
    ANNOTATION_MD5, AUDIO_MD5, DATASET_VERSION, md5_file, read_reference_events, sha256_file,
)


SCHEMA_VERSION = 1
ROOT = Path(__file__).resolve().parents[1]


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
                    encoding="utf-8")


def source_identity() -> dict:
    files = ["python/evaluate.py", "python/evaluation_metrics.py", "python/synthetic_corpus.py",
             "python/evaluate_guitarset.py", "tests/evaluation_runner.cpp", "CMakeLists.txt",
             "tests/evaluation_build.json.in", "src/transcriber.cpp", "src/audio_file.cpp",
             "src/audio_ring_buffer.cpp", "include/transcribe/transcriber.hpp",
             "include/transcribe/audio.hpp"]
    identity = {name: sha256_file(ROOT / name) for name in files}
    result: dict[str, Any] = {"files_sha256": identity, "git_revision": None, "git_dirty": None}
    if shutil.which("git"):
        revision = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                  capture_output=True, text=True)
        if revision.returncode:
            raise RuntimeError(f"Cannot identify source revision: {revision.stderr}")
        status = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain"],
                                capture_output=True, text=True, check=True)
        result.update(git_revision=revision.stdout.strip(), git_dirty=bool(status.stdout.strip()))
    return result


def build_identity(path: Path, runner: Path) -> dict:
    info = json.loads(path.read_text(encoding="utf-8"))
    for key in ("compiler", "build_type", "flags", "features", "runner"):
        if not info.get(key):
            raise ValueError(f"Build metadata missing {key}: {path}")
    if Path(info["runner"]).resolve() != runner:
        raise ValueError("Build metadata does not describe the requested runner")
    if info["build_type"] == "unspecified":
        print("Warning: benchmark build type is unspecified; do not compare as Release", file=sys.stderr)
    return dict(info, metadata_sha256=sha256_file(path), runner_sha256=sha256_file(runner))


def real_corpus(args, directory: Path) -> dict:
    if args.audio_zip is None or args.annotations_zip is None:
        raise ValueError("GuitarSet requires --audio-zip and --annotations-zip")
    actual = {}
    for label, path, expected in (
        ("audio", args.audio_zip, AUDIO_MD5), ("annotations", args.annotations_zip, ANNOTATION_MD5)
    ):
        actual[label] = md5_file(path)
        if actual[label] != expected:
            raise ValueError(f"Unexpected GuitarSet {label} archive MD5: {actual[label]}; expected {expected}")
    clips = []
    with zipfile.ZipFile(args.audio_zip) as audio, zipfile.ZipFile(args.annotations_zip) as labels:
        names = {}
        for name in audio.namelist():
            if name.lower().endswith(".wav"):
                base = PurePosixPath(name).name
                if base in names:
                    raise ValueError(f"Ambiguous audio basename in archive: {base}")
                names[base] = name
        jams = sorted(name for name in labels.namelist() if name.endswith(".jams"))
        if not jams:
            raise ValueError("No GuitarSet annotations found")
        if args.limit is not None and args.limit < len(jams):
            random.Random(args.seed).shuffle(jams)
            jams = sorted(jams[:args.limit])
        for index, name in enumerate(jams):
            track = PurePosixPath(name).stem
            audio_name = names.get(track + "_mic.wav")
            if audio_name is None:
                raise ValueError(f"Missing microphone audio for {track}")
            # Local numeric filenames avoid trusting archive member paths.
            path = directory / f"real-{index}.wav"
            with audio.open(audio_name) as source, path.open("wb") as target:
                shutil.copyfileobj(source, target)
            with wave.open(str(path), "rb") as wav:
                rate = wav.getframerate()
                duration = wav.getnframes() / rate
            notes = read_reference_events(json.loads(labels.read(name)))
            for event in notes:
                event.update(onset=event["start"], offset=event["start"] + event["duration"],
                             instrument="guitar", velocity=None)
            clips.append(dict(id=track, audio=path.name, sample_rate=rate,
                              duration_seconds=duration, notes=notes, sha256=sha256_file(path)))
    corpus = dict(name="GuitarSet", version=DATASET_VERSION, seed=args.seed,
                  provenance={"doi": "10.5281/zenodo.3371780", "license": "CC BY 4.0",
                              "archive_md5": actual, "possible_model_training_overlap": True},
                  clips=clips)
    corpus["sha256"] = hashlib.sha256(
        json.dumps(corpus, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return corpus


def invoke(args, audio: Path, output: Path) -> tuple[dict, float]:
    command = [str(args.runner), str(audio), str(output), "--backend", args.backend,
               "--mode", args.mode, "--minimum-confidence", str(args.minimum_confidence),
               "--chunk-seconds", str(args.chunk_seconds),
               "--overlap-seconds", str(args.overlap_seconds), "--latency-ms", str(args.latency_ms)]
    if args.fast:
        command.append("--fast")
    if args.serial:
        command.append("--serial")
    if args.model:
        command.extend(["--model", str(args.model), "--provider", args.provider])
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join([str(args.runner.parent), environment.get("PATH", "")])
    output.unlink(missing_ok=True)
    started = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True, env=environment,
                            timeout=args.timeout_seconds)
    wall = time.perf_counter() - started
    if result.returncode:
        raise RuntimeError(f"Evaluation runner failed ({audio.name}): {result.stderr}{result.stdout}")
    data = json.loads(output.read_text(encoding="utf-8"))
    validate_events(data["notes"])
    for key in ("total_seconds", "decode_seconds", "analysis_seconds"):
        if key == "decode_seconds" and data["decode_included_in_analysis"] and data[key] is None:
            continue
        if not math.isfinite(data[key]) or data[key] < 0:
            raise ValueError(f"Invalid runner timing: {key}")
    return data, wall


def summarize(rows: list[dict], tolerances: Tolerances) -> dict:
    summary = {}
    for metric in ("note_event", "onset_pitch", "frame"):
        counts = [sum(row["metrics"][metric][name] for row in rows) for name in ("tp", "fp", "fn")]
        summary[metric] = checked_metrics(*counts)
    for metric, denominator in (("pitch_accuracy", "matched"), ("chord_note_recall", "reference")):
        correct = sum(row["metrics"][metric]["correct"] for row in rows)
        total = sum(row["metrics"][metric][denominator] for row in rows)
        summary[metric] = {"correct": correct, denominator: total,
                           "value": correct / total if total else None}
    onsets, offsets = [], []
    latency, first_updates = [], []
    process, analysis, totals = [], [], []
    for row in rows:
        refs, preds = row["references"], row["predictions"]
        for r, p in event_pairs(refs, preds, tolerances=tolerances):
            onsets.append(abs(preds[p]["start"] - refs[r]["start"]))
            offsets.append(abs(preds[p]["start"] + preds[p]["duration"]
                               - refs[r]["start"] - refs[r]["duration"]))
        latency.extend(row["realtime_onset_analysis_latency_seconds"])
        first_updates.extend(row["first_update_seconds"])
        process.extend(row["process_wall_seconds"])
        analysis.extend(run["analysis_seconds"] for run in row["runs"])
        totals.extend(run["total_seconds"] for run in row["runs"])
    summary.update(
        false_positives=summary["note_event"]["fp"], missed_notes=summary["note_event"]["fn"],
        onset_error_seconds=distribution(onsets), offset_error_seconds=distribution(offsets),
        realtime_onset_analysis_latency_seconds=distribution(latency),
        first_update_seconds=distribution(first_updates),
        process_wall_seconds=distribution(process), analysis_seconds=distribution(analysis),
        total_seconds=distribution(totals),
        skipped_inferences=sum(run["inference_skipped"] for row in rows for run in row["runs"]),
        corpus_audio_seconds=sum(row["duration_seconds"] for row in rows),
        corpus_real_time_factor=sum(totals) / sum(
            row["duration_seconds"] * len(row["runs"]) for row in rows
        ) if any(row["duration_seconds"] for row in rows) else None,
    )
    return summary


def evaluate(args) -> dict:
    args.runner = args.runner.resolve()
    build = build_identity(args.build_info, args.runner)
    if args.model:
        args.model = args.model.resolve()
        if not args.model.is_file():
            raise FileNotFoundError(args.model)
    tolerances = Tolerances(args.onset_tolerance, args.offset_tolerance,
                            args.offset_ratio, args.frame_step)
    config = dict(backend=args.backend, mode=args.mode, fast=args.fast, parallel=not args.serial,
                  minimum_confidence=args.minimum_confidence, provider=args.provider,
                  chunk_seconds=args.chunk_seconds, overlap_seconds=args.overlap_seconds,
                  realtime_latency_ms=args.latency_ms, realtime_update_rate_hz=100,
                  realtime_minimum_confidence=.45, maximum_pitches_per_frame=6,
                  spectral_threshold=.3, onnx_polyphonic_threshold=.4,
                  onnx_independent_probabilities=True, device_id=0, instrument_prior="none")
    with tempfile.TemporaryDirectory(prefix="transcriber-evaluation-") as temporary:
        directory = Path(temporary)
        corpus = generate(directory) if args.corpus == "synthetic" else real_corpus(args, directory)
        rows = []
        for clip in corpus["clips"]:
            data_runs, wall_runs = [], []
            for iteration in range(args.warmups + args.repetitions):
                data, wall = invoke(args, directory / clip["audio"], directory / "prediction.json")
                if data["sample_rate"] != clip["sample_rate"] or abs(
                    data["duration_seconds"] - clip["duration_seconds"]
                ) > 1 / clip["sample_rate"]:
                    raise ValueError(f"Runner/corpus audio mismatch: {clip['id']}")
                if iteration >= args.warmups:
                    data_runs.append(data)
                    wall_runs.append(wall)
            predictions = data_runs[0]["notes"]
            if any(run["notes"] != predictions for run in data_runs[1:]):
                raise RuntimeError(f"Nondeterministic predictions across repetitions: {clip['id']}")
            metrics = score_events(clip["notes"], predictions, clip["duration_seconds"], tolerances)
            latencies = [
                event["detected_at_seconds"] - event["time_seconds"]
                for run in data_runs for event in run["events"] if event["type"] == "onset"
            ]
            rows.append(dict(
                id=clip["id"], sample_rate=clip["sample_rate"], audio_sha256=clip["sha256"],
                duration_seconds=clip["duration_seconds"], references=clip["notes"],
                predictions=predictions, metrics=metrics, runs=[
                    {key: value for key, value in run.items() if key not in ("notes", "events")}
                    for run in data_runs],
                realtime_events=data_runs[0]["events"],
                realtime_onset_analysis_latency_seconds=latencies,
                first_update_seconds=[run["first_update_seconds"] for run in data_runs
                                      if run["first_update_seconds"] is not None],
                process_wall_seconds=wall_runs,
                real_time_factor=[run["total_seconds"] / clip["duration_seconds"]
                                  if clip["duration_seconds"] else None for run in data_runs],
            ))
        corpus.pop("clips")
    return dict(
        schema_version=SCHEMA_VERSION, generated_at_utc=datetime.now(timezone.utc).isoformat(),
        corpus=corpus, build=build, audio_sample_rates=sorted({row["sample_rate"] for row in rows}),
        evaluator=source_identity(), environment={
            "python": platform.python_version(), "platform": platform.platform(),
            "machine": platform.machine(), "processor": platform.processor(),
            "logical_cpus": os.cpu_count(),
        },
        model={"identifier": args.model_identifier or ("onnx:" + sha256_file(args.model)
                  if args.model else "builtin:" + args.backend),
               "sha256": sha256_file(args.model) if args.model else None},
        configuration=config, tolerances=asdict(tolerances),
        timing_protocol={"clock": "steady_clock engine / perf_counter parent",
                         "repetitions": args.repetitions, "warmups": args.warmups,
                         "isolation": "new process per clip and repetition; model/session cold",
                         "first_update": "wall time to first nonempty streaming update/realtime event",
                         "realtime_latency": "detected_at - estimated onset in input time; not hardware latency",
                         "offline_latency": "decode + analysis total; no exports/process startup"},
        summary=summarize(rows, tolerances), clips=rows,
    )


def compare(previous: dict, current: dict) -> dict:
    for key in ("schema_version", "configuration", "tolerances", "model", "timing_protocol"):
        if previous[key] != current[key]:
            raise ValueError(f"Incompatible comparison: {key} differs")
    if previous["corpus"]["sha256"] != current["corpus"]["sha256"]:
        raise ValueError("Incompatible comparison: corpus identity differs")
    old = previous["summary"]["note_event"]["f1"]
    new = current["summary"]["note_event"]["f1"]
    before = previous["summary"]["process_wall_seconds"]["median"]
    after = current["summary"]["process_wall_seconds"]["median"]
    return dict(event_f1_delta=new - old,
                frame_f1_delta=current["summary"]["frame"]["f1"] - previous["summary"]["frame"]["f1"],
                median_process_time_ratio=after / before if before else None,
                same_build=previous["build"] == current["build"],
                same_environment=previous["environment"] == current["environment"])


def human_report(report: dict) -> str:
    summary = report["summary"]
    lines = [f"{report['corpus']['name']} / {report['configuration']['backend']} / "
             f"{report['configuration']['mode']} ({len(report['clips'])} clips)",
             f"Build: {report['build']['compiler']['id']} {report['build']['compiler']['version']} "
             f"{report['build']['build_type']}; model {report['model']['identifier']}; "
             f"provider {report['configuration']['provider']}"]
    for metric in ("note_event", "onset_pitch", "frame"):
        m = summary[metric]
        lines.append(f"{metric:12} P {m['precision']:.4f} R {m['recall']:.4f} F1 {m['f1']:.4f}"
                     f" (TP {m['tp']}, FP {m['fp']}, FN {m['fn']})")
    for name in ("pitch_accuracy", "chord_note_recall"):
        value = summary[name]["value"]
        lines.append(f"{name}: {value:.4f}" if value is not None else f"{name}: n/a (no eligible notes)")
    for name in ("onset_error_seconds", "offset_error_seconds", "analysis_seconds", "total_seconds",
                 "process_wall_seconds", "first_update_seconds",
                 "realtime_onset_analysis_latency_seconds"):
        value = summary[name]
        lines.append(f"{name}: median={value['median']}, p95={value['p95']}, n={value['count']}")
    lines.append("Timing is process-cold and machine-dependent; frame F1 is not event F1.")
    lines.append(f"Corpus real-time factor (engine): {summary['corpus_real_time_factor']}")
    if summary["skipped_inferences"]:
        lines.append(f"Skipped empty-input ONNX invocations: {summary['skipped_inferences']}")
    if "comparison" in report:
        lines.append("Comparison: " + json.dumps(report["comparison"], sort_keys=True))
    return "\n".join(lines) + "\n"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--build-info", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-text", type=Path)
    parser.add_argument("--backend", choices=("yin", "spectral", "onnx"), default="yin")
    parser.add_argument("--mode", choices=("full", "streaming", "realtime"), default="streaming")
    parser.add_argument("--corpus", choices=("synthetic", "guitarset"), default="synthetic")
    parser.add_argument("--model", type=Path)
    parser.add_argument("--model-identifier")
    parser.add_argument("--provider", choices=("cpu", "cuda", "directml", "rocm"), default="cpu")
    parser.add_argument("--fast", action="store_true")
    parser.add_argument("--serial", action="store_true")
    parser.add_argument("--minimum-confidence", type=float, default=.25)
    parser.add_argument("--chunk-seconds", type=float, default=15)
    parser.add_argument("--overlap-seconds", type=float, default=1)
    parser.add_argument("--latency-ms", type=float, default=40)
    parser.add_argument("--onset-tolerance", type=float, default=.05)
    parser.add_argument("--offset-tolerance", type=float, default=.05)
    parser.add_argument("--offset-ratio", type=float, default=.2)
    parser.add_argument("--frame-step", type=float, default=.01)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=float, default=120)
    parser.add_argument("--audio-zip", type=Path)
    parser.add_argument("--annotations-zip", type=Path)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--max-event-f1-drop", type=float)
    args = parser.parse_args()
    if args.repetitions < 1 or args.warmups < 0 or (args.limit is not None and args.limit < 1):
        parser.error("Repetitions/limit must be positive; warmups must be nonnegative")
    for name in ("minimum_confidence", "chunk_seconds", "overlap_seconds", "latency_ms",
                 "timeout_seconds", "onset_tolerance", "offset_tolerance", "offset_ratio", "frame_step"):
        if not math.isfinite(getattr(args, name)):
            parser.error(f"{name} must be finite")
    if args.timeout_seconds <= 0 or not 0 <= args.minimum_confidence <= 1:
        parser.error("Invalid timeout or minimum confidence")
    if ((args.backend == "onnx") != bool(args.model) or
        (args.backend != "onnx" and args.provider != "cpu") or
        (args.backend == "onnx" and (args.mode != "full" or args.fast)) or
        (args.mode == "realtime" and (args.backend != "yin" or args.fast
                                    or args.minimum_confidence != .25))):
        parser.error("Unsupported backend/mode/model/provider combination")
    if args.max_event_f1_drop is not None and (
        not args.compare or not math.isfinite(args.max_event_f1_drop) or args.max_event_f1_drop < 0
    ):
        parser.error("A finite nonnegative --max-event-f1-drop requires --compare")
    return args


def main() -> int:
    args = parse_args()
    try:
        report = evaluate(args)
        if args.compare:
            report["comparison"] = compare(json.loads(args.compare.read_text(encoding="utf-8")), report)
        write_json(args.output_json, report)
        text = human_report(report)
        print(text, end="")
        if args.output_text:
            args.output_text.parent.mkdir(parents=True, exist_ok=True)
            args.output_text.write_text(text, encoding="utf-8")
        if args.max_event_f1_drop is not None and (
            report["comparison"]["event_f1_delta"] < -args.max_event_f1_drop
        ):
            print("Event F1 regression exceeds the requested gate", file=sys.stderr)
            return 1
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError,
            zipfile.BadZipFile) as error:
        print(f"Evaluation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
