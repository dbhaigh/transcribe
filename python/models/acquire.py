#!/usr/bin/env python3
"""Explicit, pinned Basic Pitch acquisition/verification; never called by the application."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import urllib.request


def acquire(destination: Path, *, download: bool = False) -> None:
    catalog = Path(__file__).resolve().parent
    metadata = json.loads((catalog / "basic_pitch.provenance.json").read_text(encoding="utf-8"))
    license_path = catalog / metadata["license_file"]
    if metadata["license"] != "Apache-2.0" or not license_path.is_file():
        raise ValueError("Reviewed Apache-2.0 license file is required")
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).hexdigest() != metadata["sha256"]:
            raise ValueError(f"Checksum mismatch: {destination}; existing file was not overwritten")
        return
    if not download:
        raise FileNotFoundError(f"Model missing: {destination}; use --download explicitly to acquire pinned weights")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=".basic-pitch-", suffix=".tmp", dir=destination.parent)
    temporary = Path(name)
    try:
        digest = hashlib.sha256()
        count = 0
        with os.fdopen(descriptor, "wb") as target:
            with urllib.request.urlopen(metadata["download_url"], timeout=60) as source:
                if not source.geturl().startswith("https://"):
                    raise ValueError("Model download redirected to a non-HTTPS URL")
                while block := source.read(64 * 1024):
                    count += len(block)
                    if count > metadata["size_bytes"]:
                        raise ValueError("Model download exceeds pinned size limit")
                    digest.update(block)
                    target.write(block)
            target.flush()
            os.fsync(target.fileno())
        if count != metadata["size_bytes"] or digest.hexdigest() != metadata["sha256"]:
            raise ValueError("Downloaded model size/checksum does not match pinned provenance")
        # Hard-link publication is exclusive: never overwrite a file created concurrently.
        os.link(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "basic_pitch.onnx")
    parser.add_argument("--download", action="store_true", help="Explicit opt-in network acquisition")
    args = parser.parse_args()
    try:
        acquire(args.output, download=args.download)
        print(f"Verified pinned Basic Pitch: {args.output}")
    except (OSError, ValueError) as error:
        print(f"Model acquisition failed: {error}", file=sys.stderr)
        sys.exit(1)
