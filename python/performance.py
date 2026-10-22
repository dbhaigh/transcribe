#!/usr/bin/env python3
"""Cold/warm Release DSP profiling with exact-output and labeled-quality comparisons."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import wave
import math
import struct
import os
import time

from evaluate import source_identity, write_json
from evaluate_guitarset import sha256_file
from evaluation_environment import host_identity
from evaluation_metrics import score_events
from synthetic_corpus import generate, note, render, SAMPLE_RATE


def run_long_recordings(args):
    """Exercise the existing runner without materializing multi-hour fixture samples."""
    metadata = json.loads(args.build_info.read_text())
    if metadata["build_type"] != "Release":
        raise ValueError("Performance measurements require a Release build")
    if Path(metadata["runner"]).parent.resolve() != args.runner.resolve().parent:
        raise ValueError("Runner and build metadata directories must match")
    source = long_source_identity()
    binary_hash = sha256_file(args.runner)
    model_hash = sha256_file(args.onnx_model) if args.onnx_model else None
    rows = []
    rate = 8000
    silence = b"\0\0" * rate
    tone = b"".join(struct.pack("<h", round(10000 * math.sin(2 * math.pi * 220 * i / rate)))
                    for i in range(rate))
    durations = [("short", 6), ("long", args.long_seconds)]
    if args.hours:
        durations.append(("multi_hour", args.hours * 3600))
    environment = dict(os.environ, TRANSCRIBE_PERFORMANCE_WARMUPS=str(args.warmups))
    with tempfile.TemporaryDirectory(prefix="transcriber-long-") as temporary:
        directory = Path(temporary)
        for identifier, seconds in durations:
            path = directory / (identifier + ".wav")
            reference = [dict(midi=57, start=float(s + 1), duration=float(min(3, seconds - s - 1)))
                         for s in range(0, seconds, 60) if s + 1 < seconds]
            with wave.open(str(path), "wb") as output:
                output.setparams((1, 2, rate, 0, "NONE", "not compressed"))
                for second in range(seconds):
                    output.writeframesraw(tone if second % 60 in (1, 2, 3) else silence)
            digest = sha256_file(path)
            for backend in ("yin", "spectral", "onnx") if args.onnx_model else ("yin", "spectral"):
                for mode in ("bounded",) if identifier == "multi_hour" else ("model", "bounded", "cached"):
                    target = directory / "run.json"
                    command = [str(args.runner.resolve()), str(path), str(target), backend, mode,
                               "serial" if args.serial else "parallel", str(args.repetitions),
                               str(args.chunk_seconds),
                               str(args.onnx_model.resolve()) if backend == "onnx" else "-",
                               args.provider if backend == "onnx" else "cpu"]
                    process_start = time.perf_counter()
                    subprocess.run(command, check=True, timeout=max(600, seconds * .5),
                                   env=environment)
                    process_wall_seconds = time.perf_counter() - process_start
                    result = json.loads(target.read_text())
                    times = [r["wall_seconds"] for r in result["runs"]]
                    median = statistics.median(times)
                    timing_summary = summarize_timings(times)
                    rows.append(dict(id=identifier, backend=backend, mode=mode,
                                     audio_sha256=digest, **result,
                                     process_wall_seconds=process_wall_seconds,
                                     metrics=score_events(reference, result["notes"], seconds),
                                     timing_percentiles_seconds=timing_summary,
                                     median_seconds=median,
                                     p95_seconds=timing_summary["p95"],
                                     p99_seconds=timing_summary["p99"],
                                     realtime_factor=median / seconds,
                                     audio_seconds_per_second=seconds / median if median else None))
    for row in rows:
        full = next((r for r in rows if r["id"] == row["id"] and
                     r["backend"] == row["backend"] and r["mode"] == "model"), None)
        row["full_buffer_notes_identical"] = row["notes"] == full["notes"] if full else None
    if source["files_sha256"] != long_source_identity()["files_sha256"] or binary_hash != sha256_file(args.runner):
        raise ValueError("Source or runner changed during performance measurements")
    if model_hash != (sha256_file(args.onnx_model) if args.onnx_model else None):
        raise ValueError("Model changed during performance measurements")
    report = dict(schema_version=2, measurement_protocol_version=2,
                  build=metadata, binary_sha256=binary_hash,
                  generated_at_utc=datetime.now(timezone.utc).isoformat(), source=source,
                  environment=dict(host_identity(), python=platform.python_version()),
                  model_sha256=model_hash,
                  configuration=dict(chunk_seconds=args.chunk_seconds, overlap_seconds=1,
                                     warmups=args.warmups, repetitions=args.repetitions,
                                     fixture="8kHz PCM16; A3 seconds 1-4 of every minute",
                                     hours=args.hours, long_seconds=args.long_seconds,
                                     provider=args.provider, model_sha256=model_hash, serial=args.serial),
                  protocol="Separate process per row; session loaded once; warmups excluded. "
                           "Startup includes full decode only for model/cached; bounded timing includes decode. "
                           "RSS is process-lifetime high-water, including warmups. Model load separately timed. "
                           "Multi-hour uses bounded windows only to avoid deliberately exhausting memory. "
                           "Cached mode is opt-in with a 16 MiB payload budget, no evidence/timing callbacks. "
                           "Cold inference is recorded separately; warmups are excluded from measured runs. "
                           "p50/p95/p99 use linearly interpolated empirical quantiles. Tail percentiles "
                           "from fewer than 100 samples are descriptive, not statistically stable. "
                           "Process wall time includes runner startup; runner startup includes decode/model load. "
                           "RSS is a process-lifetime high-water mark, including warmups. "
                           "Multi-hour uses bounded windows only to avoid deliberately exhausting memory. "
                           "Cached mode is opt-in with a 16 MiB payload budget, no evidence/timing callbacks.",
                  clips=rows)
    if args.compare:
        compare_reports(json.loads(args.compare.read_text()), report)
    write_json(args.output, report)
    args.output.with_suffix(".md").write_text(long_report_markdown(report), encoding="utf-8")
    if args.compare and not report["all_notes_identical"]:
        raise ValueError("Predictions changed; inspect per-input comparison report")


def long_source_identity():
    source = source_identity()
    root = Path(__file__).resolve().parents[1]
    for name in ("python/performance.py", "tests/performance_runner.cpp",
                 "src/inference_stream.cpp", "src/inference.cpp", "src/audio_file.cpp",
                 "include/transcribe/inference.hpp"):
        source["files_sha256"][name] = sha256_file(root / name)
    return source


def long_report_markdown(report):
    lines = ["# Bounded pipeline measurements", "", report["protocol"], "",
             "| Input | Backend | Mode | Cold s | p50 s | p95 s | p99 s | RTF | Peak MiB | Exact full notes |",
             "|---|---|---|---:|---:|---:|---:|---:|---:|---|"]
    for row in report["clips"]:
        lines.append(f"| {row['id']} | {row['backend']} | {row['mode']} | "
                     f"{row['cold_run']['wall_seconds']:.4f} | "
                     f"{row['timing_percentiles_seconds']['p50']:.4f} | "
                     f"{row['p95_seconds']:.4f} | {row['p99_seconds']:.4f} | "
                     f"{row['realtime_factor']:.5f} | "
                     f"{max(r['peak_resident_bytes'] for r in row['runs']) / 1048576:.2f} | "
                     f"{row['full_buffer_notes_identical']} |")
    lines += ["", "## Per-input quality (not an aggregate accuracy claim)", "",
              "| Input | Backend | Mode | Frame F1 | Onset+pitch F1 | Event P | Event R | Event F1 | Extra fragments | Merged references |",
              "|---|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for row in report["clips"]:
        metrics = row["metrics"]
        event = metrics["note_event"]
        lines.append(f"| {row['id']} | {row['backend']} | {row['mode']} | "
                     f"{metrics['frame']['f1']:.4f} | {metrics['onset_pitch']['f1']:.4f} | "
                     f"{event['precision']:.4f} | {event['recall']:.4f} | {event['f1']:.4f} | "
                     f"{metrics['note_fragmentation']['extra_predicted_fragments']} | "
                     f"{metrics['note_merging']['extra_reference_notes']} |")
    lines += ["", "Onset/offset distributions, false positives/minute and predictions are in the JSON.",
              "Windowed outputs may differ; no aggregate accuracy score or equivalence guarantee."]
    return "\n".join(lines) + "\n"


def percentile(values, quantile):
    if not values:
        raise ValueError("Cannot calculate a percentile without samples")
    ordered = sorted(values)
    position = quantile * (len(ordered) - 1)
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def summarize_timings(values):
    return {
        "sample_count": len(values),
        "p50": percentile(values, .50),
        "p95": percentile(values, .95),
        "p99": percentile(values, .99),
        "stability": "descriptive" if len(values) < 100 else "empirical",
    }


def compare_reports(before, current):
    if before.get("measurement_protocol_version") != current.get("measurement_protocol_version"):
        raise ValueError("Performance measurement protocols differ")
    if before["configuration"] != current["configuration"]:
        raise ValueError("Comparison configuration differs")
    old = {(c["id"], c["backend"], c["mode"]): c for c in before["clips"]}
    if len(old) != len(current["clips"]):
        raise ValueError("Comparison corpus differs")
    same_hardware = hardware_identity(before["environment"]) == \
        hardware_identity(current["environment"])
    same_build_configuration = build_configuration_identity(before["build"]) == \
        build_configuration_identity(current["build"])
    for row in current["clips"]:
        previous = old[(row["id"], row["backend"], row["mode"])]
        if row["audio_sha256"] != previous["audio_sha256"]:
            raise ValueError("Comparison audio differs")
        cold = row["cold_run"]["wall_seconds"]
        warm = statistics.median(r["wall_seconds"] for r in row["runs"])
        previous_cold = previous["cold_run"]["wall_seconds"]
        previous_warm = statistics.median(r["wall_seconds"] for r in previous["runs"])
        row["comparison"] = dict(
            exact_notes=row["notes"] == previous["notes"],
            event_f1_delta=row["metrics"]["note_event"]["f1"] -
            previous["metrics"]["note_event"]["f1"] if row["metrics"] else None,
            cold_speedup=previous_cold / cold if cold else None,
            warm_speedup=previous_warm / warm if warm else None,
            same_hardware=same_hardware,
            same_build_configuration=same_build_configuration,
            timing_comparable=same_hardware and same_build_configuration)
    current["all_notes_identical"] = all(c["comparison"]["exact_notes"] for c in current["clips"])
    current["comparison_context"] = {
        "same_hardware": same_hardware,
        "same_build_configuration": same_build_configuration,
        "timing_comparable": same_hardware and same_build_configuration,
    }


def hardware_identity(environment):
    return tuple(environment.get(key) for key in
                 ("platform", "machine", "cpu_model", "logical_cpus", "gpu_inventory"))


def build_configuration_identity(build):
    compiler = build.get("compiler", {})
    return (
        build.get("build_type"),
        compiler.get("id"),
        compiler.get("version"),
        build.get("flags"),
        build.get("features"),
        build.get("generator"),
    )


def run(args):
    metadata = json.loads(args.build_info.read_text())
    if Path(metadata["runner"]).parent.resolve() != args.runner.resolve().parent:
        raise ValueError("Runner and build metadata directories must match")
    if metadata["build_type"] != "Release":
        raise ValueError("Performance measurements require a Release build")
    rows = []
    environment = dict(os.environ, TRANSCRIBE_PERFORMANCE_WARMUPS=str(args.warmups))
    with tempfile.TemporaryDirectory(prefix="transcriber-performance-") as temporary:
        directory = Path(temporary)
        corpus = generate(directory)
        clips = corpus["clips"]
        for identifier, seconds, notes in (
            ("low_notes", 6, [note(36, .2, 2.6), note(40, 3, 5.6)]),
            ("dense_chord", 6, [note(p, .2, 5.5, 40) for p in (40, 47, 52, 55, 59, 64)]),
            ("long_silence", 90, []),
            ("long_chord", 90, [note(p, .2, 89.5, 40) for p in (40, 47, 52, 55, 59, 64)]),
        ):
            spec = dict(duration_seconds=seconds, notes=notes, effects={})
            path = directory / (identifier + ".wav")
            with wave.open(str(path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(SAMPLE_RATE)
                output.writeframes(render(spec))
            clips.append(dict(id=identifier, audio=path.name, notes=notes, sha256=sha256_file(path)))
        if args.recording:
            clips.append(dict(id="user_recording", path=args.recording.resolve(),
                              notes=None, sha256=sha256_file(args.recording)))
        for clip in clips:
            path = clip.get("path", directory / clip.get("audio", ""))
            with wave.open(str(path), "rb") as audio:
                rate = audio.getframerate()
                channels = audio.getnchannels()
            for backend in ("yin", "spectral", "onnx") if args.onnx_model else ("yin", "spectral"):
                mode = "model" if args.model_interface else (
                    "streaming" if clip["id"].startswith("long_") else "full")
                output = directory / "measurement.json"
                command = [str(args.runner.resolve()), str(path), str(output),
                           backend, mode, "serial" if args.serial else "parallel",
                           str(args.repetitions), "15"]
                if args.model_interface:
                    command.extend([str(args.onnx_model.resolve()) if backend == "onnx" else "-",
                                    args.provider if backend == "onnx" else "cpu"])
                process_start = time.perf_counter()
                subprocess.run(command, check=True, timeout=600, env=environment)
                process_wall_seconds = time.perf_counter() - process_start
                result = json.loads(output.read_text())
                if args.verify_legacy:
                    legacy_output = directory / "legacy.json"
                    legacy_command = [metadata["runner"], str(path), str(legacy_output),
                                      "--backend", backend, "--mode", "full"]
                    if args.serial:
                        legacy_command.append("--serial")
                    if backend == "onnx":
                        legacy_command.extend(["--model", str(args.onnx_model.resolve()),
                                               "--provider", args.provider])
                    verified = subprocess.run(legacy_command, capture_output=True, text=True, timeout=600)
                    if verified.returncode:
                        raise ValueError(f"Legacy verification failed: {verified.stderr}{verified.stdout}")
                    legacy = json.loads(legacy_output.read_text())
                    if legacy["notes"] != result["notes"]:
                        raise ValueError(f"Model interface changed legacy notes: {clip['id']}/{backend}")
                    result["legacy_notes_identical"] = True
                runs = result["runs"]
                timing_percentiles = summarize_timings(
                    [run["wall_seconds"] for run in runs])
                median_seconds = timing_percentiles["p50"]
                audio_seconds = result["cold_run"]["audio_seconds"]
                rows.append(dict(id=clip["id"], backend=backend, mode=mode,
                                 sample_rate=rate, channels=channels,
                                 audio_sha256=clip["sha256"], **result,
                                 process_wall_seconds=process_wall_seconds,
                                 timing_percentiles_seconds=timing_percentiles,
                                 median_seconds=median_seconds,
                                 p50_seconds=timing_percentiles["p50"],
                                 p95_seconds=timing_percentiles["p95"],
                                 p99_seconds=timing_percentiles["p99"],
                                 realtime_factor=median_seconds / audio_seconds
                                 if audio_seconds else None,
                                 audio_seconds_per_second=audio_seconds / median_seconds
                                 if median_seconds else None,
                                 peak_resident_bytes=max(
                                     sample["peak_resident_bytes"]
                                     for sample in [result["cold_run"], *runs]),
                                 metrics=score_events(clip["notes"], result["notes"],
                                                      audio_seconds)
                                 if clip["notes"] is not None else None))
    source = source_identity()
    for name in ("python/performance.py", "tests/performance_runner.cpp"):
        source["files_sha256"][name] = sha256_file(Path(__file__).resolve().parents[1] / name)
    report = dict(schema_version=2, measurement_protocol_version=2,
                  generated_at_utc=datetime.now(timezone.utc).isoformat(),
                  build=metadata, binary_sha256=sha256_file(args.runner),
                  source=source,
                  environment=dict(host_identity(), python=platform.python_version()),
                  configuration=dict(repetitions=args.repetitions, serial=args.serial,
                                     warmups=args.warmups, chunk_seconds=15, overlap_seconds=1,
                                     minimum_confidence=.25,
                                     spectral_threshold=.3, maximum_pitches=6, provider="cpu",
                                     model_identifier="builtin:yin/spectral"),
                  protocol="The explicit cold inference is recorded before configured warmups; "
                           "warmups are excluded from measured runs. OS caches are uncontrolled. "
                           "Full-buffer decode is excluded from inference timing; streaming decode is included. "
                           "Startup includes decode/session initialization and is distinct from parent-measured "
                           "process wall time. Peak RSS is process high-water mark, including warmups. "
                           "CPU is process kernel+user time / wall time. Percentiles are linearly interpolated; "
                           "p95/p99 from fewer than 100 samples are descriptive, not statistically stable.",
                  clips=rows)
    if args.model_interface:
        report["configuration"].update(
            model_interface_version=1, provider=args.provider, batch_size=1,
            onnx_model_sha256=sha256_file(args.onnx_model) if args.onnx_model else None,
            onnx_contract="basic-pitch-v1" if args.onnx_model else None)
        report["configuration"]["legacy_verification"] = args.verify_legacy
        if args.onnx_model:
            report["configuration"]["model_identifier"] += " + onnx:" + sha256_file(args.onnx_model)
        report["protocol"] += (
            " Model instances load once before first inference; load time reported separately. "
            "The separate cold inference occurs after load; later measured runs reuse the session. "
            "Each Basic Pitch invocation reuses one input tensor wrapper across its windows. "
            "Empty ONNX input is skipped.")
    if args.compare:
        compare_reports(json.loads(args.compare.read_text()), report)
    write_json(args.output, report)
    for backend in ("yin", "spectral", "onnx") if args.onnx_model else ("yin", "spectral"):
        subset = [r for r in rows if r["backend"] == backend]
        print(backend, "cold total", sum(r["cold_run"]["wall_seconds"] for r in subset),
              "warm median total", sum(statistics.median(x["wall_seconds"] for x in r["runs"])
                                       for r in subset))
    if args.compare and not report["all_notes_identical"]:
        raise ValueError("Predictions changed; inspect comparison report")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--build-info", required=True, type=Path)
    parser.add_argument("--recording", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--serial", action="store_true")
    parser.add_argument("--model-interface", action="store_true")
    parser.add_argument("--verify-legacy", action="store_true")
    parser.add_argument("--onnx-model", type=Path)
    parser.add_argument("--provider", choices=("cpu", "cuda", "directml", "rocm"), default="cpu")
    parser.add_argument("--repetitions", type=int, default=4)
    parser.add_argument("--long-recordings", action="store_true")
    parser.add_argument("--long-seconds", type=int, default=120)
    parser.add_argument("--hours", type=int, default=0)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--chunk-seconds", type=float, default=8)
    args = parser.parse_args()
    if not 2 <= args.repetitions <= 100:
        parser.error("Repetitions must be 2-100")
    if args.long_seconds < 6 or args.long_seconds > 3600 or not 0 <= args.hours <= 4:
        parser.error("Long duration must be 6-3600 seconds; hours must be 0-4")
    if not 0 <= args.warmups <= 100 or not 2 <= args.chunk_seconds <= 30:
        parser.error("Warmups must be 0-100 and chunks 2-30 seconds")
    if (args.onnx_model or args.provider != "cpu") and not args.model_interface:
        parser.error("--onnx-model/--provider require --model-interface")
    if args.provider != "cpu" and not args.onnx_model:
        parser.error("Accelerator benchmark requires --onnx-model; built-ins use CPU")
    if args.verify_legacy and not args.model_interface:
        parser.error("--verify-legacy requires --model-interface")
    if args.long_recordings and (args.recording or args.verify_legacy):
        parser.error("--long-recordings does not support --recording/--verify-legacy")
    try:
        if args.long_recordings:
            run_long_recordings(args)
        else:
            run(args)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Performance evaluation failed: {error}\n")
