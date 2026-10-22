"""Versioned, deterministic PCM16 evaluation corpus; no external packages/data."""

from __future__ import annotations

import hashlib
import json
import math
import struct
import wave
from pathlib import Path


CORPUS_VERSION = 1
LATEST_CORPUS_VERSION = 3
SAMPLE_RATE = 22050
SEED = 2026


def generate_mixtures(directory: Path) -> Path:
    """Oracle and deliberately damaged stems, not a learned separation benchmark."""
    pairs = []
    for target, pitches in (("guitar", [57, 64]), ("bass", [40, 45]),
                            ("piano", [60, 67]), ("voice", [69, 72])):
        notes = [note(pitches[0], .2, 1.2), note(pitches[1], 1.7, 2.7)]
        interference = [note(76, .05, 1.45, 110), note(52, 1.5, 2.95, 110)]
        for damaged in (False, True):
            identity = f"{target}-{'damaged' if damaged else 'oracle'}"
            original, isolated, reference = [directory / f"{identity}-{kind}.wav"
                                               for kind in ("mixture", "isolated", "reference")]
            background = [dict(event, velocity=5) for event in interference] if damaged else interference
            for path, events in ((original, notes + background), (reference, notes),
                                 (isolated, [notes[0], note(80, 1.7, 2.7)] if damaged else notes)):
                pcm = render(dict(duration_seconds=3, notes=events, effects={}))
                with wave.open(str(path), "wb") as output:
                    output.setnchannels(1)
                    output.setsampwidth(2)
                    output.setframerate(SAMPLE_RATE)
                    output.writeframes(pcm)
            pairs.append(dict(id=identity, target=target, original=original.name,
                              isolated=isolated.name, reference_audio=reference.name, notes=notes,
                              dataset_split="test", performer_id=f"synthetic-{target}",
                              source="Deterministic harmonic synthesis; changing interference after 1.5 s",
                              license="CC0-1.0 generated fixtures", separation=dict(
                                  method="deliberate missing note and hallucination" if damaged else "oracle target",
                                  learned_model=False)))
    manifest = dict(schema_version=1, name="synthetic-mixed-v1", version=1,
                    provenance="Transcriber synthetic_corpus.generate_mixtures; not real instrument recordings",
                    license="CC0-1.0 generated fixtures",
                    training_data_warning="No learned separator. Oracle results are an upper-bound integration check, "
                                          "not evidence of model separation benefit.",
                    pairs=pairs)
    path = directory / "mixed-manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return path


def note(midi: int, onset: float, offset: float, velocity: int = 90, **kwargs) -> dict:
    return dict(midi=midi, start=onset, duration=offset - onset, onset=onset,
                offset=offset, velocity=velocity, instrument="synthetic_harmonic", **kwargs)


def specifications(version: int = CORPUS_VERSION) -> list[dict]:
    if version not in (1, 2, LATEST_CORPUS_VERSION):
        raise ValueError(f"Unsupported synthetic corpus version: {version}")
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
    specs = [dict(id=name, duration_seconds=duration, notes=notes, effects=effects)
             for name, duration, notes, effects in cases]
    if version == 1:
        return specs
    specs.extend([
        dict(id="sustained", duration_seconds=3.0,
             notes=[note(55, .2, 2.8)], effects={}),
        dict(id="pitch_bend", duration_seconds=2.5,
             notes=[note(69, .2, 2.2, bend_cents=120.0)], effects={}),
        dict(id="harmonics_only", duration_seconds=2.0,
             notes=[note(69, .2, 1.7, harmonics=[0.0, 1.0, .35])], effects={}),
        dict(id="register_low", duration_seconds=2.0,
             notes=[note(36, .2, 1.7)], effects={}),
        dict(id="register_mid", duration_seconds=2.0,
             notes=[note(60, .2, 1.7)], effects={}),
        dict(id="register_high", duration_seconds=2.0,
             notes=[note(84, .2, 1.7)], effects={}),
    ])
    if version >= 3:
        specs.extend([
            dict(id="dyad_fifth", duration_seconds=2.0,
                 notes=[note(p, .2, 1.7) for p in (52, 59)], effects={}),
            dict(id="dense_chord", duration_seconds=2.0,
                 notes=[note(p, .2, 1.7, 45) for p in (40, 47, 52, 56, 59, 64)], effects={}),
            dict(id="octave_interval", duration_seconds=2.0,
                 notes=[note(p, .2, 1.7, 65) for p in (52, 64)], effects={}),
            dict(id="bass_melody", duration_seconds=2.5,
                 notes=[note(40, .2, 2.2), note(69, .4, 1.0, 60),
                        note(71, 1.2, 1.9, 60)], effects={}),
            dict(id="distorted_guitar_fixture", duration_seconds=2.0,
                 notes=[note(p, .2, 1.7, 50) for p in (40, 47, 52)],
                 effects={"gain": 6.0}),
            dict(id="release_tails", duration_seconds=2.5,
                 notes=[note(52, .2, 1.5, decay=2), note(59, .8, 2.1, decay=2)],
                 effects={}),
            dict(id="adjacent_semitones", duration_seconds=2.0,
                 notes=[note(p, .2, 1.7, 60) for p in (69, 70)], effects={}),
            dict(id="strong_harmonics", duration_seconds=2.0,
                 notes=[note(52, .2, 1.7, harmonics=[1.0, .8, .6, .4])], effects={}),
        ])
    scenario_labels = {
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
        "dyad_fifth": ["polyphonic", "dyads"],
        "dense_chord": ["polyphonic", "dense_chords"],
        "octave_interval": ["polyphonic", "octave_intervals"],
        "bass_melody": ["polyphonic", "bass_plus_melody"],
        "distorted_guitar_fixture": ["polyphonic", "distorted_synthetic"],
        "release_tails": ["polyphonic", "overlapping_release_tails"],
        "adjacent_semitones": ["polyphonic", "adjacent_semitones"],
        "strong_harmonics": ["monophonic", "harmonic_false_positives"],
    }
    for spec in specs:
        spec["scenarios"] = scenario_labels.get(spec["id"], ["monophonic"])
        spec["dataset_split"] = "test"
    return specs


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
            bend = event.get("bend_cents", 0) * t / event["duration"]
            envelope = max(0.0, min(1.0, t / event.get("attack", .015), remaining / .025))
            envelope *= math.exp(-event.get("decay", 0) * t)
            # Velocity is a synthesis gain parameter, not a calibrated acoustic loudness.
            harmonics = event.get("harmonics", (1.0, .25, .125))
            signal = sum(weight * math.sin((index + 1) * phase)
                         for index, weight in enumerate(harmonics))
            samples[index] += .4 * event["velocity"] / 127 * envelope * signal
            phase += 2 * math.pi * frequency * 2 ** ((vibrato + bend) / 1200) / sample_rate
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


def generate(directory: Path, version: int = CORPUS_VERSION) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    clips = []
    for spec in specifications(version):
        path = directory / (spec["id"] + ".wav")
        with wave.open(str(path), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(SAMPLE_RATE)
            audio.writeframes(render(spec))
        clips.append(dict(spec, audio=path.name, sample_rate=SAMPLE_RATE,
                          sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    manifest = {
        "schema_version": version, "name": "transcriber-synthetic", "version": version,
        "seed": SEED, "sample_rate": SAMPLE_RATE,
        "provenance": "Generated locally by synthetic_corpus.py; no sampled recordings",
        "ground_truth": "Dry oscillator activation; reverb tails are not extra notes",
        "clips": clips,
    }
    if version >= 2:
        manifest.update(
            license="MIT project license; generated audio contains no third-party recordings",
            dataset_split_policy={
                "evaluation_role": "test-only; do not use these fixtures to train models",
                "training": [],
                "validation": [],
                "test": [clip["id"] for clip in clips],
            },
            training_data_warning=(
                "Evaluation-only fixture labels do not establish independence from model "
                "training data or replace held-out licensed recordings."
            ),
        )
    identity = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest["sha256"] = hashlib.sha256(identity).hexdigest()
    (directory / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", type=int, choices=(1, LATEST_CORPUS_VERSION),
                        default=CORPUS_VERSION)
    arguments = parser.parse_args()
    result = generate(arguments.output, arguments.version)
    print(f"Generated {len(result['clips'])} clips; corpus SHA-256 {result['sha256']}")
