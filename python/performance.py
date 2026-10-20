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

from evaluate import source_identity, write_json
from evaluate_guitarset import sha256_file
from evaluation_metrics import score_events
from synthetic_corpus import generate, note, render, SAMPLE_RATE


def compare_reports(before, current):
    if before["configuration"] != current["configuration"]:
        raise ValueError("Comparison configuration differs")
    old = {(c["id"], c["backend"], c["mode"]): c for c in before["clips"]}
    if len(old) != len(current["clips"]):
        raise ValueError("Comparison corpus differs")
    for row in current["clips"]:
        previous = old[(row["id"], row["backend"], row["mode"])]
        if row["audio_sha256"] != previous["audio_sha256"]:
            raise ValueError("Comparison audio differs")
        cold = row["runs"][0]["wall_seconds"]
        warm = statistics.median(r["wall_seconds"] for r in row["runs"][1:])
        row["comparison"] = dict(
            exact_notes=row["notes"] == previous["notes"],
            event_f1_delta=row["metrics"]["note_event"]["f1"] -
            previous["metrics"]["note_event"]["f1"] if row["metrics"] else None,
            cold_speedup=previous["runs"][0]["wall_seconds"] / cold if cold else None,
            warm_speedup=statistics.median(r["wall_seconds"] for r in previous["runs"][1:]) /
            warm if warm else None)
    current["all_notes_identical"] = all(c["comparison"]["exact_notes"] for c in current["clips"])


def run(args):
    metadata = json.loads(args.build_info.read_text())
    if Path(metadata["runner"]).parent.resolve() != args.runner.resolve().parent:
        raise ValueError("Runner and build metadata directories must match")
    if metadata["build_type"] != "Release":
        raise ValueError("Performance measurements require a Release build")
    rows = []
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
                subprocess.run(command, check=True, timeout=600)
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
                rows.append(dict(id=clip["id"], backend=backend, mode=mode,
                                 sample_rate=rate, channels=channels,
                                 audio_sha256=clip["sha256"], **result,
                                 metrics=score_events(clip["notes"], result["notes"],
                                                      runs[0]["audio_seconds"])
                                 if clip["notes"] is not None else None))
    source = source_identity()
    for name in ("python/performance.py", "tests/performance_runner.cpp"):
        source["files_sha256"][name] = sha256_file(Path(__file__).resolve().parents[1] / name)
    report = dict(schema_version=1, generated_at_utc=datetime.now(timezone.utc).isoformat(),
                  build=metadata, binary_sha256=sha256_file(args.runner),
                  source=source, environment=dict(platform=platform.platform(),
                                                 processor=platform.processor(),
                                                 python=platform.python_version()),
                  configuration=dict(repetitions=args.repetitions, serial=args.serial,
                                     chunk_seconds=15, overlap_seconds=1, minimum_confidence=.25,
                                     spectral_threshold=.3, maximum_pitches=6, provider="cpu",
                                     model_identifier="builtin:yin/spectral"),
                  protocol="First in-process invocation cold; remaining warm; OS caches uncontrolled. "
                           "Full-buffer decode excluded; streaming decode included. Peak RSS is "
                           "process high-water mark. CPU is process kernel+user time / wall time.",
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
            "First inference cold after load; later runs reuse the session. "
            "Each Basic Pitch invocation reuses one input tensor wrapper across its windows. "
            "Empty ONNX input is skipped. No automatic warmup; measured runs themselves warm the session.")
    if args.compare:
        compare_reports(json.loads(args.compare.read_text()), report)
    write_json(args.output, report)
    for backend in ("yin", "spectral", "onnx") if args.onnx_model else ("yin", "spectral"):
        subset = [r for r in rows if r["backend"] == backend]
        print(backend, "cold total", sum(r["runs"][0]["wall_seconds"] for r in subset),
              "warm total", sum(statistics.median(x["wall_seconds"] for x in r["runs"][1:])
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
    args = parser.parse_args()
    if not 2 <= args.repetitions <= 100:
        parser.error("Repetitions must be 2-100")
    if (args.onnx_model or args.provider != "cpu") and not args.model_interface:
        parser.error("--onnx-model/--provider require --model-interface")
    if args.provider != "cpu" and not args.onnx_model:
        parser.error("Accelerator benchmark requires --onnx-model; built-ins use CPU")
    if args.verify_legacy and not args.model_interface:
        parser.error("--verify-legacy requires --model-interface")
    try:
        run(args)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Performance evaluation failed: {error}\n")
