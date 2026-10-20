"""Versioned, deterministic PCM16 evaluation corpus; no external packages/data."""

from __future__ import annotations

import hashlib
import json
import math
import struct
import wave
from pathlib import Path


CORPUS_VERSION = 1
SAMPLE_RATE = 22050
SEED = 2026


def note(midi: int, onset: float, offset: float, velocity: int = 90, **kwargs) -> dict:
    return dict(midi=midi, start=onset, duration=offset - onset, onset=onset,
                offset=offset, velocity=velocity, instrument="synthetic_harmonic", **kwargs)


def specifications() -> list[dict]:
    cases = [
        ("monophonic", 2.0, [note(57, .15, .7), note(64, .9, 1.6)], {}),
        ("repeated", 2.0, [note(69, .15, .55), note(69, .7, 1.1), note(69, 1.25, 1.65)], {}),
        ("silence", 1.0, [], {}),
        ("octaves", 2.5, [note(45, .15, .7), note(57, .9, 1.45), note(69, 1.65, 2.2)], {}),
        ("vibrato", 2.0, [note(69, .15, 1.65, vibrato_cents=35.0)], {}),
        ("chord", 1.8, [note(p, .15, 1.4, 65) for p in (60, 64, 67)], {}),
        ("overlap", 2.2, [note(57, .15, 1.2, 70), note(64, .65, 1.7, 70)], {}),
        ("decay", 2.2, [note(64, .15, 1.8, decay=3.0)], {}),
        ("attack", 2.2, [note(57, .15, 1.8, attack=.25)], {}),
        ("noise", 2.0, [note(69, .15, 1.65)], {"noise_amplitude": .04}),
        ("noise_only", 1.0, [], {"noise_amplitude": .04}),
        ("reverberation", 2.5, [note(57, .15, .65), note(64, .9, 1.45)],
         {"echoes": [[.073, .32], [.149, .19], [.277, .10]]}),
        ("clipping", 1.5, [note(69, .15, 1.2)], {"gain": 8.0}),
        ("very_short", .01, [note(69, 0, .01, attack=.001)], {}),
        ("empty", 0.0, [], {}),
        # Straddles the CLI 15 s boundary, not just a short artificial chunk.
        ("long_boundary", 16.5, [note(57, .2, .8), note(69, 13.8, 15.5),
                                  note(64, 15.7, 16.3)], {}),
    ]
    return [dict(id=name, duration_seconds=duration, notes=notes, effects=effects)
            for name, duration, notes, effects in cases]


def render(spec: dict, sample_rate: int = SAMPLE_RATE) -> bytes:
    samples = [0.0] * round(spec["duration_seconds"] * sample_rate)
    for event in spec["notes"]:
        first = round(event["start"] * sample_rate)
        end = round(event["offset"] * sample_rate)
        frequency = 440 * 2 ** ((event["midi"] - 69) / 12)
        phase = 0.0
        for index in range(first, min(end, len(samples))):
            t = (index - first) / sample_rate
            remaining = (end - index) / sample_rate
            vibrato = event.get("vibrato_cents", 0) * math.sin(2 * math.pi * 5 * t)
            envelope = max(0.0, min(1.0, t / event.get("attack", .015), remaining / .025))
            envelope *= math.exp(-event.get("decay", 0) * t)
            # Velocity is a synthesis gain parameter, not a calibrated acoustic loudness.
            signal = math.sin(phase) + .25 * math.sin(2 * phase) + .125 * math.sin(3 * phase)
            samples[index] += .4 * event["velocity"] / 127 * envelope * signal
            phase += 2 * math.pi * frequency * 2 ** (vibrato / 1200) / sample_rate
    # Explicit integer LCG: independent of Python's random implementation.
    state = SEED
    noise = spec["effects"].get("noise_amplitude", 0)
    for index in range(len(samples)):
        state = (1664525 * state + 1013904223) & 0xffffffff
        samples[index] += noise * (2 * state / 0xffffffff - 1)
    dry = samples[:]
    for delay, gain in spec["effects"].get("echoes", []):
        shift = round(delay * sample_rate)
        for index in range(shift, len(samples)):
            samples[index] += gain * dry[index - shift]
    gain = spec["effects"].get("gain", 1)
    return b"".join(struct.pack("<h", round(max(-1, min(1, value * gain)) * 32767))
                    for value in samples)


def generate(directory: Path) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    clips = []
    for spec in specifications():
        path = directory / (spec["id"] + ".wav")
        with wave.open(str(path), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(SAMPLE_RATE)
            audio.writeframes(render(spec))
        clips.append(dict(spec, audio=path.name, sample_rate=SAMPLE_RATE,
                          sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    manifest = {
        "schema_version": 1, "name": "transcriber-synthetic", "version": CORPUS_VERSION,
        "seed": SEED, "sample_rate": SAMPLE_RATE,
        "provenance": "Generated locally by synthetic_corpus.py; no sampled recordings",
        "ground_truth": "Dry oscillator activation; reverb tails are not extra notes",
        "clips": clips,
    }
    identity = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest["sha256"] = hashlib.sha256(identity).hexdigest()
    (directory / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    result = generate(arguments.output)
    print(f"Generated {len(result['clips'])} clips; corpus SHA-256 {result['sha256']}")
