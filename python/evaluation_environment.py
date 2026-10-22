"""Standard-library-only host metadata for reproducible evaluation reports."""

import csv
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def cpu_model() -> str | None:
    value = platform.processor().strip()
    if value:
        return value
    if sys.platform.startswith("linux"):
        try:
            for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
                if line.lower().startswith(("model name", "hardware")):
                    return line.split(":", 1)[-1].strip() or None
        except OSError:
            pass
    return os.environ.get("PROCESSOR_IDENTIFIER") or None


def _run_inventory(command: list[str], tool: str) -> dict:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=5, check=False)
    except subprocess.TimeoutExpired:
        return {"tool": tool, "status": "error", "reason": "inventory command timed out"}
    if result.returncode:
        return {"tool": tool, "status": "error",
                "reason": result.stderr.strip() or f"exit code {result.returncode}"}
    return {"tool": tool, "status": "reported", "output": result.stdout.strip()}


def gpu_inventory() -> dict:
    nvidia = shutil.which("nvidia-smi")
    if nvidia:
        try:
            result = subprocess.run(
                [nvidia, "--query-gpu=name,driver_version", "--format=csv,noheader"],
                capture_output=True, text=True, timeout=5, check=False,
            )
        except subprocess.TimeoutExpired:
            return {"status": "error", "tool": "nvidia-smi",
                    "reason": "inventory command timed out"}
        except OSError as error:
            return {"status": "error", "tool": "nvidia-smi", "reason": str(error)}
        if result.returncode == 0:
            devices = []
            for row in csv.reader(result.stdout.splitlines()):
                if row:
                    devices.append(dict(
                        name=row[0].strip(),
                        driver_version=row[1].strip() if len(row) > 1 else None,
                    ))
            return {"status": "detected" if devices else "none_reported",
                    "tool": "nvidia-smi", "devices": devices}
        return {"status": "error", "tool": "nvidia-smi",
                "reason": result.stderr.strip() or f"exit code {result.returncode}"}

    rocm = shutil.which("rocm-smi")
    if rocm:
        try:
            result = _run_inventory([rocm, "--showproductname"], "rocm-smi")
        except OSError as error:
            return {"status": "error", "tool": "rocm-smi", "reason": str(error)}
        if result["status"] == "reported":
            result["status"] = "detected" if result["output"] else "none_reported"
            result["devices"] = result.pop("output")
        return result

    return {
        "status": "not_detected",
        "tool": None,
        "devices": [],
        "reason": "nvidia-smi and rocm-smi were unavailable; this does not establish that no GPU exists",
    }


def host_identity() -> dict:
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_model": cpu_model(),
        "logical_cpus": os.cpu_count(),
        "gpu_inventory": gpu_inventory(),
    }
