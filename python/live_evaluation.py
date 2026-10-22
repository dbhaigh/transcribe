#!/usr/bin/env python3
"""Evaluate the incremental live pipeline and its full-context refinement on one corpus.

Every clip is replayed through tests/live_runner.cpp (transcriber-live). Live notes and the
separate refinement proposal are scored against the same ground truth with the shared metrics,
and queue/latency counters are recorded. With --check the script fails on unbounded queues,
dropped events, duplicate live notes, unfinished sessions or a refinement F1 regression.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

from evaluation_metrics import Tolerances, checked_metrics, distribution, event_pairs, score_events
from evaluate import ROOT, write_json, source_identity
from evaluate_guitarset import sha256_file
from synthetic_corpus import generate

SCHEMA_VERSION = 1
LATENCIES = ("capture_latency", "analysis_latency", "end_to_end_latency",
             "stabilization_delay", "algorithmic_latency", "analysis_queue_latency",
             "confirmation_latency", "publication_queue_latency", "callback_execution")


def duplicates(notes: list[dict]) -> int:
    """Same-pitch live notes whose time ranges overlap (the pipeline must never emit these)."""
    count = 0
    ordered = sorted(notes, key=lambda n: (n["midi"], n["start"]))
    for left, right in zip(ordered, ordered[1:]):
        if left["midi"] == right["midi"] and right["start"] < left["start"] + left["duration"] - 1e-9:
            count += 1
    return count


def onset_errors(references: list[dict], predictions: list[dict], tolerances: Tolerances) -> list[float]:
    return [abs(predictions[p]["start"] - references[r]["start"])
            for r, p in event_pairs(references, predictions, tolerances=tolerances)]


def run_clip(args, audio: Path, output: Path) -> tuple[dict, float, int]:
    command = [str(args.runner), str(audio), str(output), "--backend", args.backend,
               "--pace", args.pace, "--policy", args.policy,
               "--block-frames", str(args.block_frames),
               "--window-seconds", str(args.window_seconds), "--hop-seconds", str(args.hop_seconds),
               "--refine-backend", args.refine_backend, "--refine-mode", args.refine_mode]
    command += ["--callback-replay", str(int(args.callback_replay)),
                "--analysis-delay-ms", str(args.analysis_delay_ms)]
    command += ["--target-ms", str(args.target_ms),
                "--stabilized-target-ms", str(args.stabilized_target_ms)]
    if args.backend == "onnx":
        command += ["--neural-decoder", args.neural_decoder]
    if args.model:
        command += ["--model", str(args.model)]
    started = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout_seconds)
    wall = time.perf_counter() - started
    if result.returncode not in (0, 2) or not output.is_file():
        raise RuntimeError(f"transcriber-live failed for {audio.name}: {result.stderr.strip()}")
    data = json.loads(output.read_text(encoding="utf-8"))
    output.unlink()
    return data, wall, result.returncode


def aggregate(rows: list[dict], key: str) -> dict:
    counts = {metric: [sum(row[key][metric][name] for row in rows) for name in ("tp", "fp", "fn")]
              for metric in ("note_event", "onset_pitch", "frame")}
    summary = {metric: checked_metrics(*values) for metric, values in counts.items()}
    onsets = [value for row in rows for value in row[key + "_onset_errors"]]
    summary["onset_error_seconds"] = distribution(onsets)
    return summary


def evaluate(args) -> dict:
    args.runner = args.runner.resolve()
    build = json.loads(args.build_info.read_text(encoding="utf-8")) if args.build_info else {}
    build = {key: build.get(key) for key in ("compiler", "build_type", "flags", "features")}
    build["live_runner_sha256"] = sha256_file(args.runner)
    source = source_identity()
    for name in ("src/live.cpp", "src/asio_capture.cpp", "src/asio_transport.hpp",
                 "src/audio_ring_buffer.cpp", "include/transcribe/live.hpp",
                 "include/transcribe/asio_capture.hpp", "tests/live_runner.cpp",
                 "python/live_evaluation.py"):
        source["files_sha256"][name] = sha256_file(ROOT / name)
    tolerances = Tolerances(args.onset_tolerance, args.offset_tolerance, args.offset_ratio, args.frame_step)
    rows, failures = [], []
    with tempfile.TemporaryDirectory(prefix="transcriber-live-") as temporary:
        directory = Path(temporary)
        corpus = generate(directory, args.corpus_version)
        for index, clip in enumerate(corpus["clips"] * args.repetitions):
            data, wall, code = run_clip(args, directory / clip["audio"], directory / "live.json")
            refs = clip["notes"]
            live = score_events(refs, data["live_notes"], clip["duration_seconds"], tolerances)
            refinement = data["refinement"]
            refined = (score_events(refs, refinement["notes"], clip["duration_seconds"], tolerances)
                       if refinement and refinement["error"] is None else None)
            metrics = data["metrics"]
            targets = {}
            for name, metric, misses in (
                    ("provisional", "end_to_end_latency", "provisional_target_misses"),
                    ("stabilized", "stabilization_delay", "stabilized_target_misses")):
                targets[name] = ("unmeasured" if not metrics[metric]["count"] else
                                 "missed" if metrics[misses] else "met_for_observed_events")
            row = dict(id=clip["id"], repetition=index // len(corpus["clips"]),
                       duration_seconds=clip["duration_seconds"], exit_code=code,
                       state=data["state"], error=data["error"], process_wall_seconds=wall,
                       live_wall_seconds=data["live_wall_seconds"],
                       first_note_on_wall_seconds=data["first_note_on_wall_seconds"],
                       event_counts=data["event_counts"], metrics=metrics, latency_targets=targets,
                       latency_budget=metrics["latency_budget"],
                       dropped_blocks=data["dropped_blocks"], push_retries=data["push_retries"],
                       document_resyncs=data["document_resyncs"],
                       duplicate_live_notes=duplicates(data["live_notes"]),
                       references=refs, live_notes=data["live_notes"], live=live,
                       live_onset_errors=[], refined=refined, refined_onset_errors=[],
                       refinement={k: v for k, v in (refinement or {}).items() if k != "notes"},
                       refined_notes=refinement["notes"] if refinement else None)
            row["live_onset_errors"] = onset_errors(refs, data["live_notes"], tolerances)
            if refined:
                row["refined_onset_errors"] = onset_errors(refs, refinement["notes"], tolerances)
            checks = {
                "finished": data["state"] == "finished" and code == 0,
                "input_queue_bounded": metrics["input_queue_high_water"] <= metrics["input_queue_capacity"],
                "event_queue_bounded": metrics["event_queue_high_water"] <= metrics["event_queue_capacity"],
                "no_dropped_events": metrics["events_dropped"] == 0,
                "no_duplicate_live_notes": row["duplicate_live_notes"] == 0,
            }
            if args.policy == "lossless":
                checks["lossless"] = metrics["skipped_frames"] == 0 and data["dropped_blocks"] == 0
            if args.require_latency_target:
                checks["provisional_latency_target"] = metrics["provisional_target_misses"] == 0
                checks["stabilized_latency_target"] = metrics["stabilized_target_misses"] == 0
            row["checks"] = checks
            failures.extend(f"{clip['id']}: {name}" for name, ok in checks.items() if not ok)
            rows.append(row)
        corpus.pop("clips")
    summary = {"live": aggregate(rows, "live")}
    if all(row["refined"] for row in rows):
        summary["refined"] = aggregate(rows, "refined")
        drop = summary["live"]["note_event"]["f1"] - summary["refined"]["note_event"]["f1"]
        summary["refined_minus_live_note_event_f1"] = -drop
        if args.max_refinement_f1_drop is not None and drop > args.max_refinement_f1_drop:
            failures.append(f"refined note-event F1 is {drop:.4f} below live")
    elif args.refine_backend != "none":
        failures.append("refinement failed on at least one clip")
    for name in LATENCIES:
        samples = [row["metrics"][name] for row in rows if row["metrics"][name]["count"]]
        summary[name] = {
            "clips": len(samples),
            "count": sum(s["count"] for s in samples),
            "worst_p50_seconds": max((s["p50"] for s in samples), default=None),
            "worst_p95_seconds": max((s["p95"] for s in samples), default=None),
            "worst_p99_seconds": max((s["p99"] for s in samples), default=None),
            "worst_maximum_seconds": max((s["maximum"] for s in samples), default=None),
        }
    summary["input_queue_high_water"] = max(row["metrics"]["input_queue_high_water"] for row in rows)
    summary["event_queue_high_water"] = max(row["metrics"]["event_queue_high_water"] for row in rows)
    summary["skipped_frames"] = sum(row["metrics"]["skipped_frames"] for row in rows)
    summary["events_dropped"] = sum(row["metrics"]["events_dropped"] for row in rows)
    summary["latency_targets"] = {}
    for name, metric, misses, threshold in (
            ("provisional", "end_to_end_latency", "provisional_target_misses", args.target_ms),
            ("stabilized", "stabilization_delay", "stabilized_target_misses", args.stabilized_target_ms)):
        count = summary[metric]["count"]
        missed = sum(row["metrics"][misses] for row in rows)
        summary["latency_targets"][name] = dict(
            target_ms=threshold, samples=count, missed_samples=missed,
            unmeasured_clips=sum(row["latency_targets"][name] == "unmeasured" for row in rows),
            status="unmeasured" if not count else "missed" if missed else "met_for_observed_events")
        if args.require_latency_target and not count:
            failures.append(f"{name} latency target could not be measured")
    for key in ("blocks_refused", "gap_frames", "windows_coalesced"):
        summary[key] = sum(row["metrics"][key] for row in rows)
    summary["overloaded_clips"] = sum(bool(row["metrics"]["overloaded"]) for row in rows)
    summary["corpus_audio_seconds"] = sum(row["duration_seconds"] for row in rows)
    summary["live_wall_seconds"] = sum(row["live_wall_seconds"] for row in rows)
    for row in rows:
        row.pop("live_onset_errors")
        row.pop("refined_onset_errors")
    if build["live_runner_sha256"] != sha256_file(args.runner) or any(
            digest != sha256_file(ROOT / name) for name, digest in source["files_sha256"].items()):
        raise RuntimeError("Source or runner changed during live measurements")
    return dict(
        schema_version=SCHEMA_VERSION, generated_at_utc=datetime.now(timezone.utc).isoformat(),
        corpus=corpus, build=build, source=source,
        environment={"python": platform.python_version(), "platform": platform.platform(),
                     "machine": platform.machine(), "logical_cpus": os.cpu_count()},
        configuration=dict(backend=args.backend, refine_backend=args.refine_backend, refine_mode=args.refine_mode,
                           model_sha256=sha256_file(args.model) if args.model else None,
                           pace=args.pace, policy=args.policy, block_frames=args.block_frames,
                           callback_replay=args.callback_replay, analysis_delay_ms=args.analysis_delay_ms,
                           repetitions=args.repetitions, corpus_version=args.corpus_version,
                           provisional_target_ms=args.target_ms, stabilized_target_ms=args.stabilized_target_ms,
                           neural_decoder=args.neural_decoder if args.backend == "onnx" else None,
                           window_seconds=args.window_seconds, hop_seconds=args.hop_seconds,
                           live_mode="incremental_yin" if args.backend == "yin" else "overlapping_window"),
        tolerances=asdict(tolerances),
        latency_protocol={
            "capture_latency": "block capture timestamp -> ingest worker",
            "analysis_latency": "wall time of one analysis step",
            "end_to_end_latency": "onset block capture -> provisional NoteOn queued",
            "stabilization_delay": "onset block capture -> NoteStabilized queued",
            "algorithmic_latency": "recording-time distance from onset to analysed audio end",
            "analysis_queue_latency": "newest evidence block ingestion -> analysis step start",
            "confirmation_latency": "first NoteOn -> NoteStabilized (not capture-relative)",
            "publication_queue_latency": "event queued -> consumer poll",
            "callback_execution": "ASIO transport receive() wall time in replay; excludes driver and OS",
            "hardware_capture_latency": None,
            "ui_render_latency": None,
            "note": "with --pace none, no capture timestamp is synthesized; capture-relative "
                    "latency samples are unavailable. Use --pace realtime for live-like timing. "
                    "Percentiles use nearest rank over the most recent <=4096 samples per clip; "
                    "aggregate values are worst per-clip percentiles, not pooled quantiles.",
        },
        summary=summary, failures=failures, clips=rows,
    )


def human_report(report: dict) -> str:
    s, c = report["summary"], report["configuration"]
    lines = [f"Live evaluation: backend={c['backend']} ({c['live_mode']}), refine={c['refine_backend']} ({c.get('refine_mode', 'auto')}), "
             f"pace={c['pace']}, policy={c['policy']}",
             f"Corpus: {report['corpus']['name']} v{report['corpus']['version']} "
             f"({s['corpus_audio_seconds']:.1f} s audio, {s['live_wall_seconds']:.2f} s live wall)"]
    for name in ("live", "refined"):
        if name in s:
            m = s[name]
            lines.append(f"{name:8s} note-event P/R/F1 {m['note_event']['precision']:.3f}/"
                         f"{m['note_event']['recall']:.3f}/{m['note_event']['f1']:.3f}  "
                         f"onset-pitch F1 {m['onset_pitch']['f1']:.3f}  frame F1 {m['frame']['f1']:.3f}")
    for name in LATENCIES:
        value = s[name]
        if value["count"]:
            scale, unit = (1_000_000, "us") if name == "callback_execution" else (1000, "ms")
            lines.append(f"{name}: n={value['count']} worst clip p50 "
                         f"{value['worst_p50_seconds'] * scale:.1f} {unit}, "
                         f"p95 {value['worst_p95_seconds'] * scale:.1f} {unit}, "
                         f"p99 {value['worst_p99_seconds'] * scale:.1f} {unit}, "
                         f"max {value['worst_maximum_seconds'] * scale:.1f} {unit}")
    lines.append(f"Queues: input high-water {s['input_queue_high_water']}, event high-water "
                 f"{s['event_queue_high_water']}, skipped frames {s['skipped_frames']}, "
                 f"dropped events {s['events_dropped']}")
    lines.append(f"Pressure: overloaded clips {s['overloaded_clips']}, coalesced windows "
                 f"{s['windows_coalesced']}, refused blocks {s['blocks_refused']}, "
                 f"missing input frames {s['gap_frames']}")
    lines.append("Failures: " + ("; ".join(report["failures"]) if report["failures"] else "none"))
    return "\n".join(lines) + "\n"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--build-info", type=Path)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-text", type=Path)
    parser.add_argument("--backend", choices=("yin", "spectral", "onnx"), default="yin")
    parser.add_argument("--refine-backend", choices=("none", "yin", "spectral", "onnx"), default="spectral")
    parser.add_argument("--refine-mode", choices=("auto", "full", "windowed"), default="auto",
                        help="auto: overlapping windows for onnx, one full-context pass otherwise")
    parser.add_argument("--model", type=Path)
    parser.add_argument("--pace", choices=("none", "realtime"), default="none")
    parser.add_argument("--policy", choices=("lossless", "skip"), default="lossless")
    parser.add_argument("--callback-replay", action="store_true",
                        help="exercise the actual ASIO transport with file blocks, without ASIO hardware")
    parser.add_argument("--analysis-delay-ms", type=float, default=0,
                        help="inject worker sleep for overload experiments, not a production setting")
    parser.add_argument("--corpus-version", type=int, choices=(1, 2, 3), default=1)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--neural-decoder", choices=("speed", "quality"), default="speed")
    parser.add_argument("--target-ms", type=float, default=200)
    parser.add_argument("--stabilized-target-ms", type=float, default=300)
    parser.add_argument("--require-latency-target", action="store_true")
    parser.add_argument("--block-frames", type=int, default=256)
    parser.add_argument("--window-seconds", type=float, default=2.0)
    parser.add_argument("--hop-seconds", type=float, default=.25)
    parser.add_argument("--onset-tolerance", type=float, default=.05)
    parser.add_argument("--offset-tolerance", type=float, default=.05)
    parser.add_argument("--offset-ratio", type=float, default=.2)
    parser.add_argument("--frame-step", type=float, default=.01)
    parser.add_argument("--timeout-seconds", type=float, default=300)
    parser.add_argument("--max-refinement-f1-drop", type=float)
    parser.add_argument("--check", action="store_true", help="exit nonzero when any gate fails")
    args = parser.parse_args()
    if not 0 < args.target_ms <= args.stabilized_target_ms <= 60000 or args.repetitions < 1:
        parser.error("Latency targets require 0 < provisional <= stabilized <=60000 ms; repetitions >=1")
    if args.require_latency_target and args.pace != "realtime":
        parser.error("Latency target enforcement requires real-time paced input")
    if args.callback_replay and args.policy != "skip":
        parser.error("--callback-replay requires --policy skip")
    if not 0 <= args.analysis_delay_ms <= 1000:
        parser.error("--analysis-delay-ms must be finite and 0-1000")
    if (args.backend == "onnx" or args.refine_backend == "onnx") and not args.model:
        parser.error("onnx requires --model")
    if args.model:
        args.model = args.model.resolve()
    return args


def main() -> int:
    args = parse_args()
    try:
        report = evaluate(args)
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        print(f"live_evaluation: {error}", file=sys.stderr)
        return 1
    write_json(args.output_json, report)
    text = human_report(report)
    if args.output_text:
        args.output_text.parent.mkdir(parents=True, exist_ok=True)
        args.output_text.write_text(text, encoding="utf-8")
    print(text, end="")
    return 1 if args.check and report["failures"] else 0


if __name__ == "__main__":
    sys.exit(main())
