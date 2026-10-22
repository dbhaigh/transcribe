#!/usr/bin/env python3
"""Reproducible synthetic/real-audio quality and process-cold performance evaluation."""

from __future__ import annotations

import argparse
import copy
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
    apply_event_calibration,
)
from synthetic_corpus import generate, generate_mixtures
from separation_evaluation import paired_corpus, paired_comparisons
from evaluate_guitarset import (
    ANNOTATION_MD5, AUDIO_MD5, DATASET_VERSION, guitarset_dataset_split,
    guitarset_player_id, guitarset_split_policy, md5_file, read_reference_events, sha256_file,
)
from evaluation_environment import host_identity


SCHEMA_VERSION = 2
ROOT = Path(__file__).resolve().parents[1]


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
                    encoding="utf-8")


def source_identity() -> dict:
    files = ["python/evaluate.py", "python/evaluation_metrics.py", "python/synthetic_corpus.py",
             "python/quality_release_gate.py",
             "python/evaluate_guitarset.py", "python/evaluation_environment.py",
             "tests/evaluation_runner.cpp", "CMakeLists.txt",
             "tests/evaluation_build.json.in", "src/transcriber.cpp", "src/audio_file.cpp",
             "src/audio_ring_buffer.cpp", "include/transcribe/transcriber.hpp",
             "include/transcribe/audio.hpp", "src/decoding.cpp", "include/transcribe/decoding.hpp",
             "python/separation_evaluation.py"]
    files.extend(["include/transcribe/inference.hpp", "src/inference.cpp", "src/onnx_backend.hpp"])
    files.extend(["include/transcribe/polyphonic.hpp", "src/polyphonic.cpp"])
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
        selected_split = getattr(args, "dataset_split", "all")
        if selected_split not in ("all", "train", "validation", "test"):
            raise ValueError(f"Unknown GuitarSet dataset split: {selected_split}")
        if selected_split != "all":
            selected = []
            for name in jams:
                split = guitarset_dataset_split(PurePosixPath(name).stem)
                if split is None:
                    raise ValueError(f"No performer split assignment for {name}")
                if split == selected_split:
                    selected.append(name)
            jams = selected
            if not jams:
                raise ValueError(f"GuitarSet split has no recordings: {selected_split}")
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
            split = guitarset_dataset_split(track)
            if split is None and selected_split != "all":
                raise ValueError(f"No performer split assignment for {track}")
            scenarios = ["guitarset"]
            if track.endswith("_comp"):
                scenarios.append("comp")
            elif track.endswith("_solo"):
                scenarios.append("solo")
            if split is not None:
                scenarios.append(split)
            clips.append(dict(id=track, audio=path.name, sample_rate=rate,
                              duration_seconds=duration, notes=notes, sha256=sha256_file(path),
                              dataset_split=split or "unassigned",
                              performer_id=guitarset_player_id(track), scenarios=scenarios))
    corpus = dict(name="GuitarSet", version=DATASET_VERSION, seed=args.seed,
                  provenance={"doi": "10.5281/zenodo.3371780", "license": "CC BY 4.0",
                              "attribution": "Qingyang Xi et al., ISMIR 2018",
                              "archive_md5": actual, "possible_model_training_overlap": True},
                  dataset_split_policy=guitarset_split_policy(),
                  selected_split=selected_split,
                  clips=clips)
    corpus["sha256"] = hashlib.sha256(
        json.dumps(corpus, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return corpus


def invoke(args, audio: Path, output: Path) -> tuple[dict, float]:
    command = [str(args.runner), str(audio), str(output), "--backend", args.backend,
               "--mode", args.mode, "--minimum-confidence", str(args.minimum_confidence),
               "--chunk-seconds", str(args.chunk_seconds),
               "--overlap-seconds", str(args.overlap_seconds), "--latency-ms", str(args.latency_ms)]
    if getattr(args, "instrument", "none") != "none":
        command.extend(["--instrument", args.instrument])
    if args.fast:
        command.append("--fast")
    if args.serial:
        command.append("--serial")
    if args.polyphonic_decoder != "legacy":
        command.extend(["--polyphonic-decoder", args.polyphonic_decoder])
    if args.model_objective:
        command.extend(["--model-objective", args.model_objective])
    if args.model_interface:
        command.extend(["--model-interface", "--model-contract", args.model_contract])
        command.extend(["--session-warmups", str(getattr(args, "session_warmups", 0)),
                        "--session-repetitions", str(getattr(args, "session_repetitions", 1))])
    if args.temporal:
        command.extend(["--temporal", "--audio-source-id", "sha256:" + sha256_file(audio),
                        "--activation-on", str(args.activation_on),
                        "--activation-off", str(args.activation_off),
                        "--maximum-gap", str(args.maximum_gap),
                        "--minimum-duration", str(args.minimum_duration),
                        "--energy-floor", str(args.energy_floor),
                        "--pitch-stability", str(args.pitch_stability),
                        "--pitch-tolerance", str(args.pitch_tolerance),
                        "--onset-threshold", str(args.onset_threshold),
                        "--onset-spacing", str(args.onset_spacing),
                        "--onset-energy-rise", str(args.onset_energy_rise),
                        "--onset-state-drop", str(args.onset_state_drop),
                        "--model-onsets", str(int(not args.no_model_onsets)),
                        "--audio-refinement", str(int(not args.no_audio_refinement))])
        if args.correct_isolated_octaves:
            command.append("--correct-isolated-octaves")
    if args.model:
        command.extend(["--model", str(args.model), "--provider", args.provider])
        if getattr(args, "neural_decoder", "legacy") != "legacy":
            command.extend(["--neural-decoder", args.neural_decoder])
        if args.temporal:
            command.extend(["--model-identifier",
                            args.model_identifier or "onnx:" + sha256_file(args.model)])
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
    memory = data.get("peak_resident_bytes")
    if memory is not None and (
        isinstance(memory, bool) or not isinstance(memory, int) or memory < 0
    ):
        raise ValueError("Invalid runner peak-resident-memory measurement")
    return data, wall


def summarize_scenarios(rows: list[dict], tolerances: Tolerances) -> dict:
    groups: dict[str, list[dict]] = {}
    for row in rows:
        for scenario in row["scenarios"]:
            groups.setdefault(scenario, []).append(row)
    return {name: summarize(group, tolerances) for name, group in sorted(groups.items())}


def summarize_registers(rows: list[dict], tolerances: Tolerances) -> dict:
    ranges = {
        "low_midi_0_47": lambda pitch: pitch < 48,
        "mid_midi_48_71": lambda pitch: 48 <= pitch < 72,
        "high_midi_72_127": lambda pitch: pitch >= 72,
    }
    groups: dict[str, list[dict]] = {name: [] for name in ranges}
    for row in rows:
        for name, includes in ranges.items():
            references = [event for event in row["references"] if includes(event["midi"])]
            predictions = [event for event in row["predictions"] if includes(event["midi"])]
            if references or predictions:
                groups[name].append(dict(
                    row, references=references, predictions=predictions,
                    metrics=score_events(references, predictions,
                                         row["duration_seconds"], tolerances),
                ))
    return {
        name: summarize(group, tolerances) if group else None
        for name, group in groups.items()
    }


def summarize(rows: list[dict], tolerances: Tolerances) -> dict:
    summary = {}
    for metric in ("note_event", "onset_pitch", "pitch_event", "frame"):
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
    memory = []
    references = predictions = fragmented = extra_fragments = merged = extra_reference_notes = 0
    for row in rows:
        refs, preds = row["references"], row["predictions"]
        references += len(refs)
        predictions += len(preds)
        fragmented += row["metrics"]["note_fragmentation"]["fragmented_reference_notes"]
        extra_fragments += row["metrics"]["note_fragmentation"]["extra_predicted_fragments"]
        merged += row["metrics"]["note_merging"]["merged_predictions"]
        extra_reference_notes += row["metrics"]["note_merging"]["extra_reference_notes"]
        for r, p in event_pairs(refs, preds, tolerances=tolerances):
            onsets.append(abs(preds[p]["start"] - refs[r]["start"]))
            offsets.append(abs(preds[p]["start"] + preds[p]["duration"]
                               - refs[r]["start"] - refs[r]["duration"]))
        latency.extend(row["realtime_onset_analysis_latency_seconds"])
        first_updates.extend(row["first_update_seconds"])
        process.extend(row["process_wall_seconds"])
        analysis.extend(run["analysis_seconds"] for run in row["runs"])
        totals.extend(run["total_seconds"] for run in row["runs"])
        memory.extend(run["peak_resident_bytes"] for run in row["runs"]
                      if run.get("peak_resident_bytes") is not None)
    audio_seconds = sum(row["duration_seconds"] for row in rows)
    prediction_count = sum(len(row["predictions"]) for row in rows)
    summary.update(
        clip_count=len(rows), reference_notes=references, predicted_notes=predictions,
        false_positives=summary["note_event"]["fp"], missed_notes=summary["note_event"]["fn"],
        false_positives_per_minute=(
            summary["note_event"]["fp"] * 60 / audio_seconds if audio_seconds else None
        ),
        note_fragmentation={
            "fragmented_reference_notes": fragmented,
            "extra_predicted_fragments": extra_fragments,
            "rate": fragmented / references if references else None,
        },
        note_merging={
            "merged_predictions": merged,
            "extra_reference_notes": extra_reference_notes,
            "rate": merged / prediction_count if prediction_count else None,
        },
        onset_error_seconds=distribution(onsets), offset_error_seconds=distribution(offsets),
        realtime_onset_analysis_latency_seconds=distribution(latency),
        first_update_seconds=distribution(first_updates),
        process_wall_seconds=distribution(process), analysis_seconds=distribution(analysis),
        total_seconds=distribution(totals), peak_resident_bytes=distribution(memory),
        skipped_inferences=sum(run["inference_skipped"] for row in rows for run in row["runs"]),
        corpus_audio_seconds=audio_seconds,
        corpus_real_time_factor=sum(totals) / sum(
            row["duration_seconds"] * len(row["runs"]) for row in rows
        ) if any(row["duration_seconds"] for row in rows) else None,
    )
    session_times = [value for row in rows for run in row["runs"]
                     for value in run.get("session_inference_seconds", [])]
    if session_times:
        summary["persistent_session_inference_seconds"] = distribution(session_times)
    return summary


SCENARIO_TAGS = {
    "monophonic": ["monophonic"],
    "repeated": ["monophonic", "repeated_notes"],
    "silence": ["silence"],
    "empty": ["silence"],
    "very_short": ["monophonic", "short_note"],
    "octaves": ["monophonic", "register_transitions"],
    "vibrato": ["monophonic", "vibrato"],
    "chord": ["simultaneous_notes", "chords", "polyphonic"],
    "overlap": ["overlapping_notes", "polyphonic"],
    "noise": ["monophonic", "noisy_recording"],
    "noise_only": ["silence", "noisy_recording"],
    "reverberation": ["monophonic", "reverberation"],
    "decay": ["monophonic", "decay"],
    "attack": ["monophonic", "slow_attack"],
    "clipping": ["monophonic", "clipping"],
    "long_boundary": ["monophonic", "streaming_boundary"],
    "sustained": ["monophonic", "sustained_notes"],
    "pitch_bend": ["monophonic", "pitch_bend"],
    "harmonics_only": ["monophonic", "harmonics"],
    "register_low": ["monophonic", "register_low"],
    "register_mid": ["monophonic", "register_mid"],
    "register_high": ["monophonic", "register_high"],
}


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
                  corpus_version=args.corpus_version if args.corpus == "synthetic" else None,
                  dataset_split=args.dataset_split if args.corpus in ("guitarset", "mixed") else None,
                  chunk_seconds=args.chunk_seconds, overlap_seconds=args.overlap_seconds,
                  realtime_latency_ms=args.latency_ms, realtime_update_rate_hz=100,
                  realtime_minimum_confidence=.45, maximum_pitches_per_frame=6,
                  spectral_threshold=.3, onnx_polyphonic_threshold=.4,
                  onnx_independent_probabilities=True, device_id=0, instrument_prior="none")
    config["polyphonic_decoder"] = args.polyphonic_decoder
    config["model_objective"] = args.model_objective
    if getattr(args, "neural_decoder", "legacy") != "legacy":
        config["neural_decoder"] = args.neural_decoder
    if args.temporal:
        config["temporal_decoding"] = dict(activation_on=args.activation_on,
            activation_off=args.activation_off, maximum_gap_seconds=args.maximum_gap,
            audio_refinement=not args.no_audio_refinement,
            minimum_duration_seconds=args.minimum_duration,
            energy_floor_rms=args.energy_floor, energy_relative_threshold=.08,
            spectral_peak_fraction=.02, pitch_stability_seconds=args.pitch_stability,
            pitch_tolerance_semitones=args.pitch_tolerance, onset_threshold=args.onset_threshold,
            minimum_onset_spacing_seconds=args.onset_spacing,
            onset_energy_rise=args.onset_energy_rise, onset_state_drop=args.onset_state_drop,
            use_model_onsets=not args.no_model_onsets)
        config["temporal_decoding"]["correct_isolated_octaves"] = args.correct_isolated_octaves
    if args.model_interface:
        config["model_interface"] = dict(version=1,
            contract=args.model_contract if args.backend == "onnx" else args.backend,
            incremental=False, batch_size=1,
            cpu_fallback_disabled=args.provider != "cpu")
        if getattr(args, "session_warmups", 0) or getattr(args, "session_repetitions", 1) != 1:
            config["model_interface"]["session_warmups"] = args.session_warmups
            config["model_interface"]["session_repetitions"] = args.session_repetitions
    with tempfile.TemporaryDirectory(prefix="transcriber-evaluation-") as temporary:
        directory = Path(temporary)
        if args.corpus == "mixed":
            corpus = paired_corpus(args.paired_manifest or generate_mixtures(directory), args.dataset_split)
        else:
            corpus = (generate(directory, args.corpus_version) if args.corpus == "synthetic"
                      else real_corpus(args, directory))
        rows = []
        for clip in corpus["clips"]:
            args.instrument = clip.get("instrument", "none")
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
            if any(run.get("canonical_events") != data_runs[0].get("canonical_events")
                   for run in data_runs[1:]):
                raise RuntimeError(f"Nondeterministic canonical evidence: {clip['id']}")
            metrics = score_events(clip["notes"], predictions, clip["duration_seconds"], tolerances)
            latencies = [
                event["detected_at_seconds"] - event["time_seconds"]
                for run in data_runs for event in run["events"] if event["type"] == "onset"
            ]
            scenarios = clip.get("scenarios", SCENARIO_TAGS.get(clip["id"], ["unclassified"]))
            rows.append(dict(
                id=clip["id"], sample_rate=clip["sample_rate"], audio_sha256=clip["sha256"],
                duration_seconds=clip["duration_seconds"], references=clip["notes"],
                scenarios=scenarios, dataset_split=clip.get("dataset_split", "test"),
                performer_id=clip.get("performer_id"),
                predictions=predictions, metrics=metrics, runs=[
                    {key: value for key, value in run.items()
                     if key not in ("notes", "events", "canonical_events")}
                    for run in data_runs],
                realtime_events=data_runs[0]["events"],
                canonical_events=data_runs[0].get("canonical_events", []),
                realtime_onset_analysis_latency_seconds=latencies,
                first_update_seconds=[run["first_update_seconds"] for run in data_runs
                                      if run["first_update_seconds"] is not None],
                process_wall_seconds=wall_runs,
                real_time_factor=[run["total_seconds"] / clip["duration_seconds"]
                                  if clip["duration_seconds"] else None for run in data_runs],
            ))
            for key in ("pair_id", "variant", "instrument", "separation", "artifacts", "source", "license"):
                if key in clip:
                    rows[-1][key] = clip[key]
        corpus.pop("clips")
    summary = summarize(rows, tolerances)
    summary["by_scenario"] = summarize_scenarios(rows, tolerances)
    summary["by_register"] = summarize_registers(rows, tolerances)
    pairs = paired_comparisons(rows)
    if pairs:
        summary["paired_preprocessing"] = pairs
        config["instrument_prior"] = "per-pair target; identical for original and isolated"
    return dict(
        schema_version=SCHEMA_VERSION, generated_at_utc=datetime.now(timezone.utc).isoformat(),
        corpus=corpus, build=build, audio_sample_rates=sorted({row["sample_rate"] for row in rows}),
        evaluator=source_identity(),
        environment=dict(host_identity(), python=platform.python_version()),
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
        summary=summary, clips=rows,
    )


def discover_onnx_models(args) -> tuple[list[Path], list[dict]]:
    if args.models:
        paths = [path.resolve() for path in args.models]
    elif args.model:
        paths = [args.model.resolve()]
    else:
        paths = sorted((ROOT / "python" / "models").rglob("*.onnx"))
    unique: dict[str, Path] = {}
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(f"ONNX model does not exist: {path}")
        unique.setdefault(sha256_file(path), path)
    models = [(digest, path) for digest, path in sorted(unique.items())]
    enabled = bool(build_identity(args.build_info, args.runner.resolve())["features"].get("onnx"))
    if not enabled and (args.models or args.model):
        raise ValueError("ONNX models were requested, but the selected build has ONNX disabled")
    skipped = []
    if not enabled:
        skipped = [
            {"file": path.name, "sha256": digest,
             "reason": "ONNX support is disabled in the selected build"}
            for digest, path in models
        ]
        models = []
    return models, skipped


def evaluate_suite(args) -> dict:
    if args.mode != "full":
        raise ValueError("The all-backends comparison requires --mode full")
    if args.temporal and args.model_interface:
        raise ValueError("--temporal and --model-interface cannot be combined")
    models, skipped = discover_onnx_models(args)
    systems = []
    candidates = [("yin", None), ("spectral", None)]
    candidates.extend(("onnx", path) for _, path in models)
    for backend, model in candidates:
        system_args = copy.copy(args)
        system_args.backend = backend
        system_args.provider = args.provider if backend == "onnx" else "cpu"
        system_args.model = model
        system_args.model_identifier = (
            args.model_identifier if model is not None and len(models) == 1 else None
        )
        report = evaluate(system_args)
        identifier = (
            "onnx:" + report["model"]["sha256"]
            if backend == "onnx" else "builtin:" + backend
        )
        systems.append({
            "id": identifier,
            "backend": backend,
            "model_file": model.name if model else None,
            "model_sha256": report["model"]["sha256"],
            "provider": system_args.provider,
            "summary": report["summary"],
            "report": report,
        })
    reference = systems[0]["report"]
    input_identity = [
        (row["id"], row["audio_sha256"]) for row in reference["clips"]
    ]
    for system in systems[1:]:
        report = system["report"]
        if report["corpus"]["sha256"] != reference["corpus"]["sha256"]:
            raise RuntimeError("Backend suite did not evaluate the same corpus identity")
        if [(row["id"], row["audio_sha256"]) for row in report["clips"]] != input_identity:
            raise RuntimeError("Backend suite did not evaluate identical clip inputs")
    return {
        "schema_version": SCHEMA_VERSION,
        "type": "comparison_suite",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "corpus": reference["corpus"],
        "input_clips": [{"id": identifier, "audio_sha256": digest}
                        for identifier, digest in input_identity],
        "build": reference["build"],
        "environment": reference["environment"],
        "configuration": {
            "mode": args.mode,
            "corpus": args.corpus,
            "corpus_version": args.corpus_version if args.corpus == "synthetic" else None,
            "dataset_split": args.dataset_split if args.corpus in ("guitarset", "mixed") else None,
            "repetitions": args.repetitions,
            "warmups": args.warmups,
            "provider": args.provider,
        },
        "onnx_models": {
            "evaluated": [
                {"file": system["model_file"], "sha256": system["model_sha256"],
                 "provider": system["provider"]}
                for system in systems if system["backend"] == "onnx"
            ],
            "not_evaluated": skipped,
            "discovery": "All .onnx files under python/models, unless --model/--models is supplied",
        },
        "systems": systems,
    }


def compare_suite(previous: dict, current: dict) -> dict:
    for key in ("schema_version", "type", "configuration"):
        if previous.get(key) != current.get(key):
            raise ValueError(f"Incompatible suite comparison: {key} differs")
    if previous["corpus"]["sha256"] != current["corpus"]["sha256"]:
        raise ValueError("Incompatible suite comparison: corpus identity differs")
    old_systems = {system["id"]: system for system in previous["systems"]}
    new_systems = {system["id"]: system for system in current["systems"]}
    if old_systems.keys() != new_systems.keys():
        raise ValueError("Incompatible suite comparison: evaluated system set differs")
    comparisons = []
    regressions = []
    for identifier in sorted(new_systems):
        delta = compare(old_systems[identifier]["report"], new_systems[identifier]["report"])
        comparisons.append(dict(id=identifier, **delta))
        regressions.extend(
            {"system_id": identifier, "clip_id": row["id"],
             "event_f1_delta": row["note_event_f1_delta"]}
            for row in delta["clip_deltas"] if row["note_event_f1_delta"] < 0
        )
    return {"systems": comparisons, "event_f1_regressions": regressions}


def compare(previous: dict, current: dict) -> dict:
    for key in ("schema_version", "configuration", "tolerances", "model", "timing_protocol"):
        if previous[key] != current[key]:
            raise ValueError(f"Incompatible comparison: {key} differs")
    if previous["corpus"]["sha256"] != current["corpus"]["sha256"]:
        raise ValueError("Incompatible comparison: corpus identity differs")
    previous_clips = {row["id"]: row for row in previous["clips"]}
    current_clips = {row["id"]: row for row in current["clips"]}
    if previous_clips.keys() != current_clips.keys():
        raise ValueError("Incompatible comparison: fixture set differs")
    clip_deltas = []
    for identifier, row in current_clips.items():
        old_row = previous_clips[identifier]
        if old_row["audio_sha256"] != row["audio_sha256"]:
            raise ValueError(f"Incompatible comparison: audio differs for {identifier}")
        deltas = {
            name: row["metrics"][name]["f1"] - old_row["metrics"][name]["f1"]
            for name in ("note_event", "onset_pitch", "pitch_event", "frame")
        }
        clip_deltas.append(dict(id=identifier, **{
            f"{name}_f1_delta": delta for name, delta in deltas.items()
        }))
    clip_deltas.sort(key=lambda row: row["id"])
    old = previous["summary"]["note_event"]["f1"]
    new = current["summary"]["note_event"]["f1"]
    before = previous["summary"]["process_wall_seconds"]["median"]
    after = current["summary"]["process_wall_seconds"]["median"]
    return dict(event_f1_delta=new - old,
                frame_f1_delta=current["summary"]["frame"]["f1"] - previous["summary"]["frame"]["f1"],
                median_process_time_ratio=after / before if before else None,
                clip_deltas=clip_deltas,
                event_f1_regressed_clips=[
                    row["id"] for row in clip_deltas if row["note_event_f1_delta"] < 0
                ],
                worst_clip_event_f1_delta=min(
                    (row["note_event_f1_delta"] for row in clip_deltas), default=None
                ),
                same_build=previous["build"] == current["build"],
                same_environment=previous["environment"] == current["environment"])


def human_report(report: dict) -> str:
    if report.get("type") == "comparison_suite":
        lines = [
            f"Quality comparison suite: {report['corpus']['name']} "
            f"({len(report['input_clips'])} identical clips)",
            f"Build: {report['build']['compiler']['id']} {report['build']['compiler']['version']} "
            f"{report['build']['build_type']}; mode {report['configuration']['mode']}",
        ]
        for system in report["systems"]:
            metrics = system["summary"]["note_event"]
            lines.append(
                f"{system['id']}: event F1 {metrics['f1']:.4f} "
                f"(P {metrics['precision']:.4f}, R {metrics['recall']:.4f}); "
                f"frame F1 {system['summary']['frame']['f1']:.4f}"
            )
        if report["onnx_models"]["not_evaluated"]:
            lines.append("ONNX not evaluated: " + json.dumps(
                report["onnx_models"]["not_evaluated"], sort_keys=True))
        if "comparison" in report:
            lines.append(f"Compared systems: {len(report['comparison']['systems'])}; "
                         f"per-clip event regressions: "
                         f"{len(report['comparison']['event_f1_regressions'])}")
        return "\n".join(lines) + "\n"
    summary = report["summary"]
    lines = [f"{report['corpus']['name']} / {report['configuration']['backend']} / "
             f"{report['configuration']['mode']} ({len(report['clips'])} clips)",
             f"Build: {report['build']['compiler']['id']} {report['build']['compiler']['version']} "
             f"{report['build']['build_type']}; model {report['model']['identifier']}; "
             f"provider {report['configuration']['provider']}"]
    if "temporal_decoding" in report["configuration"]:
        lines.append("Opt-in temporal decoder; calibrated confidence and velocity unavailable.")
    for metric in ("note_event", "onset_pitch", "pitch_event", "frame"):
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
    if "persistent_session_inference_seconds" in summary:
        lines.append("Within-process persistent-session inference seconds: " +
                     json.dumps(summary["persistent_session_inference_seconds"], sort_keys=True))
    lines.append(f"Corpus real-time factor (engine): {summary['corpus_real_time_factor']}")
    lines.append(f"False positives per minute: {summary['false_positives_per_minute']}")
    lines.append(
        "Fragmented reference notes: "
        f"{summary['note_fragmentation']['fragmented_reference_notes']} "
        f"(rate {summary['note_fragmentation']['rate']}); merged predictions: "
        f"{summary['note_merging']['merged_predictions']} "
        f"(rate {summary['note_merging']['rate']})"
    )
    lines.append(
        "Peak resident memory: "
        f"median={summary['peak_resident_bytes']['median']}, "
        f"p95={summary['peak_resident_bytes']['p95']} bytes"
    )
    lines.append("By scenario:")
    for scenario, metrics in summary["by_scenario"].items():
        lines.append(
            f"  {scenario}: {metrics['note_event']['tp']}/"
            f"{metrics['note_event']['tp'] + metrics['note_event']['fp']}/"
            f"{metrics['note_event']['tp'] + metrics['note_event']['fn']} "
            f"event TP/pred/ref; F1 {metrics['note_event']['f1']:.4f}"
        )
    if summary["skipped_inferences"]:
        lines.append(f"Skipped empty-input ONNX invocations: {summary['skipped_inferences']}")
    if summary.get("paired_preprocessing"):
        lines.append("Per-pair original -> isolated deltas (negative is worse; no aggregate benefit claim):")
        for pair in summary["paired_preprocessing"]:
            lines.append(f"  {pair['id']} / {pair['instrument']}: {pair['deltas']}; "
                         f"degraded: {pair['degraded_metrics'] or 'none'}")
    if "comparison" in report:
        lines.append("Comparison: " + json.dumps(report["comparison"], sort_keys=True))
    if "calibration_assessment" in report:
        lines.append("Held-out event confidence calibration: " +
                     json.dumps(report["calibration_assessment"], sort_keys=True))
    return "\n".join(lines) + "\n"


def markdown_report(report: dict) -> str:
    def value(number, digits=4):
        return "n/a" if number is None else f"{number:.{digits}f}"

    def metrics_table(summary):
        rows = [
            "| Metric | Precision | Recall | F1 | TP | FP | FN |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
        for metric in ("note_event", "onset_pitch", "pitch_event", "frame"):
            score = summary[metric]
            rows.append(
                f"| {metric} | {value(score['precision'])} | {value(score['recall'])} | "
                f"{value(score['f1'])} | {score['tp']} | {score['fp']} | {score['fn']} |"
            )
        return rows

    def provenance_block(corpus):
        fields = ("provenance", "license", "dataset_split_policy", "selected_split",
                  "training_data_warning")
        return json.dumps({key: corpus[key] for key in fields if key in corpus},
                          indent=2, sort_keys=True)

    def report_section(single):
        summary = single["summary"]
        rows = [
            f"### {single['configuration']['backend']} / {single['configuration']['mode']}",
            f"Model `{single['model']['identifier']}`; SHA-256 "
            f"`{single['model']['sha256'] or 'n/a'}`; provider "
            f"`{single['configuration']['provider']}`.",
            "",
            *metrics_table(summary),
            "",
            f"False positives/min: **{value(summary['false_positives_per_minute'], 3)}**; "
            f"fragmented references: **{summary['note_fragmentation']['fragmented_reference_notes']}** "
            f"(rate {value(summary['note_fragmentation']['rate'], 3)}); merged predictions: "
            f"**{summary['note_merging']['merged_predictions']}** "
            f"(rate {value(summary['note_merging']['rate'], 3)}).",
            "",
            "| Timing/resource | Median | p95 | Samples |",
            "|---|---:|---:|---:|",
        ]
        for key in ("analysis_seconds", "total_seconds", "process_wall_seconds",
                    "peak_resident_bytes"):
            measurement = summary[key]
            units = " bytes" if key == "peak_resident_bytes" else " s"
            rows.append(
                f"| {key} | {value(measurement['median'])}{units} | "
                f"{value(measurement['p95'])}{units} | {measurement['count']} |"
            )
        if summary.get("paired_preprocessing"):
            rows.extend(["", "#### Original versus isolated (per-pair deltas; negative is worse)", "",
                         "| Pair | Target | Event F1 delta | Onset F1 delta | Pitch F1 delta | Frame F1 delta | Degraded metrics |",
                         "|---|---|---:|---:|---:|---:|---|"])
            for pair in summary["paired_preprocessing"]:
                delta = pair["deltas"]
                rows.append(f"| {pair['id']} | {pair['instrument']} | {value(delta['note_event_f1'])} | "
                            f"{value(delta['onset_pitch_f1'])} | {value(delta['pitch_event_f1'])} | "
                            f"{value(delta['frame_f1'])} | {', '.join(pair['degraded_metrics']) or 'none'} |")
            rows.extend(["", "No separator benefit is inferred from an aggregate score. "
                         "Oracle/damaged synthetic stems are integration checks, not learned-model evidence."])
        rows.extend([
            "",
            f"Engine corpus real-time factor: {value(summary['corpus_real_time_factor'], 5)}.",
            "",
            "#### Scenario cohorts",
            "",
            "| Scenario | Clips | Event F1 | Pitch F1 | Frame F1 | FP/min | Fragmented refs | Merged predictions |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ])
        for scenario, cohort in summary["by_scenario"].items():
            rows.append(
                f"| {scenario} | {cohort['clip_count']} | "
                f"{value(cohort['note_event']['f1'])} | {value(cohort['pitch_event']['f1'])} | "
                f"{value(cohort['frame']['f1'])} | {value(cohort['false_positives_per_minute'], 3)} | "
                f"{cohort['note_fragmentation']['fragmented_reference_notes']} | "
                f"{cohort['note_merging']['merged_predictions']} |"
            )
        rows.extend([
            "",
            "#### Register cohorts",
            "",
            "| Register (MIDI) | Event F1 | Pitch F1 | Frame F1 | Reference notes |",
            "|---|---:|---:|---:|---:|",
        ])
        for register in sorted(summary["by_register"]):
            cohort = summary["by_register"][register]
            if cohort is None:
                rows.append(f"| {register} | n/a | n/a | n/a | 0 |")
            else:
                rows.append(
                    f"| {register} | {value(cohort['note_event']['f1'])} | "
                    f"{value(cohort['pitch_event']['f1'])} | {value(cohort['frame']['f1'])} | "
                    f"{cohort['reference_notes']} |"
                )
        rows.extend([
            "",
            "#### Per-clip results (regressions remain visible)",
            "",
            "| Clip | Scenarios | Refs | Preds | Event F1 | Onset+pitch F1 | Pitch F1 | Frame F1 | Onset median (s) | Offset median (s) | FP/min | Fragmented refs | Merged preds |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ])
        for clip in single["clips"]:
            metrics = clip["metrics"]
            rows.append(
                f"| {clip['id']} | {', '.join(clip['scenarios'])} | {len(clip['references'])} | "
                f"{len(clip['predictions'])} | {value(metrics['note_event']['f1'])} | "
                f"{value(metrics['onset_pitch']['f1'])} | {value(metrics['pitch_event']['f1'])} | "
                f"{value(metrics['frame']['f1'])} | "
                f"{value(metrics['onset_error_seconds']['median'])} | "
                f"{value(metrics['offset_error_seconds']['median'])} | "
                f"{value(metrics['false_positives_per_minute'], 3)} | "
                f"{metrics['note_fragmentation']['fragmented_reference_notes']} | "
                f"{metrics['note_merging']['merged_predictions']} |"
            )
        return rows

    if report.get("type") == "comparison_suite":
        rows = [
            "# Transcriber Quality Evaluation Suite",
            "",
            f"Corpus: **{report['corpus']['name']} v{report['corpus']['version']}**; "
            f"{len(report['input_clips'])} identical input clips; mode "
            f"`{report['configuration']['mode']}`; {report['configuration']['repetitions']} "
            f"measured repetitions after {report['configuration']['warmups']} warmups.",
            "",
            "Build/provider/model hashes and complete run data are recorded in the JSON report.",
            "",
            "## Dataset provenance and split policy",
            "",
            "```json",
            provenance_block(report["corpus"]),
            "```",
            "",
            "## Systems",
            "",
            "| System | Provider | Model SHA-256 | Event F1 | Pitch F1 | Frame F1 | FP/min | Peak memory median (bytes) |",
            "|---|---|---|---:|---:|---:|---:|---:|",
        ]
        for system in report["systems"]:
            summary = system["summary"]
            rows.append(
                f"| {system['id']} | {system['provider']} | {system['model_sha256'] or 'n/a'} | "
                f"{value(summary['note_event']['f1'])} | {value(summary['pitch_event']['f1'])} | "
                f"{value(summary['frame']['f1'])} | {value(summary['false_positives_per_minute'], 3)} | "
                f"{value(summary['peak_resident_bytes']['median'], 0)} |"
            )
        rows.extend(["", "## ONNX models", ""])
        for model in report["onnx_models"]["evaluated"]:
            rows.append(f"- Evaluated `{model['file']}` ({model['sha256']}) on `{model['provider']}`.")
        if not report["onnx_models"]["evaluated"]:
            rows.append("- No ONNX model was evaluated.")
        for model in report["onnx_models"]["not_evaluated"]:
            rows.append(f"- Not evaluated `{model['file']}` ({model['sha256']}): {model['reason']}.")
        rows.extend(["", "## Detailed results", ""])
        for system in report["systems"]:
            rows.extend(report_section(system["report"]))
            rows.append("")
        if "comparison" in report:
            rows.extend(["## Baseline comparison", ""])
            for system in report["comparison"]["systems"]:
                rows.append(
                    f"- `{system['id']}` aggregate event-F1 delta "
                    f"{value(system['event_f1_delta'])}; worst per-clip delta "
                    f"{value(system['worst_clip_event_f1_delta'])}."
                )
                for clip in system["clip_deltas"]:
                    rows.append(
                        f"  - `{clip['id']}`: event {value(clip['note_event_f1_delta'])}, "
                        f"pitch {value(clip['pitch_event_f1_delta'])}, "
                        f"frame {value(clip['frame_f1_delta'])}."
                    )
            rows.append("")
        return "\n".join(rows) + "\n"

    rows = [
        "# Transcriber Quality Evaluation",
        "",
        f"Corpus: **{report['corpus']['name']} v{report['corpus']['version']}**; "
        f"backend `{report['configuration']['backend']}` / mode "
        f"`{report['configuration']['mode']}`; {len(report['clips'])} clips.",
        "",
        f"Build: `{report['build']['compiler']['id']} "
        f"{report['build']['compiler']['version']}` / `{report['build']['build_type']}`; "
        f"model `{report['model']['identifier']}` / SHA-256 "
        f"`{report['model']['sha256'] or 'n/a'}`; provider `{report['configuration']['provider']}`.",
        "",
        f"Timing protocol: {report['timing_protocol']['repetitions']} measured repetitions, "
        f"{report['timing_protocol']['warmups']} warmups; "
        f"{report['timing_protocol']['isolation']}.",
        "",
        "## Overall metrics",
        "",
        *metrics_table(report["summary"]),
        "",
        f"False positives/min: **{value(report['summary']['false_positives_per_minute'], 3)}**; "
        f"fragmented reference notes: **{report['summary']['note_fragmentation']['fragmented_reference_notes']}**; "
        f"merged predictions: **{report['summary']['note_merging']['merged_predictions']}**.",
        "",
        "## Dataset provenance",
        "",
        f"Corpus identity: `{report['corpus']['sha256']}`.",
        "",
        "```json",
        provenance_block(report["corpus"]),
        "```",
        "",
        *report_section(report),
    ]
    if "comparison" in report:
        rows.extend(["", "## Baseline comparison", "",
                     f"Aggregate event-F1 delta: {value(report['comparison']['event_f1_delta'])}; "
                     f"worst clip delta: {value(report['comparison']['worst_clip_event_f1_delta'])}.",
                     "",
                     "| Clip | Event F1 delta | Onset+pitch F1 delta | Pitch F1 delta | Frame F1 delta |",
                     "|---|---:|---:|---:|---:|"])
        for clip in report["comparison"]["clip_deltas"]:
            rows.append(
                f"| {clip['id']} | {value(clip['note_event_f1_delta'])} | "
                f"{value(clip['onset_pitch_f1_delta'])} | {value(clip['pitch_event_f1_delta'])} | "
                f"{value(clip['frame_f1_delta'])} |"
            )
    if "calibration_assessment" in report:
        rows.extend(["", "## Held-out event confidence calibration", "",
                     "```json", json.dumps(report["calibration_assessment"], indent=2), "```"])
    return "\n".join(rows) + "\n"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--build-info", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-text", type=Path)
    parser.add_argument("--output-markdown", type=Path)
    parser.add_argument("--backend", choices=("yin", "spectral", "onnx", "all"), default="yin")
    parser.add_argument("--mode", choices=("full", "streaming", "realtime"))
    parser.add_argument("--corpus", choices=("synthetic", "guitarset", "mixed"), default="synthetic")
    parser.add_argument("--paired-manifest", type=Path,
                        help="Mixed original/stem pairs with target-only note annotations and licensing; absent uses synthetic oracle/damage checks")
    parser.add_argument("--corpus-version", type=int, choices=(1, 2, 3), default=1)
    parser.add_argument("--polyphonic-decoder", choices=("legacy", "competing"), default="legacy")
    parser.add_argument("--model-objective", choices=("monophonic", "independent-multi-pitch"))
    parser.add_argument("--dataset-split", choices=("all", "train", "validation", "test"),
                        default="all")
    parser.add_argument("--model", type=Path)
    parser.add_argument("--models", type=Path, action="append",
                        help="ONNX model path; repeat to evaluate multiple models in --backend all mode")
    parser.add_argument("--model-identifier")
    parser.add_argument("--provider", choices=("cpu", "cuda", "directml", "rocm"), default="cpu")
    parser.add_argument("--fast", action="store_true")
    parser.add_argument("--serial", action="store_true")
    parser.add_argument("--temporal", action="store_true")
    parser.add_argument("--model-interface", action="store_true")
    parser.add_argument("--session-warmups", type=int, default=0)
    parser.add_argument("--session-repetitions", type=int, default=1)
    parser.add_argument("--neural-decoder", choices=("legacy", "events", "speed", "quality"), default="legacy",
                        help="Opt-in aligned Basic Pitch event decoder; no accuracy guarantee")
    parser.add_argument("--model-contract", choices=("basic-pitch-v1", "legacy-pitch-v1"),
                        default="basic-pitch-v1")
    parser.add_argument("--activation-on", type=float, default=.4)
    parser.add_argument("--activation-off", type=float, default=.4)
    parser.add_argument("--maximum-gap", type=float, default=0)
    parser.add_argument("--minimum-duration", type=float, default=.055)
    parser.add_argument("--energy-floor", type=float, default=.0015)
    parser.add_argument("--pitch-stability", type=float, default=.05)
    parser.add_argument("--pitch-tolerance", type=float, default=.75)
    parser.add_argument("--onset-threshold", type=float, default=.5)
    parser.add_argument("--onset-spacing", type=float, default=.07)
    parser.add_argument("--onset-energy-rise", type=float, default=.25)
    parser.add_argument("--onset-state-drop", type=float, default=.15)
    parser.add_argument("--no-model-onsets", action="store_true")
    parser.add_argument("--no-audio-refinement", action="store_true")
    parser.add_argument("--correct-isolated-octaves", action="store_true")
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
    parser.add_argument("--confidence-calibration", type=Path,
                        help="Frozen validation calibration; assess on disjoint test recordings only")
    parser.add_argument("--max-event-f1-drop", type=float)
    args = parser.parse_args()
    args.neural_decoder = {"speed": "legacy", "quality": "events"}.get(
        args.neural_decoder, args.neural_decoder)
    if (not 0 <= args.session_warmups <= 100 or not 1 <= args.session_repetitions <= 100 or
        (not args.model_interface and (args.session_warmups or args.session_repetitions != 1))):
        parser.error("Session timing counts require --model-interface and counts within 0..100")
    if args.confidence_calibration and (args.backend == "all" or args.dataset_split != "test"):
        parser.error("--confidence-calibration requires a single backend and --dataset-split test")
    if args.neural_decoder != "legacy" and (
        args.backend != "onnx" or args.temporal or args.model_contract != "basic-pitch-v1"
    ):
        parser.error("--neural-decoder events requires ONNX Basic Pitch without --temporal")
    if args.mode is None:
        args.mode = "full" if args.backend in ("onnx", "all") else "streaming"
    if args.model_interface and (args.mode != "full" or args.temporal):
        parser.error("--model-interface requires --mode full without --temporal")
    if args.temporal and args.mode == "realtime":
        parser.error("Temporal evaluation is offline only")
    if not args.temporal and (args.activation_on != .4 or args.activation_off != .4 or
                             args.maximum_gap != 0 or args.no_audio_refinement or
                             args.correct_isolated_octaves or args.minimum_duration != .055 or
                             args.energy_floor != .0015 or args.pitch_stability != .05 or
                             args.pitch_tolerance != .75 or args.onset_threshold != .5 or
                             args.onset_spacing != .07 or args.onset_energy_rise != .25 or
                             args.onset_state_drop != .15 or args.no_model_onsets):
        parser.error("Temporal decoding options require --temporal")
    if args.repetitions < 1 or args.warmups < 0 or (args.limit is not None and args.limit < 1):
        parser.error("Repetitions/limit must be positive; warmups must be nonnegative")
    for name in ("minimum_confidence", "chunk_seconds", "overlap_seconds", "latency_ms",
                 "timeout_seconds", "onset_tolerance", "offset_tolerance", "offset_ratio", "frame_step",
                 "activation_on", "activation_off", "maximum_gap", "minimum_duration",
                 "energy_floor", "pitch_stability", "pitch_tolerance", "onset_threshold",
                 "onset_spacing", "onset_energy_rise", "onset_state_drop"):
        if not math.isfinite(getattr(args, name)):
            parser.error(f"{name} must be finite")
    if args.timeout_seconds <= 0 or not 0 <= args.minimum_confidence <= 1:
        parser.error("Invalid timeout or minimum confidence")
    if not 0 <= args.activation_off <= args.activation_on <= 1 or args.maximum_gap < 0:
        parser.error("Temporal thresholds require 0 <= off <= on <= 1 and a nonnegative gap")
    if (args.minimum_duration <= 0 or args.energy_floor < 0 or args.pitch_stability < 0 or
        not 0 <= args.pitch_tolerance <= 1 or not 0 <= args.onset_threshold <= 1 or
        args.onset_spacing < 0 or args.onset_energy_rise < 0 or
        not 0 <= args.onset_state_drop <= 1):
        parser.error("Invalid segmentation duration, floor, stability or onset parameter")
    if ((args.backend == "onnx" and (not args.model or args.models)) or
        (args.backend == "all" and args.model and args.models) or
        (args.backend not in ("onnx", "all") and (args.model or args.models)) or
        (args.backend not in ("onnx", "all") and args.provider != "cpu") or
        (args.backend in ("onnx", "all") and (args.mode != "full" or args.fast)) or
        (args.backend == "all" and args.fast) or
        (args.mode == "realtime" and (args.backend != "yin" or args.fast
                                    or args.minimum_confidence != .25))):
        parser.error("Unsupported backend/mode/model/provider combination")
    if args.corpus == "synthetic" and args.dataset_split != "all":
        parser.error("--dataset-split applies only to the GuitarSet corpus")
    if args.paired_manifest and args.corpus != "mixed":
        parser.error("--paired-manifest requires --corpus mixed")
    if args.max_event_f1_drop is not None and (
        not args.compare or not math.isfinite(args.max_event_f1_drop) or args.max_event_f1_drop < 0
    ):
        parser.error("A finite nonnegative --max-event-f1-drop requires --compare")
    return args


def main() -> int:
    args = parse_args()
    try:
        report = evaluate_suite(args) if args.backend == "all" else evaluate(args)
        if args.compare:
            previous = json.loads(args.compare.read_text(encoding="utf-8"))
            report["comparison"] = (
                compare_suite(previous, report) if args.backend == "all"
                else compare(previous, report)
            )
        if getattr(args, "confidence_calibration", None):
            calibration = json.loads(args.confidence_calibration.read_text(encoding="utf-8"))
            report["calibration_assessment"] = apply_event_calibration(report, calibration)
            report["calibration_assessment"]["artifact_sha256"] = sha256_file(args.confidence_calibration)
        write_json(args.output_json, report)
        text = human_report(report)
        print(text, end="")
        if args.output_text:
            args.output_text.parent.mkdir(parents=True, exist_ok=True)
            args.output_text.write_text(text, encoding="utf-8")
        if args.output_markdown:
            args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
            args.output_markdown.write_text(markdown_report(report), encoding="utf-8")
        if args.max_event_f1_drop is not None:
            if args.backend == "all":
                comparisons = report["comparison"]["systems"]
            else:
                comparisons = [report["comparison"]]
            failures = [
                (comparison["id"] if args.backend == "all" else "system", row["id"],
                 row["note_event_f1_delta"])
                for comparison in comparisons
                for row in comparison["clip_deltas"]
                if row["note_event_f1_delta"] < -args.max_event_f1_drop
            ]
            failures.extend(
                (comparison.get("id", "system"), "aggregate",
                 comparison["event_f1_delta"])
                for comparison in comparisons
                if comparison["event_f1_delta"] < -args.max_event_f1_drop
            )
            if failures:
                print(
                    f"Event F1 regression exceeds the requested gate: {failures}",
                    file=sys.stderr,
                )
                return 1
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError,
            zipfile.BadZipFile) as error:
        print(f"Evaluation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
