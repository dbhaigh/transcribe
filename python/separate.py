#!/usr/bin/env python3
"""Explicit local waveform-ONNX isolation; no network access or bundled model weights."""

from __future__ import annotations

import argparse
from array import array
import json
import math
import os
from pathlib import Path
import platform
import sys
import tempfile
import time
import wave

from evaluation_metrics import distribution
from evaluate_guitarset import sha256_file


class SeparationCancelled(RuntimeError):
    pass


def check_cancelled(cancel):
    if cancel():
        raise SeparationCancelled("Separation cancelled; no partial stem was published")


def validate_contract(contract):
    if not isinstance(contract, dict):
        raise ValueError("Separation manifest must be a JSON object")
    if contract.get("schema_version") != 1 or contract.get("contract") != "waveform-stems-v1":
        raise ValueError("Expected schema 1 waveform-stems-v1; MDX/STFT and Demucs checkpoints need specific adapters")
    for key, low, high in (("sample_rate", 8000, 192000), ("channels", 1, 2),
                           ("window_frames", 64, 2_000_000), ("context_frames", 0, 999999)):
        value = contract.get(key)
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f"Invalid {key}: expected integer {low}..{high}")
    if 2 * contract["context_frames"] >= contract["window_frames"]:
        raise ValueError("Window must exceed twice its trimmed context")
    stems = contract.get("stems")
    if (not isinstance(stems, list) or not 1 <= len(stems) <= 16 or
            any(not isinstance(stem, str) or not stem for stem in stems) or len(set(stems)) != len(stems)):
        raise ValueError("Model must declare 1-16 unique source names")
    if contract.get("latency_samples") != 0:
        raise ValueError("Only explicitly zero-latency waveform exports are supported; correct model delay in its adapter")


def isolate_wav(source, destination, contract, target, infer, *, cancel=lambda: False,
                warmups=0, max_buffer_bytes=128 * 1024 * 1024):
    """infer(planar float arrays) returns target planar arrays; one persistent session."""
    validate_contract(contract)
    if type(warmups) is not int or not 0 <= warmups <= 100 or max_buffer_bytes <= 0:
        raise ValueError("Warmups must be an integer in 0-100 and buffer budget must be positive")
    if target not in contract["stems"]:
        raise ValueError(f"Model has no {target} source; available: {contract['stems']}")
    window, context = contract["window_frames"], contract["context_frames"]
    channels = contract["channels"]
    # Input, all model outputs, selected result and PCM conversion; excludes runtime model/workspace.
    buffer_estimate = window * channels * 4 * (len(contract["stems"]) + 4)
    if buffer_estimate > max_buffer_bytes:
        raise ValueError(f"Window tensor estimate {buffer_estimate} exceeds buffer budget {max_buffer_bytes}")
    source, destination = Path(source), Path(destination)
    if destination.exists():
        raise FileExistsError(f"Refusing to replace an existing stem: {destination}")
    check_cancelled(cancel)
    initial = (source.stat().st_size, source.stat().st_mtime_ns)
    descriptor, name = tempfile.mkstemp(prefix=".isolation-", suffix=".wav", dir=destination.parent)
    temporary = Path(name)
    times, clipped = [], 0
    started = time.perf_counter()
    try:
        with os.fdopen(descriptor, "wb") as raw, wave.open(str(source), "rb") as audio:
            if (audio.getcomptype() != "NONE" or audio.getsampwidth() != 2 or
                    audio.getnchannels() != channels or audio.getframerate() != contract["sample_rate"]):
                raise ValueError("Local separation requires PCM16 WAV at the model's exact channel count/rate; no silent resampling")
            frames = audio.getnframes()
            if frames == 0:
                raise ValueError("Cannot separate an empty recording")
            with wave.open(raw, "wb") as output:
                output.setnchannels(channels)
                output.setsampwidth(2)
                output.setframerate(audio.getframerate())
                step = window - 2 * context
                for first in range(0, frames, step):
                    check_cancelled(cancel)
                    start, end = max(0, first - context), min(frames, first - context + window)
                    audio.setpos(start)
                    pcm = array("h")
                    pcm.frombytes(audio.readframes(end - start))
                    if len(pcm) != (end - start) * channels:
                        raise ValueError("Truncated source WAV while separating")
                    if sys.byteorder != "little":
                        pcm.byteswap()
                    planar = [array("f", [0]) * window for _ in range(channels)]
                    padding = max(0, context - first)
                    for i in range(end - start):
                        for channel in range(channels):
                            planar[channel][padding + i] = pcm[i * channels + channel] / 32768
                    if first == 0:
                        for _ in range(warmups):
                            check_cancelled(cancel)
                            infer(planar)
                    check_cancelled(cancel)
                    before = time.perf_counter()
                    separated = infer(planar)
                    times.append(time.perf_counter() - before)
                    check_cancelled(cancel)
                    if len(separated) != channels or any(len(row) != window for row in separated):
                        raise ValueError("Separator returned incompatible waveform dimensions")
                    if any(not math.isfinite(value) for row in separated for value in row):
                        raise ValueError("Separator produced nonfinite samples")
                    count = min(step, frames - first)
                    encoded = array("h")
                    for i in range(context, context + count):
                        for channel in range(channels):
                            value = separated[channel][i]
                            clipped += int(value < -1 or value >= 1)
                            encoded.append(max(-32768, min(32767, round(value * 32768))))
                    if sys.byteorder != "little":
                        encoded.byteswap()
                    output.writeframesraw(encoded.tobytes())
                    del separated
        check_cancelled(cancel)
        if initial != (source.stat().st_size, source.stat().st_mtime_ns):
            raise RuntimeError("Original source changed during separation; result discarded")
        with wave.open(str(temporary), "rb") as check:
            if check.getnframes() != frames:
                raise RuntimeError("Separator output lost alignment; result discarded")
        os.link(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    elapsed = time.perf_counter() - started
    return dict(duration_seconds=frames / contract["sample_rate"], frames=frames,
                sample_rate=contract["sample_rate"], channels=channels, origin_sample=0,
                processing_seconds=elapsed, real_time_factor=elapsed / (frames / contract["sample_rate"]),
                window_seconds=distribution(times), window_count=len(times), warmups=warmups,
                tensor_buffer_estimate_bytes=buffer_estimate, clipped_samples=clipped,
                alignment="Declared zero-latency contract; exact rate/count enforced, no content-based delay correction")


class LocalOnnxSeparator:
    def __init__(self, manifest, target, provider, max_model_bytes):
        startup_started = time.perf_counter()
        contract = json.loads(Path(manifest).read_text(encoding="utf-8"))
        validate_contract(contract)
        root = Path(manifest).resolve().parent
        for field in ("model", "license_file", "sha256", "license_sha256", "weights_license", "source", "version"):
            if not isinstance(contract.get(field), str) or not contract[field]:
                raise ValueError(f"Local model manifest requires nonempty {field}")
        model = root / contract["model"]
        license_file = root / contract["license_file"]
        if (contract.get("weights_license_verified") is not True or not contract.get("weights_license") or
                not contract.get("source") or not contract.get("version")):
            raise ValueError("Record verified weight license, provenance and version before running a local model")
        if model.stat().st_size > max_model_bytes:
            raise ValueError("Local model exceeds --max-model-mib")
        if sha256_file(model) != contract["sha256"] or sha256_file(license_file) != contract["license_sha256"]:
            raise ValueError("Model/license checksum mismatch; no inference attempted")
        if target not in contract["stems"]:
            raise ValueError(f"No {target} head in this model")
        # Optional dependencies are imported only after an explicit local-model request.
        import onnx
        import numpy as np
        import onnxruntime as ort
        from google.protobuf.message import DecodeError
        from onnxruntime.capi import onnxruntime_pybind11_state as ort_errors
        self.runtime_errors = (ort_errors.Fail, ort_errors.InvalidArgument, ort_errors.NoSuchFile,
            ort_errors.NoModel, ort_errors.EngineError, ort_errors.RuntimeException,
            ort_errors.InvalidProtobuf, ort_errors.NotImplemented, ort_errors.InvalidGraph, ort_errors.EPFail)
        def check_tensor(tensor):
            if tensor.external_data or tensor.data_location == onnx.TensorProto.EXTERNAL:
                raise ValueError("External tensor files are not supported by the checksummed single-file model contract")
        def check_graph(graph):
            for tensor in graph.initializer:
                check_tensor(tensor)
            for sparse in graph.sparse_initializer:
                check_tensor(sparse.values)
                check_tensor(sparse.indices)
            for node in graph.node:
                for attribute in node.attribute:
                    if attribute.HasField("t"):
                        check_tensor(attribute.t)
                    for tensor in attribute.tensors:
                        check_tensor(tensor)
                    if attribute.HasField("g"):
                        check_graph(attribute.g)
                    for nested in attribute.graphs:
                        check_graph(nested)
        try:
            inspected = onnx.load_model(str(model), load_external_data=False)
        except DecodeError as error:
            raise ValueError(f"Cannot parse the local separation ONNX graph: {error}") from error
        check_graph(inspected.graph)
        if inspected.functions:
            raise ValueError("Local ONNX functions require a reviewed adapter; use a self-contained graph")
        del inspected
        providers = {"cpu": "CPUExecutionProvider", "cuda": "CUDAExecutionProvider",
                     "directml": "DmlExecutionProvider", "rocm": "ROCMExecutionProvider"}
        selected = providers[provider]
        if selected not in ort.get_available_providers():
            raise RuntimeError(f"{selected} unavailable; installed: {ort.get_available_providers()}; "
                               "select an installed provider or disable preprocessing")
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        options.enable_mem_pattern = provider != "directml"
        if provider != "cpu":
            options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
        started = time.perf_counter()
        try:
            self.session = ort.InferenceSession(str(model), sess_options=options, providers=[selected])
        except self.runtime_errors as error:
            raise RuntimeError(f"Cannot initialize separation model on {selected}: {error}") from error
        self.initialization_seconds = time.perf_counter() - started
        inputs, outputs = self.session.get_inputs(), self.session.get_outputs()
        expected_input = [1, contract["channels"], contract["window_frames"]]
        expected_output = [1, len(contract["stems"]), contract["channels"], contract["window_frames"]]
        if (len(inputs) != 1 or len(outputs) != 1 or inputs[0].shape != expected_input or
                outputs[0].shape != expected_output or inputs[0].type != "tensor(float)" or
                outputs[0].type != "tensor(float)"):
            raise ValueError(f"Expected float waveform tensors {expected_input} -> {expected_output}; "
                             "spectrogram/mask/checkpoint models require their own adapter")
        self.input_name, self.output_name = inputs[0].name, outputs[0].name
        self.target_index, self.np = contract["stems"].index(target), np
        self.contract = contract
        self.metadata = dict(model_bytes=model.stat().st_size, model_sha256=contract["sha256"],
                             license_sha256=contract["license_sha256"],
                             weights_license=contract["weights_license"], source=contract["source"],
                             version=contract["version"], provider=selected, runtime=ort.__version__,
                             numpy_version=np.__version__, onnx_version=onnx.__version__,
                             initialization_seconds=self.initialization_seconds)
        self.metadata["startup_seconds"] = time.perf_counter() - startup_started

    def __call__(self, planar):
        values = self.np.asarray(planar, dtype=self.np.float32)[None, ...]
        try:
            output = self.session.run([self.output_name], {self.input_name: values})[0]
        except self.runtime_errors as error:
            raise RuntimeError(f"Separation inference failed: {error}") from error
        expected = (1, len(self.contract["stems"]), self.contract["channels"], self.contract["window_frames"])
        if output.shape != expected:
            raise ValueError("Runtime output dimensions disagree with separation metadata")
        return output[0, self.target_index]


def peak_memory():
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ("PeakWorkingSetSize", "WorkingSetSize",
                "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
        value = Counters()
        value.cb = ctypes.sizeof(value)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(value), value.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return value.PeakWorkingSetSize
    if sys.platform.startswith(("linux", "darwin")):
        import resource
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    return None


def publish_report(path, result):
    descriptor, name = tempfile.mkstemp(prefix=".isolation-report-", suffix=".json", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(result, output, indent=2, allow_nan=False)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.link(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--target", choices=("guitar", "bass", "piano", "voice"), required=True)
    parser.add_argument("--provider", choices=("cpu", "cuda", "directml", "rocm"), default="cpu")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--max-model-mib", type=int, default=1024)
    parser.add_argument("--max-buffer-mib", type=int, default=128)
    parser.add_argument("--cancel-file", type=Path)
    args = parser.parse_args()
    started = time.perf_counter()
    try:
        if args.warmups < 0 or args.warmups > 100 or args.max_model_mib <= 0 or args.max_buffer_mib <= 0:
            raise ValueError("Warmups must be 0-100 and resource limits positive")
        protected = {args.input.resolve(), args.manifest.resolve(), args.output.resolve()}
        if args.report.resolve() in protected or args.report.exists():
            raise ValueError("Report must be a new file distinct from input/manifest/stem")
        check_cancelled(lambda: args.cancel_file is not None and args.cancel_file.exists())
        separator = LocalOnnxSeparator(args.manifest, args.target, args.provider, args.max_model_mib * 1024**2)
        result = isolate_wav(args.input, args.output, separator.contract, args.target, separator,
                             cancel=lambda: args.cancel_file is not None and args.cancel_file.exists(),
                             warmups=args.warmups, max_buffer_bytes=args.max_buffer_mib * 1024**2)
        result.update(separator.metadata)
        result.update(target=args.target, input_sha256=sha256_file(args.input),
                      isolated_sha256=sha256_file(args.output), peak_resident_bytes=peak_memory(),
                      environment=dict(python=platform.python_version(), platform=platform.platform()),
                      warning="No claim of transcription benefit. Audition source changes and compare paired note metrics.")
        result["total_wall_seconds"] = time.perf_counter() - started
        result["end_to_end_real_time_factor"] = result["total_wall_seconds"] / result["duration_seconds"]
        publish_report(args.report, result)
        print(f"Isolated audio written to {args.output}; audition before transcribing. Report: {args.report}")
        return 0
    except KeyboardInterrupt:
        print("Separation cancelled; partial output was discarded.", file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError, KeyError, ImportError, wave.Error) as error:
        print(f"Separation failed: {error}. No automatic fallback or download.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
