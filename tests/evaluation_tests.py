"""Unit and engine integration tests for the evaluation contract (stdlib only)."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import itertools
import json
import math
import os
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
from evaluation_metrics import (Tolerances, distribution, event_pairs, frame_counts, score_events,
                                fit_event_calibration, apply_event_calibration,
                                calibrated_event_confidence)
from synthetic_corpus import generate, specifications
from evaluate import build_identity, compare, human_report, markdown_report
import evaluate
from evaluate_guitarset import md5_file, read_reference_events
import evaluate_guitarset
sys.path.insert(0, str(ROOT / "python" / "models"))
from acquire import acquire
from performance import compare_reports, percentile, long_report_markdown, summarize_timings
from evaluation_environment import host_identity
from quality_release_gate import compare as compare_release_quality
from release_report import MANUAL_CHECKS, create_report, manual_evidence, test_evidence
from onnx_fixtures import write_model

RUNNER = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
BUILD_INFO = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
PERFORMANCE_RUNNER = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None


def event(pitch=69, start=0.0, duration=1.0):
    return dict(midi=pitch, start=start, duration=duration)


class MetricsTests(unittest.TestCase):
    def test_explicit_model_acquisition_preserves_existing_and_cleans_bad_download(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "model.onnx"
            with self.assertRaises(FileNotFoundError):
                acquire(output)
            output.write_bytes(b"not a model")
            with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
                acquire(output, download=True)
            self.assertEqual(output.read_bytes(), b"not a model")
            output.unlink()
            response = io.BytesIO(b"bad download")
            response.geturl = lambda: "https://example.invalid/pinned"
            with patch("urllib.request.urlopen", return_value=response):
                with self.assertRaisesRegex(ValueError, "size/checksum"):
                    acquire(output, download=True)
            self.assertEqual(list(output.parent.iterdir()), [])
        acquire(ROOT / "python" / "models" / "basic_pitch.onnx")

    def calibration_report(self):
        return dict(configuration=dict(backend="onnx", dataset_split="validation"),
                    model=dict(sha256="model"), evaluator=dict(source="fixed"),
                    corpus=dict(name="hand", version=1),
                    tolerances=dict(onset_seconds=.05, offset_seconds=.05,
                                    offset_ratio=.2, frame_seconds=.01),
                    clips=[dict(dataset_split="validation", performer_id="04", audio_sha256="validation",
                                references=[event()],
                                predictions=[dict(event(), confidence=.9),
                                             dict(event(pitch=60), confidence=.1)])])

    def test_isotonic_confidence_uses_strict_one_to_one_events(self):
        report = self.calibration_report()
        report["clips"][0]["predictions"].append(dict(event(), confidence=.9))
        artifact = fit_event_calibration(report)
        self.assertEqual(artifact["samples"], 3)
        self.assertEqual(artifact["positives"], 1)
        self.assertEqual(calibrated_event_confidence(.1, artifact), 0)
        self.assertEqual(calibrated_event_confidence(.9, artifact), .5)
        self.assertEqual(calibrated_event_confidence(1, artifact), .5)
        self.assertEqual(calibrated_event_confidence(-1, artifact), 0)

    def test_isotonic_pools_nonmonotonic_scores_and_rejects_missing_labels(self):
        report = self.calibration_report()
        predictions = report["clips"][0]["predictions"]
        predictions[0]["confidence"], predictions[1]["confidence"] = .1, .9
        artifact = fit_event_calibration(report)
        self.assertEqual(artifact["probabilities"], [.5])
        for split in ("test", "train", "all", None):
            report["clips"][0]["dataset_split"] = split
            with self.assertRaisesRegex(ValueError, "validation-only"):
                fit_event_calibration(report)
        report["clips"][0]["dataset_split"] = "validation"
        predictions[0]["confidence"] = None
        with self.assertRaises(ValueError):
            fit_event_calibration(report)

    def test_calibration_assessment_preserves_events_and_checks_disjointness(self):
        report = self.calibration_report()
        artifact = fit_event_calibration(report)
        report["configuration"]["dataset_split"] = "test"
        clip = report["clips"][0]
        clip.update(dataset_split="test", performer_id="05", audio_sha256="test")
        result = apply_event_calibration(report, artifact)
        self.assertEqual(result["brier"], 0)
        self.assertEqual(result["expected_calibration_error"], 0)
        self.assertEqual(len(clip["predictions"]), 2)
        clip["audio_sha256"] = "validation"
        with self.assertRaisesRegex(ValueError, "audio overlap"):
            apply_event_calibration(report, artifact)
        clip["audio_sha256"] = "test"
        clip["performer_id"] = "04"
        with self.assertRaisesRegex(ValueError, "performer overlap"):
            apply_event_calibration(report, artifact)
        with self.assertRaises(ValueError):
            calibrated_event_confidence(float("nan"), artifact)
        artifact["probabilities"] = [1, 0]
        with self.assertRaises(ValueError):
            calibrated_event_confidence(.5, artifact)

    def test_performance_percentiles_interpolate_hand_verified_samples(self):
        self.assertEqual(percentile([2], .95), 2)
        self.assertEqual(percentile([4, 1, 2], .5), 2)
        self.assertAlmostEqual(percentile([1, 3], .95), 2.9)
        self.assertEqual(summarize_timings([2])["stability"], "descriptive")
        self.assertAlmostEqual(summarize_timings([0, 10, 20])["p99"], 19.8)
        with self.assertRaisesRegex(ValueError, "without samples"):
            percentile([], .95)

    def test_host_identity_records_cpu_gpu_and_platform_inventory(self):
        identity = host_identity()
        for key in ("platform", "machine", "cpu_model", "logical_cpus", "gpu_inventory"):
            self.assertIn(key, identity)
        self.assertIn(identity["gpu_inventory"]["status"],
                      ("detected", "none_reported", "not_detected", "error"))

    def test_long_report_exposes_per_input_quality_and_non_equivalence(self):
        row = dict(id="hand", backend="yin", mode="bounded", median_seconds=2,
                   p95_seconds=3, p99_seconds=3, realtime_factor=.2,
                   full_buffer_notes_identical=False,
                   cold_run=dict(wall_seconds=2),
                   timing_percentiles_seconds=dict(p50=2),
                   runs=[dict(peak_resident_bytes=1048576)],
                   metrics=score_events([event()], [event()], 1))
        rendered = long_report_markdown(dict(protocol="hand verified", clips=[row]))
        self.assertIn("Event F1", rendered)
        self.assertIn("Extra fragments", rendered)
        self.assertIn("| False |", rendered)
        self.assertIn("| 1.0000 | 1.0000 | 1.0000 |", rendered)

    def test_polyphonic_corpus_preserves_v2_and_labels_extended_scenarios(self):
        old = specifications(2)
        new = specifications(3)
        self.assertEqual(new[:len(old)], old)
        self.assertEqual(len(new), 30)
        ids = {clip["id"]: clip for clip in new}
        for name in ("dyad_fifth", "dense_chord", "octave_interval", "bass_melody",
                     "distorted_guitar_fixture", "release_tails", "adjacent_semitones"):
            self.assertIn("polyphonic", ids[name]["scenarios"])
            self.assertGreater(len(ids[name]["notes"]), 1)
        self.assertIn("distorted_synthetic", ids["distorted_guitar_fixture"]["scenarios"])

    def test_distribution_reports_hand_checked_percentiles(self):
        result = distribution(list(range(1, 101)))
        self.assertEqual((result["median"], result["p90"], result["p95"], result["p99"]),
                         (50.5, 90, 95, 99))
        self.assertIsNone(distribution([])["p99"])

    def test_performance_comparison_handles_clock_resolution_and_rejects_drift(self):
        row = dict(id="empty", backend="yin", mode="full", audio_sha256="same",
                   notes=[], metrics=score_events([], [], 0),
                   cold_run=dict(wall_seconds=0),
                   runs=[dict(wall_seconds=0), dict(wall_seconds=0)])
        before = dict(measurement_protocol_version=2, configuration={}, clips=[row],
                      build=dict(build_type="Release",
                                 compiler=dict(id="Clang", version="1"),
                                 flags={}, features={}, generator="Ninja"),
                      environment=dict(platform="host", machine="x64", cpu_model="cpu",
                                       logical_cpus=8, gpu_inventory={"status": "not_detected"}))
        current = copy.deepcopy(before)
        compare_reports(before, current)
        self.assertTrue(current["all_notes_identical"])
        self.assertTrue(current["comparison_context"]["timing_comparable"])
        self.assertIsNone(current["clips"][0]["comparison"]["cold_speedup"])
        changed = copy.deepcopy(before)
        changed["clips"][0]["notes"] = [event()]
        compare_reports(before, changed)
        self.assertFalse(changed["all_notes_identical"])
        changed["clips"][0]["audio_sha256"] = "different"
        with self.assertRaisesRegex(ValueError, "audio differs"):
            compare_reports(before, changed)
        incompatible_protocol = copy.deepcopy(before)
        incompatible_protocol["measurement_protocol_version"] = 1
        with self.assertRaisesRegex(ValueError, "protocols differ"):
            compare_reports(before, incompatible_protocol)

    def test_quality_release_gate_compares_every_fixture_and_rejects_config_drift(self):
        baseline_path = ROOT / "python" / "evaluation" / \
            "synthetic-v1-windows-clang23-yin-streaming.json"
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
        candidate = copy.deepcopy(baseline)
        candidate["schema_version"] = 2
        candidate["configuration"].update(polyphonic_decoder="legacy", model_objective=None)
        result = compare_release_quality(candidate, baseline, .01, .01, .01)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(len(result["fixtures"]), len(baseline["clips"]))
        regressed = copy.deepcopy(candidate)
        regressed["clips"][0]["metrics"]["note_event"]["f1"] -= .02
        failed = compare_release_quality(regressed, baseline, .01, .01, .01)
        self.assertEqual(failed["status"], "fail")
        self.assertTrue(any(regressed["clips"][0]["id"] in failure
                            for failure in failed["failures"]))
        incompatible = copy.deepcopy(candidate)
        incompatible["configuration"]["minimum_confidence"] = .3
        with self.assertRaisesRegex(ValueError, "configuration differs"):
            compare_release_quality(incompatible, baseline, .01, .01, .01)

    def test_manual_acceptance_and_ctest_junit_evidence_are_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            acceptance = Path(temporary) / "manual.json"
            acceptance.write_text(json.dumps({
                "schema_version": 1,
                "checks": {name: {"status": "not_run"} for name in MANUAL_CHECKS},
            }), encoding="utf-8")
            report, pending = manual_evidence(acceptance)
            self.assertEqual(report["status"], "pending")
            self.assertTrue(pending)
            junit = Path(temporary) / "ctest.xml"
            junit.write_text(
                '<testsuites><testsuite><testcase name="ok"/>'
                '<testcase name="skip"><skipped message="needs fixture"/></testcase>'
                '<testcase name="bad"><failure message="assertion failed"/></testcase>'
                '</testsuite></testsuites>',
                encoding="utf-8")
            evidence = test_evidence(junit)
            self.assertEqual((evidence["status"], evidence["tests"], evidence["passed"],
                              evidence["failed"], evidence["skipped"]),
                             ("fail", 3, 1, 1, 1))
            self.assertEqual(evidence["failed_tests"][0]["name"], "bad")
            self.assertEqual(evidence["skipped_tests"][0]["details"], "needs fixture")

    def test_release_report_stays_pending_when_release_evidence_is_missing(self):
        args = SimpleNamespace(
            quality=[], quality_gate=[], performance=[], ctest_junit=None,
            manual_acceptance=None, change_summary="unit test", limitation=[],
        )
        report = create_report(args)
        self.assertEqual(report["overall_status"], "pending")
        self.assertEqual(report["automated_tests"]["status"], "pending")
        self.assertTrue(report["unresolved_limitations"])

    def test_matched_performance_slowdown_requires_disposition(self):
        with tempfile.TemporaryDirectory() as temporary:
            performance = Path(temporary) / "performance.json"
            performance.write_text(json.dumps({
                "schema_version": 2,
                "measurement_protocol_version": 2,
                "build": {"build_type": "Release"},
                "environment": {},
                "configuration": {"model_identifier": "builtin:yin"},
                "comparison_context": {"timing_comparable": True},
                "clips": [{
                    "id": "case",
                    "backend": "yin",
                    "mode": "full",
                    "comparison": {"warm_speedup": .95},
                }],
            }), encoding="utf-8")
            args = SimpleNamespace(
                quality=[], quality_gate=[], performance=[performance], ctest_junit=None,
                manual_acceptance=None, change_summary="unit test", limitation=[],
            )
            report = create_report(args)
            self.assertEqual(report["performance_review"][0]["status"], "observed_slowdown")
            self.assertEqual(report["overall_status"], "pending")
            self.assertTrue(any("slowdown" in item
                                for item in report["unresolved_limitations"]))

    def test_perfect_events_and_empty_convention(self):
        result = score_events([event()], [event()], 1)
        for metric in ("note_event", "onset_pitch", "pitch_event", "frame"):
            self.assertEqual(result[metric]["f1"], 1)
        empty = score_events([], [], 1)
        self.assertEqual(empty["note_event"]["fp"], 0)
        self.assertEqual(empty["note_event"]["f1"], 0)
        self.assertEqual(empty["pitch_event"]["f1"], 0)
        self.assertEqual(empty["false_positives_per_minute"], 0)
        self.assertIsNone(score_events([], [], 0)["false_positives_per_minute"])
        self.assertIsNone(empty["pitch_accuracy"]["value"])
        self.assertIsNone(empty["onset_error_seconds"]["mean"])
        missed = score_events([event()], [], 60)
        self.assertEqual(missed["note_event"], dict(
            tp=0, fp=0, fn=1, precision=0, recall=0, f1=0))
        self.assertEqual(missed["false_positives_per_minute"], 0)
        extra = score_events([], [event()], 60)
        self.assertEqual(extra["note_event"], dict(
            tp=0, fp=1, fn=0, precision=0, recall=0, f1=0))
        self.assertEqual(extra["false_positives_per_minute"], 1)

    def test_pitch_events_are_overlap_matched_and_one_to_one(self):
        reference = [event(60, 0, 2), event(64, 0, 2)]
        predictions = [event(60, 1, 1), event(60, 1, 1), event(65, 0, 2)]
        result = score_events(reference, predictions, 2)
        self.assertEqual(result["pitch_event"], dict(
            tp=1, fp=2, fn=1, precision=1 / 3, recall=.5, f1=.4))
        self.assertEqual(len(event_pairs(
            [event(60, 0, 2)], [event(60, 1, 1)],
            require_onset=False, require_overlap=True)), 1)
        self.assertFalse(event_pairs(
            [event(60, 0, 1)], [event(60, 1, 1)],
            require_onset=False, require_overlap=True))

    def test_fragmentation_merging_and_false_positive_rate(self):
        fragmented = score_events(
            [event(60, 0, 1)],
            [event(60, 0, .5), event(60, .5, .5), event(61, 0, .5)],
            30,
        )
        self.assertEqual(fragmented["note_fragmentation"], dict(
            fragmented_reference_notes=1, extra_predicted_fragments=1, rate=1))
        self.assertEqual(fragmented["note_merging"], dict(
            merged_predictions=0, extra_reference_notes=0, rate=0))
        self.assertAlmostEqual(fragmented["false_positives_per_minute"], 6)

        merged = score_events(
            [event(60, 0, .5), event(60, .5, .5)],
            [event(60, 0, 1)],
            60,
        )
        self.assertEqual(merged["note_merging"], dict(
            merged_predictions=1, extra_reference_notes=1, rate=1))
        self.assertEqual(merged["note_fragmentation"]["fragmented_reference_notes"], 0)
        overlapping_references = score_events(
            [event(60, 0, .75), event(60, .25, .75)],
            [event(60, 0, 1)],
            1,
        )
        self.assertEqual(overlapping_references["note_merging"]["merged_predictions"], 0)

    def test_tolerances_are_explicit_and_inclusive(self):
        tolerances = Tolerances(.05, .05, 0, .01)
        self.assertEqual(len(event_pairs([event()], [event(start=.05)],
                                        require_offset=True, tolerances=tolerances)), 1)
        self.assertEqual(len(event_pairs([event()], [event(start=.05, duration=1)],
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
            self.assertEqual(first["sha256"],
                             "183c7f68643d6ac54a1b15a26af195e16590f115c5194f25c139366844f6fca9")
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

    def test_corpus_v2_scenarios_are_deterministic_and_test_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = generate(root / "v2-a", version=2)
            second = generate(root / "v2-b", version=2)
            self.assertEqual(first, second)
            self.assertEqual(first["version"], 2)
            self.assertEqual(len(first["clips"]), 22)
            self.assertEqual(first["dataset_split_policy"]["training"], [])
            self.assertEqual(first["dataset_split_policy"]["validation"], [])
            self.assertEqual(len(first["dataset_split_policy"]["test"]), 22)
            fixtures = {clip["id"]: clip for clip in first["clips"]}
            for name in ("sustained", "pitch_bend", "harmonics_only",
                         "register_low", "register_mid", "register_high"):
                self.assertEqual(fixtures[name]["dataset_split"], "test")
                self.assertTrue(fixtures[name]["notes"])
                self.assertAlmostEqual(
                    fixtures[name]["notes"][0]["offset"] -
                    fixtures[name]["notes"][0]["onset"],
                    fixtures[name]["notes"][0]["duration"],
                )
                self.assertEqual(
                    (root / "v2-a" / fixtures[name]["audio"]).read_bytes(),
                    (root / "v2-b" / fixtures[name]["audio"]).read_bytes(),
                )
            self.assertIn("vibrato", fixtures["vibrato"]["scenarios"])
            self.assertIn("chords", fixtures["chord"]["scenarios"])
            self.assertIn("repeated_notes", fixtures["repeated"]["scenarios"])
            self.assertIn("noisy_recording", fixtures["noise"]["scenarios"])

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
            self.assertEqual(corpus["clips"][0]["dataset_split"], "unassigned")
            self.assertEqual(corpus["clips"][0]["notes"][0]["instrument"], "guitar")
            self.assertIsNone(corpus["clips"][0]["notes"][0]["velocity"])
            self.assertTrue((destination / "real-0.wav").is_file())
            self.assertEqual(corpus["dataset_split_policy"]["partitions"]["train"],
                             ["00", "01", "02", "03"])
            self.assertEqual(corpus["dataset_split_policy"]["partitions"]["validation"], ["04"])
            self.assertEqual(corpus["dataset_split_policy"]["partitions"]["test"], ["05"])
            self.assertEqual(evaluate_guitarset.guitarset_dataset_split("00_comp"), "train")
            self.assertEqual(evaluate_guitarset.guitarset_dataset_split("04_solo"), "validation")
            self.assertEqual(evaluate_guitarset.guitarset_dataset_split("05_comp"), "test")
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
            with patch.dict(os.environ, {"TRANSCRIBE_PERFORMANCE_WARMUPS": "1"}):
                subprocess.run([str(PERFORMANCE_RUNNER), str(ROOT / "samples" / "single_a4.wav"),
                                str(output), "spectral", "full", "serial", "3", "15"],
                               check=True, capture_output=True, timeout=30)
            report = json.loads(output.read_text())
            self.assertEqual(len(report["runs"]), 3)
            self.assertEqual(report["warmup_runs"], 1)
            self.assertFalse(report["cold_run"]["stages"]["spectrogram_cache_hit"])
            self.assertTrue(report["runs"][0]["stages"]["spectrogram_cache_hit"])
            for run in [report["cold_run"], *report["runs"]]:
                self.assertGreaterEqual(run["wall_seconds"], 0)
                self.assertGreaterEqual(run["cpu_seconds"], 0)
                self.assertGreater(run["peak_resident_bytes"], 0)
                self.assertGreaterEqual(run["cpu_core_equivalents"], 0)
                self.assertGreater(run["audio_seconds"], 0)
                self.assertGreater(run["audio_seconds_per_second"], 0)
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
                self.assertEqual(interface["completed_inferences"], 4)
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
                        "--model-objective", "independent-multi-pitch",
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

    def test_model_objective_and_basic_pitch_optional_heads_are_validated(self):
        if not json.loads(BUILD_INFO.read_text())["features"]["onnx"]:
            self.skipTest("ONNX build required")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            model = directory / "contract.onnx"
            output = directory / "prediction.json"
            audio = ROOT / "samples" / "single_a4.wav"
            write_model(model)
            command = [str(RUNNER), str(audio), str(output), "--backend", "onnx",
                       "--model", str(model), "--model-interface",
                       "--model-contract", "legacy-pitch-v1"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("training objective", result.stderr)
            result = subprocess.run(command + ["--model-objective", "monophonic"],
                                    capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("training objective", result.stderr)
            for name, classes, kind in (("StatefulPartitionedCall:2", 87, 1),
                                        ("StatefulPartitionedCall:0", 263, 1),
                                        ("StatefulPartitionedCall:0", 264, 11)):
                write_model(model, inputs=(1, 43844, 1), classes=88, frames=172,
                            output_name="StatefulPartitionedCall:1",
                            additional_heads=[(name, 172, classes, kind)])
                output.unlink(missing_ok=True)
                result = subprocess.run([
                    str(RUNNER), str(audio), str(output), "--backend", "onnx",
                    "--model", str(model), "--model-interface",
                    "--model-contract", "basic-pitch-v1",
                ], capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)
                self.assertIn("float32", result.stderr)
                self.assertFalse(output.exists())

    def test_neural_event_mode_requires_onsets_and_session_timing_is_explicit(self):
        if not json.loads(BUILD_INFO.read_text())["features"]["onnx"]:
            self.skipTest("ONNX build required")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            model, output = directory / "note-only.onnx", directory / "output.json"
            write_model(model, inputs=(1, 43844, 1), classes=88, frames=172,
                        output_name="StatefulPartitionedCall:1")
            command = [str(RUNNER), str(ROOT / "samples" / "single_a4.wav"), str(output),
                       "--backend", "onnx", "--model", str(model), "--model-interface"]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            metadata = json.loads(output.read_text())["model_interface"]
            self.assertEqual([h["available"] for h in metadata["heads"]], [True, False, False])
            output.unlink()
            completed = subprocess.run(command + ["--neural-decoder", "events"],
                                       capture_output=True, text=True, timeout=30)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("onset head", completed.stderr)
            self.assertFalse(output.exists())
            command[command.index(str(model))] = str(ROOT / "python" / "models" / "basic_pitch.onnx")
            completed = subprocess.run(command + ["--neural-decoder", "events",
                                                  "--session-warmups", "1", "--session-repetitions", "3"],
                                       capture_output=True, text=True, timeout=60)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            data = json.loads(output.read_text())
            self.assertEqual(len(data["session_inference_seconds"]), 3)
            self.assertTrue(all(t > 0 for t in data["session_inference_seconds"]))
            self.assertGreater(data["session_warmup_seconds"], 0)
            completed = subprocess.run(command + ["--neural-decoder", "quality"],
                                       capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(output.read_text())["notes"], data["notes"])

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
                configuration = decoded["configuration"]["temporal_decoding"]
                for key, value in baseline["configuration"]["temporal_decoding"].items():
                    self.assertEqual(configuration[key], value)
                additions = {"pitch_stability_seconds": .05, "pitch_tolerance_semitones": .75,
                             "onset_threshold": .5, "minimum_onset_spacing_seconds": .07,
                             "onset_energy_rise": .25, "onset_state_drop": .15,
                             "use_model_onsets": True}
                self.assertEqual(set(configuration) -
                                 set(baseline["configuration"]["temporal_decoding"]),
                                 set(additions))
                for key, value in additions.items():
                    self.assertEqual(configuration[key], value)
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
                                if backend == "yin":
                                    self.assertTrue(math.isfinite(f["fractional_midi"]))
                                else:
                                    self.assertIsNone(f["fractional_midi"])

    def test_report_end_to_end_and_comparison_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "report.json"
            text = Path(temporary) / "report.txt"
            markdown = Path(temporary) / "report.md"
            result = subprocess.run([
                sys.executable, str(ROOT / "python" / "evaluate.py"),
                "--runner", str(RUNNER), "--build-info", str(BUILD_INFO),
                "--output-json", str(output), "--output-text", str(text),
                "--output-markdown", str(markdown),
                "--warmups", "0", "--repetitions", "2", "--serial",
            ], capture_output=True, text=True, timeout=150)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(text.read_text(encoding="utf-8"), human_report(report))
            self.assertEqual(markdown.read_text(encoding="utf-8"), markdown_report(report))
            self.assertEqual(len(report["clips"]), 16)
            self.assertEqual(report["timing_protocol"]["repetitions"], 2)
            for row in report["clips"]:
                self.assertEqual(len(row["runs"]), 2)
                self.assertEqual(row["sample_rate"], 22050)
                self.assertIn("frame", row["metrics"])
                self.assertIn("pitch_event", row["metrics"])
                self.assertIn("note_event", row["metrics"])
                self.assertIn("scenarios", row)
                self.assertIn("peak_resident_bytes", row["runs"][0])
            for name in ("empty", "silence"):
                row = next(row for row in report["clips"] if row["id"] == name)
                self.assertEqual(row["predictions"], [])
            delta = compare(report, report)
            self.assertEqual(delta["event_f1_delta"], 0)
            self.assertEqual(delta["event_f1_regressed_clips"], [])
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
                                   output_markdown=None, backend="yin",
                                   max_event_f1_drop=0)
            with patch.object(evaluate, "parse_args", return_value=args), \
                 patch.object(evaluate, "evaluate", return_value=candidate), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(evaluate.main(), 0)
                candidate["summary"]["note_event"]["f1"] -= .01
                self.assertEqual(evaluate.main(), 1)
                self.assertIn("aggregate", errors.getvalue())
                candidate["summary"]["note_event"]["f1"] = report["summary"]["note_event"]["f1"]
                nonempty = next(row for row in candidate["clips"] if row["references"])
                nonempty["metrics"]["note_event"]["f1"] -= .1
                self.assertEqual(evaluate.main(), 1)
                self.assertIn(nonempty["id"], errors.getvalue())
            comparison = json.loads(output.read_text())["comparison"]
            self.assertLess(comparison["worst_clip_event_f1_delta"], 0)
            self.assertIn(nonempty["id"], comparison["event_f1_regressed_clips"])

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
