import csv
import json
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def manifest_text(headers, rows):
    import io

    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(["transcriber-batch-manifest", "1"])
    writer.writerow(headers)
    writer.writerows(rows)
    return output.getvalue()


def make_wave(path):
    import math
    import struct

    path.parent.mkdir(parents=True, exist_ok=True)
    rate = 22050
    frames = bytearray()
    for index in range(rate):
        sample = int(12000 * math.sin(2.0 * math.pi * 440.0 * index / rate))
        frames.extend(struct.pack("<h", sample))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(frames)


def run(executable, *args):
    return subprocess.run(
        [str(executable), *map(str, args)],
        capture_output=True,
        text=True,
        timeout=120,
    )


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="transcriber-batch-") as temporary:
        root = Path(temporary)
        first = root / "音频, take.wav"
        second = root / "duplicate" / "音频, take.wav"
        broken = root / "broken.wav"
        bad_compressed = root / "broken.flac"
        make_wave(first)
        make_wave(second)
        broken.write_bytes(b"not a wave file")
        bad_compressed.write_bytes(b"not a FLAC file")
        flac = root / "alternate-format.flac"
        flac_supported = False
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg:
            encoded = subprocess.run(
                [ffmpeg, "-v", "error", "-y", "-i", str(first), str(flac)],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if encoded.returncode == 0:
                probe = run(
                    executable,
                    flac,
                    "--musicxml", root / "flac-probe.musicxml",
                    "--tab", root / "flac-probe.tab",
                    "--quiet",
                )
                flac_supported = probe.returncode == 0

        results = root / "results.json"
        progress = root / "progress.jsonl"
        output_dir = root / "generated output"
        manifest = root / "jobs.csv"
        headers = [
            "input", "musicxml", "tab", "midi", "json", "score-project",
            "events-json", "project",
            "instrument", "tempo", "quality",
        ]
        rows = [
            [
                first.name, "custom/first.musicxml", "custom/first.tab",
                "custom/first.mid", "custom/first.json", "custom/first.score.json",
                "custom/first.events.json", "custom/first.transcriber",
                "guitar", "96", "fast",
            ],
            [
                "duplicate/" + second.name, "", "", "", "", "", "", "",
                "piano", "108", "high",
            ],
        ]
        if flac_supported:
            rows.append([flac.name, "", "", "", "", "", "", "", "guitar", "", "high"])
        rows.append([broken.name, "", "", "", "", "", "", "", "guitar", "", ""])
        rows.append([bad_compressed.name, "", "", "", "", "", "", "", "guitar", "", ""])
        expected_successes = 2 + int(flac_supported)
        expected_total = expected_successes + 2
        with manifest.open("w", encoding="utf-8", newline="") as output:
            output.write(manifest_text(headers, rows))
        batch = run(
            executable,
            "--batch-manifest", manifest,
            "--batch-results", results,
            "--progress-jsonl", progress,
            "--output-dir", output_dir,
            "--concurrency", "2",
            "--memory-limit-mb", "4096",
            "--events-json", root / "enable-events.json",
            "--project", root / "enable-project.transcriber",
        )
        require(batch.returncode == 1, f"Mixed manifest should return partial-failure status: {batch.stderr}")
        require(f"2 of {expected_total} files failed" in batch.stderr,
                "Batch failure count is not reported")

        report = json.loads(results.read_text(encoding="utf-8"))
        require(report["format"] == "transcriber-batch-results" and report["schema_version"] == 1,
                "Batch result format/version is missing")
        require(report["summary"] == {
            "total": expected_total, "succeeded": expected_successes,
            "failed": 2, "skipped": 0, "cancelled": 0,
            "concurrency": 2, "memory_limit_mb": 4096,
        }, "Batch result summary does not reflect mixed outcomes")
        expected_statuses = ["success"] * expected_successes + ["failed", "failed"]
        require([item["status"] for item in report["files"]] == expected_statuses,
                "Per-file outcomes are incorrect")

        explicit_xml = root / "custom" / "first.musicxml"
        explicit_tab = root / "custom" / "first.tab"
        explicit_midi = root / "custom" / "first.mid"
        explicit_json = root / "custom" / "first.json"
        explicit_score = root / "custom" / "first.score.json"
        explicit_events = root / "custom" / "first.events.json"
        explicit_project = root / "custom" / "first.transcriber"
        require(all(path.is_file() for path in (
            explicit_xml, explicit_tab, explicit_midi, explicit_json, explicit_score,
            explicit_events, explicit_project,
        )), "Explicit relative output paths were not resolved from the manifest directory")
        require(explicit_midi.read_bytes().startswith(b"MThd")
                and json.loads(explicit_json.read_text(encoding="utf-8"))["notes"]
                and json.loads(explicit_score.read_text(encoding="utf-8"))["schema_version"] == 1,
                "Manifest MIDI, JSON or legacy score-project export is invalid")
        default_xml = output_dir / "000002_音频, take.musicxml"
        default_events = output_dir / "000002_音频, take.events.json"
        require(default_xml.is_file() and default_events.is_file(),
                "Duplicate stems did not receive distinct ordinal-prefixed output names")

        events = json.loads(explicit_events.read_text(encoding="utf-8"))
        require(events["format"] == "transcriber-note-events" and events["schema_version"] == 1,
                "Versioned note-event output contract is missing")
        require(events["timebase"]["sample_frames"] == "zero-based source frames",
                "Note-event sample timing units are undocumented")
        require(events["notes"], "Successful event JSON contains no notes")
        note = events["notes"][0]
        require(note["sample_rate_hz"] == events["source"]["sample_rate_hz"]
                and note["onset_frame"] is not None and note["offset_frame"] is not None,
                "Note events are missing sample-frame timing")
        confidence = note["confidence"]
        require(confidence["semantics"] in ("heuristic_score", "calibrated_probability", "unspecified"),
                "Note confidence provenance is not explicit")
        if confidence["semantics"] == "heuristic_score":
            require(confidence["calibrated_probability"] is None,
                    "Heuristic confidence was mislabeled as a calibrated probability")

        project = json.loads(explicit_project.read_text(encoding="utf-8"))
        require(project["project_schema_version"] == 2, "Native project export is not schema version 2")
        require(project["audio"]["reference_path"] == str(first.resolve()),
                "Native project did not preserve the source audio reference")
        require(project["audio"]["sample_rate"] == 22050
                and project["audio"]["channels"] == 1
                and project["audio"]["frame_count"] == 22050,
                "Native project audio metadata does not match the WAV source")

        progress_events = [
            json.loads(line) for line in progress.read_text(encoding="utf-8").splitlines()
        ]
        require(progress_events[0]["type"] == "batch_started"
                and progress_events[-1]["type"] == "batch_finished",
                "Progress JSONL lacks batch lifecycle events")
        require(any(event["type"] == "file_failed" and event["status"] == "failed"
                    for event in progress_events),
                "Progress JSONL omitted the per-file failure")
        require(sum(event["type"] == "file_failed" for event in progress_events) == 2,
                "Progress JSONL omitted a preflight failure")
        require(all(event["schema_version"] == 1 for event in progress_events),
                "Progress events are not versioned")

        repaired = root / "repaired.wav"
        make_wave(repaired)
        shutil.copyfile(repaired, broken)
        with manifest.open("w", encoding="utf-8", newline="") as output:
            output.write(manifest_text(headers, rows[:-1]))
        resumed = run(
            executable,
            "--batch-manifest", manifest,
            "--batch-results", results,
            "--progress-jsonl", progress,
            "--output-dir", output_dir,
            "--concurrency", "2",
            "--memory-limit-mb", "4096",
            "--events-json", root / "enable-events.json",
            "--project", root / "enable-project.transcriber",
            "--resume",
        )
        require(resumed.returncode == 0, f"Resume failed after repairing the bad input: {resumed.stderr}")
        resumed_report = json.loads(results.read_text(encoding="utf-8"))
        require(resumed_report["summary"]["succeeded"] == 1
                and resumed_report["summary"]["skipped"] == expected_successes
                and resumed_report["summary"]["failed"] == 0,
                "Resume did not skip intact successes and retry the repaired file")

        removed_output = default_xml
        removed_output.unlink()
        retried_missing = run(
            executable,
            "--batch-manifest", manifest,
            "--batch-results", results,
            "--output-dir", output_dir,
            "--concurrency", "2",
            "--memory-limit-mb", "4096",
            "--events-json", root / "enable-events.json",
            "--project", root / "enable-project.transcriber",
            "--resume",
        )
        require(retried_missing.returncode == 0,
                f"Resume failed to regenerate a missing output: {retried_missing.stderr}")
        regenerated = json.loads(results.read_text(encoding="utf-8"))
        require(regenerated["summary"]["succeeded"] == 1
                and regenerated["summary"]["skipped"] == expected_successes
                and removed_output.is_file(),
                "Resume incorrectly skipped a successful job with a missing output")

        consistency_manifest = root / "consistency.csv"
        with consistency_manifest.open("w", encoding="utf-8", newline="") as output:
            output.write(manifest_text(["input"], [[first.name], ["duplicate/" + second.name]]))
        outputs_by_concurrency = []
        for concurrency in (1, 2):
            directory = root / f"concurrency-{concurrency}"
            outcome = run(
                executable,
                "--batch-manifest", consistency_manifest,
                "--batch-results", directory / "results.json",
                "--output-dir", directory / "exports",
                "--events-json", directory / "enable-events.json",
                "--concurrency", str(concurrency),
                "--memory-limit-mb", "512",
                "--quiet",
            )
            require(outcome.returncode == 0,
                    f"Batch failed at concurrency {concurrency}: {outcome.stderr}")
            concurrent_report = json.loads((directory / "results.json").read_text(encoding="utf-8"))
            require(concurrent_report["summary"]["succeeded"] == 2,
                    f"Concurrency {concurrency} did not complete both jobs")
            outputs_by_concurrency.append([
                json.loads(path.read_text(encoding="utf-8"))["notes"]
                for path in sorted((directory / "exports").glob("*.events.json"))
            ])
        normalize_notes = lambda groups: [
            [
                (note["midi_pitch"], note["onset_seconds"], note["offset_seconds"])
                for note in group
            ]
            for group in groups
        ]
        require(normalize_notes(outputs_by_concurrency[0]) ==
                normalize_notes(outputs_by_concurrency[1]),
                "Serial and concurrent batch runs produced different note events")

        colliding_manifest = root / "colliding.csv"
        colliding_manifest.write_text(
            manifest_text(
                ["input", "musicxml", "tab"],
                [[first.name, "same.musicxml", "a.tab"],
                 ["duplicate/" + second.name, "same.musicxml", "b.tab"]],
            ),
            encoding="utf-8",
            newline="",
        )
        collision = run(
            executable,
            "--batch-manifest", colliding_manifest,
            "--output-dir", output_dir,
        )
        require(collision.returncode != 0 and "collide" in collision.stderr.lower(),
                "Manifest output collisions were not rejected before processing")

        budget_manifest = root / "budget.csv"
        with budget_manifest.open("w", encoding="utf-8", newline="") as output:
            output.write(manifest_text(["input"], [[first.name]]))
        over_budget = run(
            executable,
            "--batch-manifest", budget_manifest,
            "--output-dir", root / "over-budget",
            "--memory-limit-mb", "128",
            "--backend", "builtin:yin",
        )
        require(over_budget.returncode != 0,
                "Jobs exceeding the scheduler memory-admission budget were accepted")
        budget_report = json.loads(
            (root / "over-budget" / "batch-results.json").read_text(encoding="utf-8")
        )
        require(budget_report["summary"]["failed"] == 1
                and "memory" in budget_report["files"][0]["error"].lower(),
                "Memory admission failures were not reported per file")

    print("Batch manifest, structured results, progress, event JSON, project, collision, resume and memory checks passed")


if __name__ == "__main__":
    main()
