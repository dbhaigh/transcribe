"""Dependency-free isolation/paired-metric tests, plus optional real ONNX contract test."""

from array import array
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from separate import isolate_wav, LocalOnnxSeparator, SeparationCancelled, validate_contract
from separation_evaluation import artifact_metrics, paired_corpus, paired_comparisons
from evaluation_metrics import score_events
from synthetic_corpus import generate_mixtures


class SeparationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="transcriber-isolation-test-")
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)
        self.source = self.root / "original.wav"
        self.output = self.root / "isolated.wav"
        self.contract = dict(schema_version=1, contract="waveform-stems-v1", sample_rate=8000,
                             channels=1, window_frames=128, context_frames=16,
                             stems=["guitar", "bass", "piano", "voice"], latency_samples=0)
        self.values = array("h", [0] * 1301)
        # Source disappears/reappears at different positions, including window edges.
        for index in (0, 95, 96, 112, 383, 800, 1300):
            self.values[index] = 16000
        self.write(self.source, self.values)

    def write(self, path, values, channels=1, rate=8000):
        with wave.open(str(path), "wb") as out:
            out.setnchannels(channels)
            out.setsampwidth(2)
            out.setframerate(rate)
            data = array("h", values)
            if sys.byteorder != "little":
                data.byteswap()
            out.writeframes(data.tobytes())

    def test_context_edges_and_source_changes_preserve_every_sample(self):
        for target in self.contract["stems"]:
            self.output.unlink(missing_ok=True)
            report = isolate_wav(self.source, self.output, self.contract, target, lambda audio: audio, warmups=2)
            with wave.open(str(self.output), "rb") as out, wave.open(str(self.source), "rb") as original:
                self.assertEqual(out.readframes(out.getnframes()), original.readframes(original.getnframes()))
            self.assertEqual(report["frames"], 1301)
            self.assertEqual(report["origin_sample"], 0)
            self.assertEqual(report["window_count"], 14)
            self.assertEqual(report["clipped_samples"], 0)

    def test_stereo_alignment_and_short_final_window(self):
        self.contract["channels"] = 2
        self.write(self.source, [100, -100] * 41, 2)
        report = isolate_wav(self.source, self.output, self.contract, "voice", lambda audio: audio)
        self.assertEqual(report["frames"], 41)
        with wave.open(str(self.output), "rb") as out, wave.open(str(self.source), "rb") as original:
            self.assertEqual(out.readframes(41), original.readframes(41))

    def test_cancel_during_inference_cleans_partial_output(self):
        cancelled = False
        def infer(audio):
            nonlocal cancelled
            cancelled = True
            return audio
        with self.assertRaises(SeparationCancelled):
            isolate_wav(self.source, self.output, self.contract, "bass", infer, cancel=lambda: cancelled)
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".isolation-*")))

    def test_failed_inference_is_not_a_success_shaped_fallback(self):
        def fail(audio):
            raise RuntimeError("inference failed")
        with self.assertRaisesRegex(RuntimeError, "inference failed"):
            isolate_wav(self.source, self.output, self.contract, "guitar", fail)
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".isolation-*")))

    def test_invalid_output_shapes_and_nonfinite_values(self):
        for infer in (lambda audio: [], lambda audio: [[0]], lambda audio: [[float("nan")] * 128]):
            with self.assertRaises(ValueError):
                isolate_wav(self.source, self.output, self.contract, "guitar", infer)
            self.assertFalse(self.output.exists())

    def test_resource_rate_and_channel_limits(self):
        with self.assertRaises(ValueError):
            isolate_wav(self.source, self.output, self.contract, "guitar", lambda x: x, max_buffer_bytes=1)
        self.contract["sample_rate"] = 16000
        with self.assertRaises(ValueError):
            isolate_wav(self.source, self.output, self.contract, "guitar", lambda x: x)
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".isolation-*")))

    def test_refuses_existing_files_and_invalid_latency_contract(self):
        self.output.write_bytes(b"preserve")
        with self.assertRaises(FileExistsError):
            isolate_wav(self.source, self.output, self.contract, "guitar", lambda x: x)
        self.assertEqual(self.output.read_bytes(), b"preserve")
        self.contract["latency_samples"] = 1
        with self.assertRaises(ValueError):
            validate_contract(self.contract)
        with self.assertRaises(ValueError):
            validate_contract([])

    def test_source_mutation_during_inference_is_rejected(self):
        def change(audio):
            with self.source.open("ab") as out:
                out.write(b"x")
            return audio
        with self.assertRaisesRegex(RuntimeError, "source changed"):
            isolate_wav(self.source, self.output, self.contract, "piano", change)
        self.assertFalse(self.output.exists())

    def test_clipping_is_reported(self):
        report = isolate_wav(self.source, self.output, self.contract, "piano",
                             lambda audio: [[2.] * 128])
        self.assertEqual(report["clipped_samples"], 1301)

    def test_artifact_cases_are_finite_and_explicit(self):
        exact = artifact_metrics(self.source, self.source)
        self.assertEqual(exact["status"], "exact-up-to-scale")
        self.assertIsNone(exact["si_sdr_db"])
        self.write(self.output, [0] * len(self.values))
        silent = artifact_metrics(self.source, self.output)
        self.assertEqual(silent["status"], "silent-estimate")
        self.assertIsNone(silent["si_sdr_db"])
        self.assertEqual(artifact_metrics(self.output, self.source)["status"], "silent-reference")
        noise = [100 if i % 2 else -100 for i in range(len(self.values))]
        self.write(self.output, [value + n for value, n in zip(self.values, noise)])
        self.assertGreater(artifact_metrics(self.source, self.output)["si_sdr_db"], 10)

    def test_paired_corpus_and_quality_regression_remain_per_pair(self):
        manifest = generate_mixtures(self.root)
        corpus = paired_corpus(manifest)
        self.assertEqual(len(corpus["clips"]), 16)
        rows = []
        for clip in corpus["clips"]:
            predictions = clip["notes"] if clip["variant"] == "original" else []
            rows.append(dict(clip, metrics=score_events(clip["notes"], predictions, clip["duration_seconds"])))
        comparisons = paired_comparisons(rows)
        self.assertEqual(len(comparisons), 8)
        self.assertTrue(all("note_event_f1" in row["degraded_metrics"] for row in comparisons))

    def test_annotations_split_and_alignment_are_checked(self):
        manifest = generate_mixtures(self.root)
        original = json.loads(manifest.read_text())
        for key, value in (("dataset_split", "invalid"), ("license", ""), ("target", "drums")):
            document = copy.deepcopy(original)
            document["pairs"][0][key] = value
            manifest.write_text(json.dumps(document))
            with self.assertRaises(ValueError):
                paired_corpus(manifest)
        document = copy.deepcopy(original)
        document["pairs"][1]["dataset_split"] = "validation"
        manifest.write_text(json.dumps(document))
        with self.assertRaisesRegex(ValueError, "crosses dataset splits"):
            paired_corpus(manifest)
        document = copy.deepcopy(original)
        document["pairs"][0]["notes"][0]["duration"] = 100
        manifest.write_text(json.dumps(document))
        with self.assertRaises(ValueError):
            paired_corpus(manifest)

    def test_paired_identity_is_independent_of_temporary_directory(self):
        manifest = generate_mixtures(self.root)
        other = self.root / "another-location"
        other.mkdir()
        another = generate_mixtures(other)
        left, right = paired_corpus(manifest), paired_corpus(another)
        self.assertEqual(left["sha256"], right["sha256"])
        self.assertEqual([clip["sha256"] for clip in left["clips"]],
                         [clip["sha256"] for clip in right["clips"]])

    def test_real_onnx_waveform_contract_and_missing_provider(self):
        try:
            import onnx
            import numpy
            import onnxruntime
        except ImportError:
            self.skipTest("Optional onnx/numpy/onnxruntime fixture dependencies unavailable")
        from onnx import helper, TensorProto
        # Identity fixture tests runtime wiring only; it does not perform source separation.
        self.contract["stems"] = ["guitar"]
        input_info = helper.make_tensor_value_info("audio", TensorProto.FLOAT, [1, 1, 128])
        output_info = helper.make_tensor_value_info("stems", TensorProto.FLOAT, [1, 1, 1, 128])
        shape = helper.make_tensor("shape", TensorProto.INT64, [4], [1, 1, 1, 128])
        node = helper.make_node("Reshape", ["audio", "shape"], ["stems"])
        graph = helper.make_graph([node], "identity-test-not-a-separator", [input_info], [output_info], [shape])
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        model.ir_version = 8
        model_path = self.root / "fixture.onnx"
        onnx.save(model, str(model_path))
        license_path = self.root / "fixture-license.txt"
        license_path.write_text("CC0-1.0 test fixture")
        self.contract.update(model=model_path.name, sha256=hashlib.sha256(model_path.read_bytes()).hexdigest(),
                             weights_license="CC0-1.0", weights_license_verified=True,
                             license_file=license_path.name,
                             license_sha256=hashlib.sha256(license_path.read_bytes()).hexdigest(),
                             source="Generated identity graph, not a separation model", version="test-1")
        manifest = self.root / "model.json"
        manifest.write_text(json.dumps(self.contract))
        session = LocalOnnxSeparator(manifest, "guitar", "cpu", 1024**2)
        report = isolate_wav(self.source, self.output, self.contract, "guitar", session, warmups=1)
        self.assertEqual(report["frames"], 1301)
        self.assertEqual(session.metadata["provider"], "CPUExecutionProvider")
        cli_report = self.root / "cli-report.json"
        cli_output = self.root / "cli-stem.wav"
        command = [sys.executable, str(Path(__file__).resolve().parents[1] / "python" / "separate.py"),
                   str(self.source), "--manifest", str(manifest), "--target", "guitar",
                   "--output", str(cli_output), "--report", str(cli_report)]
        run = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        measured = json.loads(cli_report.read_text())
        self.assertGreater(measured["peak_resident_bytes"], 0)
        self.assertEqual(measured["frames"], 1301)
        self.assertGreaterEqual(measured["initialization_seconds"], 0)
        if os.environ.get("TRANSCRIBER_SEPARATION_TEST_REPORT"):
            measured["identity_fixture_only"] = True
            Path(os.environ["TRANSCRIBER_SEPARATION_TEST_REPORT"]).write_text(
                json.dumps(measured, indent=2) + "\n", encoding="utf-8")
        for provider, runtime_name in (("cuda", "CUDAExecutionProvider"), ("directml", "DmlExecutionProvider"),
                                       ("rocm", "ROCMExecutionProvider")):
            if runtime_name not in onnxruntime.get_available_providers():
                with self.assertRaisesRegex(RuntimeError, "unavailable"):
                    LocalOnnxSeparator(manifest, "guitar", provider, 1024**2)
        # Keep a selected head even when it is silent; never replace it with the loudest source.
        self.contract["stems"] = ["guitar", "bass", "piano", "voice"]
        factors = helper.make_tensor("factor", TensorProto.FLOAT, [1], [-1.])
        zero = helper.make_tensor("zero", TensorProto.FLOAT, [1], [0.])
        nodes = [helper.make_node("Reshape", ["audio", "shape"], ["one"]),
                 helper.make_node("Mul", ["one", "factor"], ["negative"]),
                 helper.make_node("Mul", ["one", "zero"], ["silent"]),
                 helper.make_node("Concat", ["one", "negative", "silent", "one"], ["stems"], axis=1)]
        graph = helper.make_graph(nodes, "fixed-source-head-test", [input_info],
            [helper.make_tensor_value_info("stems", TensorProto.FLOAT, [1, 4, 1, 128])], [shape, factors, zero])
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        model.ir_version = 8
        onnx.save(model, str(model_path))
        self.contract["sha256"] = hashlib.sha256(model_path.read_bytes()).hexdigest()
        manifest.write_text(json.dumps(self.contract))
        bass = LocalOnnxSeparator(manifest, "bass", "cpu", 1024**2)
        bass_path = self.root / "bass.wav"
        isolate_wav(self.source, bass_path, self.contract, "bass", bass)
        with wave.open(str(bass_path), "rb") as audio:
            values = array("h")
            values.frombytes(audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                values.byteswap()
        self.assertEqual(values, array("h", [-value for value in self.values]))
        piano = LocalOnnxSeparator(manifest, "piano", "cpu", 1024**2)
        piano_path = self.root / "piano.wav"
        isolate_wav(self.source, piano_path, self.contract, "piano", piano)
        with wave.open(str(piano_path), "rb") as audio:
            self.assertEqual(audio.readframes(audio.getnframes()), b"\0" * len(self.values) * 2)
        model.graph.node[0].op_type = "UnsupportedSeparationOperator"
        onnx.save(model, str(model_path))
        self.contract["sha256"] = hashlib.sha256(model_path.read_bytes()).hexdigest()
        manifest.write_text(json.dumps(self.contract))
        failed_output, failed_report = self.root / "failed.wav", self.root / "failed-report.json"
        failed = subprocess.run([sys.executable,
            str(Path(__file__).resolve().parents[1] / "python" / "separate.py"),
            str(self.source), "--manifest", str(manifest), "--target", "guitar",
            "--output", str(failed_output), "--report", str(failed_report)],
            capture_output=True, text=True, timeout=60)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("Separation failed:", failed.stderr)
        self.assertNotIn("Traceback", failed.stderr)
        self.assertFalse(failed_output.exists())
        self.assertFalse(failed_report.exists())
        self.contract["sha256"] = "0" * 64
        manifest.write_text(json.dumps(self.contract))
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            LocalOnnxSeparator(manifest, "guitar", "cpu", 1024**2)


if __name__ == "__main__":
    unittest.main()
