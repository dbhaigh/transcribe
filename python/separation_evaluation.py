"""Paired corpus loading and artifact diagnostics for the existing evaluator."""

from array import array
import hashlib
import json
import math
from pathlib import Path
import sys
import wave

from evaluate_guitarset import sha256_file
from evaluation_metrics import validate_events


def wav_info(path):
    with wave.open(str(path), "rb") as audio:
        if audio.getcomptype() != "NONE" or audio.getsampwidth() != 2:
            raise ValueError("Paired evaluation requires PCM16 WAV inputs")
        if audio.getnchannels() not in (1, 2) or audio.getnframes() == 0:
            raise ValueError("Paired evaluation requires nonempty mono/stereo WAV")
        return audio.getframerate(), audio.getnframes()


def artifact_metrics(reference, estimate):
    """Streaming, zero-mean mono SI-SDR; no time shift or gain fitting of note times."""
    if wav_info(reference) != wav_info(estimate):
        raise ValueError("Artifact reference rate/frame count differs from estimate")
    count, sum_r, sum_e, rr, ee, re, clipped = 0, 0., 0., 0., 0., 0., 0
    with wave.open(str(reference), "rb") as ref, wave.open(str(estimate), "rb") as est:
        while True:
            rbytes, ebytes = ref.readframes(16384), est.readframes(16384)
            if not rbytes and not ebytes:
                break
            signals = []
            for raw, channels in ((rbytes, ref.getnchannels()), (ebytes, est.getnchannels())):
                values = array("h")
                values.frombytes(raw)
                if sys.byteorder != "little":
                    values.byteswap()
                signals.append([sum(values[i:i+channels]) / (32768 * channels)
                                for i in range(0, len(values), channels)])
            if len(signals[0]) != len(signals[1]):
                raise ValueError("Truncated paired WAV")
            for r, e in zip(*signals):
                count += 1
                sum_r += r
                sum_e += e
                rr += r*r
                ee += e*e
                re += r*e
                clipped += int(abs(e) >= 32767 / 32768)
    if count == 0:
        raise ValueError("Empty artifact comparison")
    energy_r, energy_e = max(0., rr - sum_r*sum_r/count), max(0., ee - sum_e*sum_e/count)
    covariance = re - sum_r*sum_e/count
    projected = covariance*covariance/energy_r if energy_r > 1e-15 else 0.
    error = max(0., energy_e - projected)
    score = 10 * math.log10(projected/error) if projected > 1e-15 and error > 1e-15 else None
    status = ("silent-reference" if energy_r <= 1e-15 else
              "silent-estimate" if energy_e <= 1e-15 else
              "exact-up-to-scale" if error <= 1e-15 else
              "orthogonal-estimate" if projected <= 1e-15 else "measured")
    return dict(si_sdr_db=score, status=status, clipped_frame_fraction=clipped/count,
                reference_rms=math.sqrt(rr/count), estimate_rms=math.sqrt(ee/count),
                warning="SI-SDR is a waveform artifact diagnostic, not instrument identity or note accuracy; "
                        "no content alignment is performed")


def paired_corpus(manifest, selected_split="all"):
    manifest = Path(manifest).resolve()
    document = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or not isinstance(document.get("pairs"), list):
        raise ValueError("Paired manifest must be an object containing a pairs list")
    for key in ("name", "version", "provenance", "license", "training_data_warning", "pairs"):
        if not document.get(key):
            raise ValueError(f"Paired manifest requires explicit {key}")
    if document.get("schema_version") != 1:
        raise ValueError("Unsupported paired manifest version")
    clips, ids, audio_splits, performer_splits = [], set(), {}, {}
    for pair in document["pairs"]:
        if not isinstance(pair, dict):
            raise ValueError("Each paired clip must be an object")
        identity = pair["id"]
        if not isinstance(identity, str) or not identity or identity in ids:
            raise ValueError("Paired IDs must be unique nonempty strings")
        ids.add(identity)
        if pair["target"] not in ("guitar", "bass", "piano", "voice"):
            raise ValueError("Unknown paired target instrument")
        split, performer = pair["dataset_split"], pair["performer_id"]
        if split not in ("train", "validation", "test") or not isinstance(performer, str) or not performer:
            raise ValueError("Each pair needs a split and performer/song group for leakage checks")
        if not pair.get("license") or not pair.get("source") or not pair.get("separation"):
            raise ValueError("Each pair requires license, source provenance and separation metadata")
        separation = pair["separation"]
        if not isinstance(separation, dict):
            raise ValueError("Separation metadata must be an object")
        if type(separation.get("learned_model")) is not bool or not separation.get("method"):
            raise ValueError("Separation metadata must declare method and learned_model explicitly")
        if separation["learned_model"]:
            for field in ("model_sha256", "weights_license", "model_source", "model_version", "provider"):
                if not isinstance(separation.get(field), str) or not separation[field]:
                    raise ValueError(f"Learned separation metadata requires {field}")
            digest = separation["model_sha256"]
            if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
                raise ValueError("Learned separator SHA-256 is invalid")
        if performer in performer_splits and performer_splits[performer] != split:
            raise ValueError("Performer/song group crosses dataset splits")
        performer_splits[performer] = split
        for field in ("original", "isolated"):
            if not isinstance(pair.get(field), str) or not pair[field]:
                raise ValueError(f"Paired clip requires a nonempty {field} WAV path")
        paths = {variant: (manifest.parent / pair[variant]).resolve() for variant in ("original", "isolated")}
        rates = {variant: wav_info(path) for variant, path in paths.items()}
        if rates["original"] != rates["isolated"]:
            raise ValueError("Original and isolated audio must have identical rate/frame count")
        if paths["original"] == paths["isolated"]:
            raise ValueError("Paired original and stem must be distinct files")
        rate, frames = rates["original"]
        if not isinstance(pair.get("notes"), list) or any(not isinstance(note, dict) for note in pair["notes"]):
            raise ValueError("Paired annotations must be a list of note objects")
        validate_events(pair["notes"])
        duration = frames/rate
        if any(note["start"] + note["duration"] > duration + 1/rate for note in pair["notes"]):
            raise ValueError("Target annotation exceeds original recording duration")
        artifact = {}
        if pair.get("reference_audio"):
            if not isinstance(pair["reference_audio"], str):
                raise ValueError("Reference audio must be a WAV path string")
            reference = (manifest.parent / pair["reference_audio"]).resolve()
            digest = sha256_file(reference)
            if digest in audio_splits and audio_splits[digest] != split:
                raise ValueError("Identical target reference crosses dataset splits")
            audio_splits[digest] = split
            artifact = {variant: artifact_metrics(reference, path) for variant, path in paths.items()}
        for variant, path in paths.items():
            digest = sha256_file(path)
            if digest in audio_splits and audio_splits[digest] != split:
                raise ValueError("Identical audio crosses dataset splits")
            audio_splits[digest] = split
            if selected_split not in ("all", split):
                continue
            clips.append(dict(id=f"{identity}/{variant}", pair_id=identity, variant=variant,
                              instrument=pair["target"], audio=str(path), sha256=digest,
                              sample_rate=rate, duration_seconds=duration, notes=pair["notes"],
                              dataset_split=split, performer_id=performer, separation=pair["separation"],
                              artifacts=artifact.get(variant), source=pair["source"], license=pair["license"],
                              scenarios=["mixed_recording", pair["target"], variant]))
    if not clips:
        raise ValueError("No paired clips in selected split")
    metadata = {key: document[key] for key in ("name", "version", "provenance", "license",
                                               "training_data_warning")}
    metadata.update(manifest_sha256=sha256_file(manifest), selected_split=selected_split, clips=clips)
    identity = dict(metadata, clips=[{key: value for key, value in clip.items() if key != "audio"}
                                     for clip in clips])
    metadata["sha256"] = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    return metadata


def paired_comparisons(rows):
    pairs = {}
    for row in rows:
        if "pair_id" in row:
            pairs.setdefault(row["pair_id"], {})[row["variant"]] = row
    result = []
    for identity, variants in pairs.items():
        original, isolated = variants["original"], variants["isolated"]
        deltas = {metric + "_f1": isolated["metrics"][metric]["f1"] - original["metrics"][metric]["f1"]
                  for metric in ("note_event", "onset_pitch", "pitch_event", "frame")}
        result.append(dict(id=identity, instrument=isolated["instrument"], deltas=deltas,
                           degraded_metrics=[key for key, delta in deltas.items() if delta < -1e-12],
                           original_metrics=original["metrics"], isolated_metrics=isolated["metrics"],
                           separation=isolated["separation"],
                           original_artifacts=original.get("artifacts"),
                           isolated_artifacts=isolated.get("artifacts")))
    return result
