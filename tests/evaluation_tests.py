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

RUNNER = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
BUILD_INFO = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None


def event(pitch=69, start=0.0, duration=1.0):
    return dict(midi=pitch, start=start, duration=duration)


class MetricsTests(unittest.TestCase):
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

    def test_build_metadata_rejects_wrong_binary(self):
        self.assertIn("compiler", build_identity(BUILD_INFO, RUNNER))
        with self.assertRaises(ValueError):
            build_identity(BUILD_INFO, RUNNER.with_name("wrong-runner"))

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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
