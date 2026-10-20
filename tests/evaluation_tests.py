"""Unit and engine integration tests for the evaluation contract (stdlib only)."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import itertools
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import wave
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
from evaluation_metrics import Tolerances, event_pairs, frame_counts, score_events
from synthetic_corpus import generate, specifications
from evaluate import build_identity, compare, human_report
import evaluate
from evaluate_guitarset import md5_file, read_reference_events
import evaluate_guitarset
from performance import compare_reports
from onnx_fixtures import write_model

RUNNER = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
BUILD_INFO = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
PERFORMANCE_RUNNER = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None


def event(pitch=69, start=0.0, duration=1.0):
    return dict(midi=pitch, start=start, duration=duration)


class MetricsTests(unittest.TestCase):
    def test_performance_comparison_handles_clock_resolution_and_rejects_drift(self):
        row = dict(id="empty", backend="yin", mode="full", audio_sha256="same",
                   notes=[], metrics=score_events([], [], 0),
                   runs=[dict(wall_seconds=0), dict(wall_seconds=0)])
        before = dict(configuration={}, clips=[row])
        current = copy.deepcopy(before)
        compare_reports(before, current)
        self.assertTrue(current["all_notes_identical"])
        self.assertIsNone(current["clips"][0]["comparison"]["cold_speedup"])
        changed = copy.deepcopy(before)
        changed["clips"][0]["notes"] = [event()]
        compare_reports(before, changed)
        self.assertFalse(changed["all_notes_identical"])
        changed["clips"][0]["audio_sha256"] = "different"
        with self.assertRaisesRegex(ValueError, "audio differs"):
            compare_reports(before, changed)

    def test_perfect_events_and_empty_convention(self):
        result = score_events([event()], [event()], 1)
        for metric in ("note_event", "onset_pitch", "frame"):
            self.assertEqual(result[metric]["f1"], 1)
        empty = score_events([], [], 1)
        self.assertEqual(empty["note_event"]["fp"], 0)
        self.assertEqual(empty["note_event"]["f1"], 0)
        self.assertIsNone(empty["pitch_accuracy"]["value"])
        self.assertIsNone(empty["onset_error_seconds"]["mean"])

    def test_tolerances_are_explicit_and_inclusive(self):
        tolerances = Tolerances(.05, .05, 0, .01)
        self.assertEqual(len(event_pairs([event()], [event(start=.05)],
                                        require_offset=True, tolerances=tolerances)), 1)
        self.assertFalse(event_pairs([event()], [event(start=.051)], tolerances=tolerances))
        self.assertFalse(event_pairs([event()], [event(duration=1.051)],
                                     require_offset=True, tolerances=tolerances))

    def test_offset_ratio_and_frame_event_distinction(self):
        refs = [event(duration=2)]
        self.assertEqual(len(event_pairs(refs, [event(duration=2.3)], require_offset=True)), 1)
        fragmented = [event(duration=.5), event(start=.5, duration=.5)]
        result = score_events([event()], fragmented, 1)
        self.assertEqual(result["frame"]["f1"], 1)
        self.assertEqual(result["note_event"]["f1"], 0)
        self.assertEqual(result["offset_error_seconds"]["mean"], .5)

    def test_one_to_one_duplicates_wrong_pitch_and_chords(self):
        result = score_events([event()], [event(), event()], 1)
        self.assertEqual(result["note_event"]["tp"], 1)
        self.assertEqual(result["false_positives"], 1)
        wrong = score_events([event()], [event(70)], 1)
        self.assertEqual(wrong["pitch_accuracy"]["value"], 0)
        self.assertEqual(wrong["missed_notes"], 1)
        chord = score_events([event(60), event(64), event(67)], [event(60), event(67)], 1)
        self.assertEqual(chord["chord_note_recall"]["value"], 2 / 3)
        reordered = score_events([event(60), event(64), event(67)],
                                 [event(67), event(64), event(60)], 1)
        self.assertEqual(reordered["pitch_accuracy"]["value"], 1)

    def test_augmenting_paths_match_bruteforce(self):
        rng = random.Random(2026)
        for _ in range(50):
            refs = [event(start=rng.randrange(4) * .03) for _ in range(4)]
            preds = [event(start=rng.randrange(4) * .03) for _ in range(4)]
            best = 0
            for n in range(1, 5):
                for rs in itertools.combinations(range(4), n):
                    for ps in itertools.permutations(range(4), n):
                        if all(abs(refs[r]["start"] - preds[p]["start"]) <= .05
                               for r, p in zip(rs, ps)):
                            best = max(best, n)
            self.assertEqual(len(event_pairs(refs, preds)), best)

    def test_chord_pitch_accuracy_does_not_depend_on_offsets(self):
        refs = [event(60, duration=1), event(64, duration=2)]
        preds = [event(60, duration=2), event(64, duration=1)]
        for ordered in (preds, list(reversed(preds))):
            result = score_events(refs, ordered, 2)
            self.assertEqual(result["pitch_accuracy"]["value"], 1)
            self.assertEqual(result["note_event"]["f1"], 0)
            self.assertEqual(result["onset_pitch"]["f1"], 1)

    def test_sparse_frame_counts_match_dense_legacy_definition(self):
        rng = random.Random(17)
        for _ in range(40):
            refs = [event(rng.randrange(60, 63), rng.random(), .1 + rng.random()) for _ in range(6)]
            preds = [event(rng.randrange(60, 63), rng.random(), .1 + rng.random()) for _ in range(6)]
            counts = [0, 0, 0]
            for frame in range(200):
                t = (frame + .5) * .01
                for pitch in range(60, 63):
                    r = any(n["midi"] == pitch and n["start"] <= t <
                            n["start"] + n["duration"] for n in refs)
                    p = any(n["midi"] == pitch and n["start"] <= t <
                            n["start"] + n["duration"] for n in preds)
                    if r and p: counts[0] += 1
                    elif p: counts[1] += 1
                    elif r: counts[2] += 1
            self.assertEqual(frame_counts(refs, preds, 2), tuple(counts))

    def test_invalid_events_and_tolerances_fail_explicitly(self):
        for invalid in (event(start=-1), event(duration=0), event(128),
                        event(start=float("nan"))):
            with self.assertRaises(ValueError):
                score_events([invalid], [], 1)
        for value in (float("nan"), float("inf"), -1):
            with self.assertRaises(ValueError):
                Tolerances(onset_seconds=value)
        with self.assertRaises(ValueError):
            Tolerances(frame_seconds=0)


class CorpusTests(unittest.TestCase):
    def test_corpus_reproducibility_ground_truth_and_effects(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = generate(root / "a")
            second = generate(root / "b")
            self.assertEqual(first, second)
            self.assertEqual(len(first["clips"]), 16)
            for clip in first["clips"]:
                self.assertEqual((root / "a" / clip["audio"]).read_bytes(),
                                 (root / "b" / clip["audio"]).read_bytes())
                for n in clip["notes"]:
                    self.assertEqual(n["onset"], n["start"])
                    self.assertAlmostEqual(n["offset"] - n["onset"], n["duration"])
                    self.assertIn("velocity", n)
                    self.assertIn("instrument", n)
                with wave.open(str(root / "a" / clip["audio"]), "rb") as wav:
                    self.assertAlmostEqual(wav.getnframes() / wav.getframerate(),
                                           clip["duration_seconds"], places=4)
            silence = (root / "a" / "silence.wav").read_bytes()[44:]
            self.assertFalse(any(silence))
            self.assertNotEqual((root / "a" / "noise.wav").read_bytes()[44:], silence)
            self.assertTrue(any(s["id"] == "long_boundary" and s["duration_seconds"] > 15
                                for s in specifications()))

    def test_real_annotation_and_archive_workflow_without_external_data(self):
        annotation = {"annotations": [{"namespace": "note_midi", "data": [
            {"time": .1, "duration": .5, "value": 60.1}
        ]}]}
        self.assertEqual(read_reference_events(annotation), [event(60, .1, .5)])
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            generated = generate(directory / "corpus")
            source = directory / "corpus" / generated["clips"][0]["audio"]
            audio, labels = directory / "audio.zip", directory / "labels.zip"
            with zipfile.ZipFile(audio, "w") as archive:
                archive.write(source, "fixture_mic.wav")
            with zipfile.ZipFile(labels, "w") as archive:
                archive.writestr("fixture.jams", json.dumps(annotation))
            args = SimpleNamespace(audio_zip=audio, annotations_zip=labels, limit=None, seed=2026)
            destination = directory / "extracted"
            destination.mkdir()
            with self.assertRaises(ValueError):
                evaluate.real_corpus(args, destination)
            # Only this hermetic test substitutes archive hashes; the CLI has no bypass.
            with patch.object(evaluate, "AUDIO_MD5", md5_file(audio)), \
                 patch.object(evaluate, "ANNOTATION_MD5", md5_file(labels)):
                corpus = evaluate.real_corpus(args, destination)
            self.assertEqual(corpus["clips"][0]["id"], "fixture")
            self.assertEqual(corpus["clips"][0]["notes"][0]["instrument"], "guitar")
            self.assertIsNone(corpus["clips"][0]["notes"][0]["velocity"])
            self.assertTrue((destination / "real-0.wav").is_file())
            if RUNNER is not None and BUILD_INFO is not None:
                model = directory / "custom.onnx"
                model.write_bytes(b"identity-only mock model; not used for inference")
                legacy_args = SimpleNamespace(
                    transcriber=Path(json.loads(BUILD_INFO.read_text())["executable"]),
                    model=model, audio_zip=audio, annotations_zip=labels,
                    build_info=BUILD_INFO, limit=None, seed=2026,
                    skip_checksums=False, skip_yin_baseline=False)
                with patch.object(evaluate_guitarset, "AUDIO_MD5", md5_file(audio)), \
                     patch.object(evaluate_guitarset, "ANNOTATION_MD5", md5_file(labels)), \
                     patch.object(evaluate_guitarset, "run_transcriber",
                                  return_value=[event(60, .1, .5)]):
                    legacy = evaluate_guitarset.evaluate(legacy_args)
                self.assertEqual(legacy["schema_version"], 2)
                self.assertEqual(legacy["model"]["name"], "User-supplied ONNX model")
                self.assertIsNone(legacy["model"]["source_commit"])
                self.assertTrue(legacy["dataset"]["checksums_verified"])
                self.assertEqual(legacy["audio_sample_rates"], [22050])
                self.assertEqual(legacy["timing"]["yin"]["count"], 1)
                # CLI-only builds have no test runner or Catch2 dependency.
                metadata = json.loads(BUILD_INFO.read_text())
                metadata["runner"] = None
                cli_metadata = directory / "evaluation-build-Release.json"
                cli_metadata.write_text(json.dumps(metadata))
                legacy_args.build_info = cli_metadata
                with patch.object(evaluate_guitarset, "AUDIO_MD5", md5_file(audio)), \
                     patch.object(evaluate_guitarset, "ANNOTATION_MD5", md5_file(labels)), \
                     patch.object(evaluate_guitarset, "run_transcriber",
                                  return_value=[event(60, .1, .5)]):
                    self.assertEqual(evaluate_guitarset.evaluate(legacy_args)["build"]["runner"], None)
                    legacy_args.transcriber = RUNNER
                    with self.assertRaisesRegex(ValueError, "does not match"):
                        evaluate_guitarset.evaluate(legacy_args)


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        if RUNNER is None or BUILD_INFO is None:
            self.skipTest("Pass runner and build-info paths to execute engine integrations")

    def test_performance_resources_and_warm_cache(self):
        if PERFORMANCE_RUNNER is None:
            self.skipTest("Pass performance runner to execute resource instrumentation")
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "performance.json"
            subprocess.run([str(PERFORMANCE_RUNNER), str(ROOT / "samples" / "single_a4.wav"),
                            str(output), "spectral", "full", "serial", "3", "15"],
                           check=True, capture_output=True, timeout=30)
            report = json.loads(output.read_text())
            self.assertEqual(len(report["runs"]), 3)
            self.assertFalse(report["runs"][0]["stages"]["spectrogram_cache_hit"])
            self.assertTrue(report["runs"][1]["stages"]["spectrogram_cache_hit"])
            for run in report["runs"]:
                self.assertGreaterEqual(run["wall_seconds"], 0)
                self.assertGreaterEqual(run["cpu_seconds"], 0)
                self.assertGreater(run["peak_resident_bytes"], 0)
                self.assertGreaterEqual(run["cpu_core_equivalents"], 0)
                self.assertGreater(run["audio_seconds"], 0)
            result = subprocess.run([str(PERFORMANCE_RUNNER), str(ROOT / "samples" / "single_a4.wav"),
                                     str(output), "spectral", "full", "serial", "1", "15"],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Repetitions", result.stderr)
            for backend in ("yin", "spectral", "onnx"):
                if backend == "onnx" and not json.loads(BUILD_INFO.read_text())["features"]["onnx"]:
                    continue
                model = str(ROOT / "python" / "models" / "basic_pitch.onnx") if backend == "onnx" else "-"
                subprocess.run([str(PERFORMANCE_RUNNER), str(ROOT / "samples" / "single_a4.wav"),
                                str(output), backend, "model", "serial", "3", "15", model, "cpu"],
                               check=True, capture_output=True, timeout=30)
                report = json.loads(output.read_text())
                interface = report["model_interface"]
                self.assertEqual(interface["completed_inferences"], 3)
                self.assertEqual(interface["live_sessions"], 1 if backend == "onnx" else 0)
                self.assertEqual(report["live_sessions_after_release"], 0)
                self.assertEqual(interface["batch_size"], 1)
                self.assertFalse(interface["incremental"])
                self.assertEqual(interface["provider"], "cpu")
                self.assertGreaterEqual(interface["load_seconds"], 0)

    def test_build_metadata_rejects_wrong_binary(self):
        self.assertIn("compiler", build_identity(BUILD_INFO, RUNNER))
        with self.assertRaises(ValueError):
            build_identity(BUILD_INFO, RUNNER.with_name("wrong-runner"))

    def test_model_interface_preserves_corpus_predictions(self):
        backends = ["yin", "spectral"]
        if json.loads(BUILD_INFO.read_text())["features"]["onnx"]:
            backends.append("onnx")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for backend in backends:
                reports = []
                for interface in (False, True):
                    output = directory / f"{backend}-{interface}.json"
                    command = [sys.executable, str(ROOT / "python" / "evaluate.py"),
                        "--runner", str(RUNNER), "--build-info", str(BUILD_INFO),
                        "--backend", backend, "--mode", "full", "--output-json", str(output),
                        "--warmups", "0", "--repetitions", "1"]
                    if interface:
                        command.append("--model-interface")
                    if backend == "onnx":
                        command.extend(["--model", str(ROOT / "python" / "models" / "basic_pitch.onnx")])
                    completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    reports.append(json.loads(output.read_text()))
                self.assertEqual(reports[0]["corpus"]["sha256"], reports[1]["corpus"]["sha256"])
                for old, new in zip(reports[0]["clips"], reports[1]["clips"]):
                    self.assertEqual(old["id"], new["id"])
                    self.assertEqual(old["predictions"], new["predictions"], backend + ":" + old["id"])
                    self.assertEqual(old["metrics"], new["metrics"])
                    metadata = new["runs"][0]["model_interface"]
                    self.assertEqual(metadata["version"], 1)
                    self.assertEqual(metadata["active_provider"], "cpu")
                    self.assertFalse(metadata["incremental"])
                    self.assertEqual(metadata["batch_size"], 1)
                    if backend == "onnx":
                        self.assertEqual(metadata["inputs"][0]["dimensions"], [-1, 43844, 1])
                        self.assertEqual(len(metadata["outputs"]), 3)
                        self.assertTrue(metadata["runtime_version"])
            if "onnx" in backends:
                predictions = []
                executable = json.loads(BUILD_INFO.read_text())["executable"]
                for interface in (False, True):
                    output = directory / f"cli-onnx-{interface}.json"
                    command = [executable, str(ROOT / "samples" / "single_a4.wav"),
                        "--model", str(ROOT / "python" / "models" / "basic_pitch.onnx"),
                        "--provider", "cpu", "--polyphonic", "--quiet", "--json", str(output),
                        "--musicxml", str(directory / "score.musicxml"), "--tab", str(directory / "tab.txt")]
                    if interface:
                        command.extend(["--backend", "onnx:basic-pitch-v1"])
                    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    predictions.append(json.loads(output.read_text())["notes"])
                self.assertEqual(predictions[0], predictions[1])

    def test_onnx_contract_validation_with_tiny_local_models(self):
        if not json.loads(BUILD_INFO.read_text())["features"]["onnx"]:
            self.skipTest("ONNX build required for tensor-contract validation")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            audio = directory / "one-second.wav"
            with wave.open(str(audio), "wb") as output:
                output.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                output.writeframes(b"\0\0" * 16000)
            model = directory / "contract.onnx"
            output = directory / "prediction.json"
            cases = [
                ({}, True, ""),
                ({"classes": 70}, True, ""),
                ({"inputs": (1, 16000)}, True, ""),
                ({"inputs": (2, None)}, False, "float32 [1,samples]"),
                ({"inputs": (1, None, 1)}, False, "float32 [1,samples]"),
                ({"inputs": (1, 8000)}, False, "length"),
                ({"input_type": 11}, False, "float32"),
                ({"classes": 68}, False, "69|70"),
                ({"output_type": 11}, False, "float32"),
                ({"nonfinite": True}, False, "NaN/Infinity"),
            ]
            for options, success, diagnostic in cases:
                with self.subTest(options=options):
                    write_model(model, **options)
                    output.unlink(missing_ok=True)
                    result = subprocess.run([
                        str(RUNNER), str(audio), str(output), "--backend", "onnx",
                        "--model", str(model), "--mode", "full", "--model-interface",
                        "--model-contract", "legacy-pitch-v1",
                    ], capture_output=True, text=True, timeout=30)
                    if success:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertTrue(json.loads(output.read_text())["notes"])
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn(diagnostic, result.stderr)
                        self.assertFalse(output.exists())
            model.write_bytes(b"invalid ONNX model")
            result = subprocess.run([str(RUNNER), str(audio), str(output), "--backend", "onnx",
                "--model", str(model), "--model-interface"],
                capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Cannot load model", result.stderr)

    def test_temporal_quality_provenance_and_streaming_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)

            def report(backend, temporal=False, mode="full"):
                output = directory / f"{backend}-{temporal}-{mode}.json"
                command = [
                    sys.executable, str(ROOT / "python" / "evaluate.py"),
                    "--runner", str(RUNNER), "--build-info", str(BUILD_INFO),
                    "--backend", backend, "--mode", mode, "--output-json", str(output),
                    "--warmups", "0", "--repetitions", "1", "--serial",
                ]
                if temporal:
                    command.append("--temporal")
                if backend == "onnx":
                    command.extend(["--model", str(ROOT / "python" / "models" / "basic_pitch.onnx")])
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stderr)
                return json.loads(output.read_text(encoding="utf-8"))

            backends = ["yin", "spectral"]
            if json.loads(BUILD_INFO.read_text())["features"]["onnx"]:
                backends.append("onnx")
            for backend in backends:
                legacy = report(backend)
                decoded = report(backend, True)
                streamed = report(backend, True, "streaming") if backend != "onnx" else None
                baseline = json.loads((ROOT / "python" / "evaluation" /
                    f"synthetic-v1-windows-clang23-{backend}-temporal-full.json").read_text())
                self.assertEqual(decoded["corpus"]["sha256"], baseline["corpus"]["sha256"])
                self.assertEqual(decoded["configuration"]["temporal_decoding"],
                                 baseline["configuration"]["temporal_decoding"])
                self.assertEqual(decoded["model"], baseline["model"])
                self.assertGreaterEqual(decoded["summary"]["note_event"]["tp"],
                                        baseline["summary"]["note_event"]["tp"])
                self.assertLessEqual(decoded["summary"]["note_event"]["fp"],
                                     baseline["summary"]["note_event"]["fp"])
                self.assertEqual(legacy["corpus"]["sha256"], decoded["corpus"]["sha256"])
                for metric in ("precision", "recall", "f1"):
                    self.assertGreater(decoded["summary"]["note_event"][metric],
                                       legacy["summary"]["note_event"][metric])
                for metric in ("onset_error_seconds", "offset_error_seconds"):
                    self.assertLess(decoded["summary"][metric]["median"],
                                    legacy["summary"][metric]["median"])
                for index, (old, current) in enumerate(zip(legacy["clips"], decoded["clips"])):
                    chunked = streamed["clips"][index] if streamed else None
                    if chunked:
                        self.assertEqual(current["id"], chunked["id"])
                        self.assertEqual(current["metrics"]["note_event"], chunked["metrics"]["note_event"])
                    if current["id"] in ("repeated", "very_short", "silence", "long_boundary"):
                        for metric in ("precision", "recall", "f1"):
                            self.assertGreaterEqual(current["metrics"]["note_event"][metric],
                                                    old["metrics"]["note_event"][metric])
                    for row in (current, chunked) if chunked else (current,):
                        events = row["canonical_events"]
                        self.assertEqual(len(events), len({e["id"] for e in events}))
                        self.assertEqual(len(events), len(row["predictions"]))
                        self.assertTrue(all(n["confidence"] is None for n in row["predictions"]))
                        for e in events:
                            self.assertEqual(e["audio_source_id"], "sha256:" + row["audio_sha256"])
                            self.assertEqual(e["source_model"], decoded["model"]["identifier"])
                            self.assertIsNone(e["calibrated_confidence"])
                            self.assertIsNone(e["velocity"])
                            self.assertEqual(e["source_sample_rate"], row["sample_rate"])
                            self.assertAlmostEqual(e["onset_seconds"],
                                                   e["onset_sample"] / row["sample_rate"])
                            self.assertAlmostEqual(e["offset_seconds"],
                                                   e["offset_sample"] / row["sample_rate"])
                            times = [f["time_seconds"] for f in e["evidence"]]
                            self.assertEqual(times, sorted(times))
                            self.assertTrue(times)
                            for f in e["evidence"]:
                                self.assertIn("raw_model_value", f)
                                self.assertIsNone(f["fractional_midi"])

    def test_report_end_to_end_and_comparison_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "report.json"
            text = Path(temporary) / "report.txt"
            result = subprocess.run([
                sys.executable, str(ROOT / "python" / "evaluate.py"),
                "--runner", str(RUNNER), "--build-info", str(BUILD_INFO),
                "--output-json", str(output), "--output-text", str(text),
                "--warmups", "0", "--repetitions", "2", "--serial",
            ], capture_output=True, text=True, timeout=150)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(text.read_text(encoding="utf-8"), human_report(report))
            self.assertEqual(len(report["clips"]), 16)
            self.assertEqual(report["timing_protocol"]["repetitions"], 2)
            for row in report["clips"]:
                self.assertEqual(len(row["runs"]), 2)
                self.assertEqual(row["sample_rate"], 22050)
                self.assertIn("frame", row["metrics"])
                self.assertIn("note_event", row["metrics"])
            for name in ("empty", "silence"):
                row = next(row for row in report["clips"] if row["id"] == name)
                self.assertEqual(row["predictions"], [])
            delta = compare(report, report)
            self.assertEqual(delta["event_f1_delta"], 0)
            self.assertEqual(delta["median_process_time_ratio"], 1)
            for key in ("configuration", "tolerances", "model", "corpus", "timing_protocol"):
                changed = copy.deepcopy(report)
                changed[key] = {} if key != "corpus" else {"sha256": "changed"}
                with self.assertRaises(ValueError):
                    compare(report, changed)
            baseline = Path(temporary) / "baseline.json"
            baseline.write_text(json.dumps(report))
            candidate = copy.deepcopy(report)
            args = SimpleNamespace(compare=baseline, output_json=output, output_text=text,
                                   max_event_f1_drop=0)
            with patch.object(evaluate, "parse_args", return_value=args), \
                 patch.object(evaluate, "evaluate", return_value=candidate), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(evaluate.main(), 0)
                candidate["summary"]["note_event"]["f1"] -= .01
                self.assertEqual(evaluate.main(), 1)
                self.assertIn("regression exceeds", errors.getvalue())
            self.assertLess(json.loads(output.read_text())["comparison"]["event_f1_delta"], 0)

    def test_runner_errors_are_failures_not_empty_predictions(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            broken = directory / "broken.wav"
            broken.write_bytes(b"not RIFF")
            output = directory / "out.json"
            for extra in ([], ["--backend", "unknown"], ["--chunk-seconds", "nan"]):
                result = subprocess.run([str(RUNNER), str(broken), str(output), *extra],
                                        capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertFalse(output.exists())
            source = 'audio:"quoted"\\path\ncontrol:\x01'
            subprocess.run([str(RUNNER), str(ROOT / "samples" / "single_a4.wav"), str(output),
                            "--temporal", "--audio-source-id", source],
                           check=True, capture_output=True, timeout=30)
            decoded = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(decoded["canonical_events"])
            self.assertEqual(decoded["canonical_events"][0]["audio_source_id"], source)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
