import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def pitches_in_part(part):
    semitones = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    pitches = set()
    for pitch in part.findall(".//note/pitch"):
        step = pitch.findtext("step")
        octave = int(pitch.findtext("octave"))
        alter = int(pitch.findtext("alter", "0"))
        pitches.add((octave + 1) * 12 + semitones[step] + alter)
    return pitches


def midi_notes(path):
    data = path.read_bytes()
    require(data[:4] == b"MThd", "MIDI header is invalid")
    size, format_id, tracks, division = struct.unpack(">IHHH", data[4:14])
    require(size == 6 and format_id == 0 and tracks == 1 and division == 480,
            "Unexpected MIDI header values")
    require(data[14:18] == b"MTrk", "MIDI track header missing")
    length = struct.unpack(">I", data[18:22])[0]
    require(len(data) == 22 + length, "MIDI track length mismatch")
    pos, tick, active, notes = 22, 0, {}, []

    def vlq():
        nonlocal pos
        value = 0
        for _ in range(4):
            byte = data[pos]
            pos += 1
            value = (value << 7) | (byte & 127)
            if byte < 128:
                return value
        raise RuntimeError("Invalid MIDI variable-length quantity")

    while pos < len(data):
        tick += vlq()
        status = data[pos]
        pos += 1
        if status == 0xff:
            kind = data[pos]
            pos += 1
            count = vlq()
            pos += count
            if kind == 0x2f:
                require(not active and pos == len(data), "MIDI has hanging notes/trailing events")
                return notes
        else:
            require(status & 0xf0 in (0x80, 0x90), "Unexpected MIDI channel event")
            pitch, velocity = data[pos:pos+2]
            pos += 2
            if status & 0xf0 == 0x90 and velocity:
                active.setdefault(pitch, []).append(tick)
            else:
                require(pitch in active and tick > active[pitch][0], "MIDI has unmatched/zero-length note")
                notes.append((pitch, active[pitch].pop(0), tick))
                if not active[pitch]:
                    del active[pitch]
    raise RuntimeError("MIDI end-of-track missing")


def reliability_checks(executable, audio, output):
    def run(*arguments):
        return subprocess.run([str(executable), *map(str, arguments)], cwd=output,
                              capture_output=True, text=True, timeout=30)

    for flag, key, values in (
        ("--tempo", "tempo", ("120junk", "nan", "inf", "1e309")),
        ("--device-id", "device-id", ("1.5", "0x10", "2147483648")),
        ("--tab-alternatives", "tab-alternatives", ("-1", "6", "2junk")),
        ("--capo", "capo", ("25", "-1", "2.5")),
        ("--string-tuning", "string-tuning", ("", "64,", "64,x", "128",
                                            ",".join(["64"] * 13))),
        ("--fret-count", "fret-count", ("-1", "-2147483648", "37", "12.5")),
        ("--guitar-min-midi", "guitar-min-midi", ("-1", "128", "60x")),
        ("--guitar-max-midi", "guitar-max-midi", ("-1", "128", "60x")),
        ("--minimum-confidence", "minimum-confidence", ("0.3junk", "nan", "1.1")),
        ("--latency-ms", "latency-ms", ("30ms", "19", "51")),
        ("--asio", "asio", ("nan", "inf", "301")),
    ):
        for value in values:
            invalid = run(audio, flag, value, "--quiet")
            require(invalid.returncode != 0 and "transcriber:" in invalid.stderr,
                    f"Invalid CLI value accepted: {flag}={value}")
            config = output / "invalid-value.conf"
            config.write_text(f"{key}={value}\n", encoding="utf-8")
            invalid = run(audio, "--config", config, "--quiet")
            require(invalid.returncode != 0 and "transcriber:" in invalid.stderr,
                    f"Invalid config value accepted: {key}={value}")

    config = output / "duplicates.conf"
    config.write_text("tempo=120\ntempo=96\n")
    invalid = run("--config", config)
    require(invalid.returncode != 0 and "duplicate config" in invalid.stderr,
            "Duplicate config values silently replaced each other")
    config.write_text("tempo=96\n")
    for args in (
        ("--musicxml", audio),
        ("--json", audio),
        ("--musicxml", output / "same", "--tab", output / "same"),
        ("--musicxml", config, "--config", config),
        ("--musicxml", output / "missing-parent" / "score.xml"),
        ("--musicxml", output),
        ("--batch", output),
    ):
        invalid = run(audio, *args, "--quiet")
        require(invalid.returncode != 0, f"Unsafe output/input combination succeeded: {args}")
    require(config.read_text() == "tempo=96\n", "Output overwrote a config input")

    unicode_dir = output / "audio space \u97f3\u697d"
    unicode_dir.mkdir()
    unicode_audio = unicode_dir / "guitare \u00e9.wav"
    shutil.copyfile(audio, unicode_audio)
    unicode_config = unicode_dir / "options \u00e9.conf"
    unicode_xml = unicode_dir / "score \u97f3.musicxml"
    unicode_tab = unicode_dir / "tab \u97f3.txt"
    unicode_json = unicode_dir / "notes \u97f3.json"
    unicode_midi = unicode_dir / "notes \u97f3.mid"
    unicode_score = unicode_dir / "project \u97f3.json"
    unicode_config.write_text(f"\ufeffmusicxml={unicode_xml}\ntab={unicode_tab}\n", encoding="utf-8")
    success = run(unicode_audio, "--config", unicode_config, "--quiet",
                  "--json", unicode_json, "--midi", unicode_midi, "--score-project", unicode_score)
    require(success.returncode == 0, f"Unicode/space input/config/export failed: {success.stderr}")
    require(ET.parse(unicode_xml).getroot().tag == "score-partwise" and unicode_tab.stat().st_size,
            "Unicode exports are missing or corrupt")
    require(json.loads(unicode_json.read_text(encoding="utf-8"))["notes"] and
            json.loads(unicode_score.read_text(encoding="utf-8"))["notes"] and midi_notes(unicode_midi),
            "Unicode JSON, MIDI or score project failed semantic parsing")
    extended = run(unicode_audio, "--string-tuning", "64,59,55,50,45,40,35",
                   "--fret-count", "22", "--guitar-min-midi", "35", "--guitar-max-midi", "86",
                   "--musicxml", unicode_xml, "--tab", unicode_tab, "--score-project", unicode_score,
                   "--quiet")
    require(extended.returncode == 0, f"Extended guitar CLI failed: {extended.stderr}")
    require(int(ET.parse(unicode_xml).findtext(".//part[@id='P2']/measure/attributes/staff-details/staff-lines")) == 7,
            "Extended CLI export lost its seventh string")
    instrument = json.loads(unicode_score.read_text(encoding="utf-8"))["instrument"]
    require(len(instrument["tuning"]) == 7 and instrument["physical_fret_count"] == 22,
            "Extended guitar project lost configuration")
    unicode_config.write_text(
        "string-tuning=69,64,60,67\nfret-count=18\nguitar-min-midi=60\nguitar-max-midi=90\n",
        encoding="utf-8")
    configured = run(unicode_audio, "--config", unicode_config, "--score-project", unicode_score,
                     "--quiet")
    require(configured.returncode == 0, f"Custom tuning configuration failed: {configured.stderr}")
    instrument = json.loads(unicode_score.read_text(encoding="utf-8"))["instrument"]
    require(instrument["tuning"] == [69, 64, 60, 67] and instrument["physical_fret_count"] == 18,
            "Reentrant configuration round trip changed tuning or physical neck")
    for arguments in (("--fret-count", "1", "--capo", "2"),
                      ("--guitar-min-midi", "90", "--guitar-max-midi", "60")):
        invalid = run(unicode_audio, *arguments, "--quiet")
        require(invalid.returncode != 0 and "transcriber:" in invalid.stderr,
                f"Contradictory guitar constraints accepted: {arguments}")

    stem = unicode_dir / "isolated \u00e9.wav"
    shutil.copyfile(unicode_audio, stem)
    isolated = run(unicode_audio, "--isolated-audio", stem, "--instrument", "bass",
                   "--score-project", unicode_score, "--quiet")
    require(isolated.returncode == 0, f"Aligned-stem transcription failed: {isolated.stderr}")
    project = json.loads(unicode_score.read_text(encoding="utf-8"))
    require(project["instrument"]["name"] == "Bass" and
            any("isolation" in item for item in project["uncertainties"]),
            "Isolated transcription lost instrument/provenance warning")
    sentinel = output / "preserved-isolation.musicxml"
    sentinel.write_text("preserve")
    missing = unicode_dir / "missing stem.wav"
    failed = run(unicode_audio, "--isolated-audio", missing, "--instrument", "guitar",
                 "--musicxml", sentinel, "--quiet")
    require(failed.returncode != 0 and sentinel.read_text() == "preserve",
            "Missing stem silently succeeded or damaged prior output")
    fallback = run(unicode_audio, "--isolated-audio", missing, "--instrument", "guitar",
                   "--preprocess-fallback", "--quiet")
    require(fallback.returncode == 0 and "explicit fallback" in fallback.stderr,
            "Explicit preprocessing fallback did not disclose mixture use")
    config = output / "isolation.conf"
    config.write_text(f"isolated-audio={stem}\ninstrument=voice\npreprocess-fallback=false\n",
                      encoding="utf-8")
    configured = run(unicode_audio, "--config", config, "--quiet")
    require(configured.returncode == 0, f"Isolation config failed: {configured.stderr}")
    for arguments in (("--isolated-audio", stem, "--instrument", "none"),
                      ("--isolated-audio", stem, "--instrument", "guitar", "--musicxml", stem),
                      ("--preprocess-fallback",)):
        invalid = run(unicode_audio, *arguments, "--quiet")
        require(invalid.returncode != 0 and "transcriber:" in invalid.stderr,
                f"Invalid preprocessing request accepted: {arguments}")
    require(stem.read_bytes() == unicode_audio.read_bytes(), "Export damaged isolated audio")

    original = audio.read_bytes()
    for label, contents in (
        ("empty", b""),
        ("header", b"RIFF\xff\xff\xff\xffWAVE"),
        ("truncated", original[:-2]),
        ("missing-format", b"RIFF\x04\x00\x00\x00WAVE"),
        ("chunk-overflow", b"RIFF\x0c\x00\x00\x00WAVEJUNK\xff\xff\xff\xff"),
        ("unsupported", original[:20] + b"\x06\x00" + original[22:]),
    ):
        malformed = output / f"{label}.wav"
        malformed.write_bytes(contents)
        sentinel = output / "preserved.musicxml"
        sentinel.write_bytes(b"previous export")
        invalid = run(malformed, "--quiet", "--musicxml", sentinel)
        require(invalid.returncode != 0, f"Malformed audio was accepted: {label}")
        require(sentinel.read_bytes() == b"previous export", "Decode failure damaged a prior output")
    if os.name != "nt" and Path("/dev/full").exists():
        # The live log is intentionally a checked incremental stream, not an atomic snapshot.
        failed = run(audio, "--live-pipeline", "--live-events", "/dev/full", "--quiet")
        require(failed.returncode != 0, "Live export silently succeeded after a disk-full write")
    require(not list(output.rglob("*.tmp-*")), "Failed exports left temporary files")


def main():
    executable = Path(sys.argv[1])
    audio = Path(sys.argv[2])
    help_result = subprocess.run(
        [str(executable), "--help"], check=True, capture_output=True, text=True
    )
    for flag in ("--live", "--quality", "--tempo", "--key-fifths", "--mode", "--meter",
                 "--pickup", "--written-transposition", "--backend", "--score-project",
                 "--quantization", "--quantization-strength", "--minimum-rhythm-evidence"):
        require(flag in help_result.stdout, f"CLI help is missing {flag}")
    live_validation = subprocess.run(
        [str(executable), "--live", "0"], capture_output=True, text=True
    )
    require(
        live_validation.returncode != 0
        and "ASIO capture duration" in live_validation.stderr,
        "--live is not accepted as an ASIO capture alias",
    )

    with tempfile.TemporaryDirectory(prefix="transcribe-cli-smoke-") as directory:
        output = Path(directory)
        reliability_checks(executable, audio, output)
        for backend, extra in (("builtin:yin", []), ("builtin:spectral", ["--polyphonic"])):
            predictions = []
            for registered in (False, True):
                path = output / f"{backend.split(':')[1]}-{registered}.json"
                command = [str(executable), str(audio), "--json", str(path), "--quiet",
                           "--musicxml", str(output / "adapter.musicxml"), "--tab", str(output / "adapter.tab")]
                command.extend(["--backend", backend] if registered else extra)
                completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
                require(completed.returncode == 0, completed.stderr)
                predictions.append(json.loads(path.read_text())["notes"])
            require(predictions[0] == predictions[1], f"Registered CLI backend changed notes: {backend}")
        missing = subprocess.run([str(executable), str(audio), "--backend", "missing", "--quiet"],
                                 capture_output=True, text=True, timeout=30)
        require(missing.returncode != 0 and "Unknown model ID" in missing.stderr,
                "Unknown model registry ID was not reported")
        defaults = output / "defaults.conf"
        defaults.write_text("quality=high\nmode=major\n", encoding="utf-8")
        invalid_config = output / "invalid.conf"
        invalid_config.write_text("minimim-confidence=0.3\n", encoding="utf-8")
        invalid_config_result = subprocess.run(
            [str(executable), "--config", str(invalid_config), "--help"],
            capture_output=True,
            text=True,
        )
        require(
            invalid_config_result.returncode != 0
            and "Unknown config key: minimim-confidence" in invalid_config_result.stderr,
            "A misspelled config key was silently ignored",
        )

        musicxml = output / "score.musicxml"
        tab = output / "score.tab.txt"
        midi = output / "score.mid"
        notes_json = output / "notes.json"
        score_project = output / "score-project.json"
        result = subprocess.run(
            [
                str(executable),
                "--config",
                str(defaults),
                str(audio),
                "--quality",
                "fast",
                "--tempo",
                "96",
                "--key-fifths",
                "2",
                "--mode",
                "minor",
                "--meter",
                "3/4",
                "--pickup",
                "1",
                "--musicxml",
                str(musicxml),
                "--tab",
                str(tab),
                "--midi",
                str(midi),
                "--json",
                str(notes_json),
                "--score-project",
                str(score_project),
                "--quiet",
            ],
            capture_output=True,
            text=True,
        )
        require(result.returncode == 0, f"CLI export failed:\n{result.stderr}")

        for path in (musicxml, tab, midi, notes_json, score_project):
            require(path.is_file() and path.stat().st_size > 0, f"Missing output: {path.name}")

        exported = ET.parse(musicxml).getroot()
        require(exported.tag == "score-partwise", "MusicXML root is not score-partwise")
        result_json = json.loads(notes_json.read_text(encoding="utf-8"))
        canonical_project = json.loads(score_project.read_text(encoding="utf-8"))
        require(canonical_project["schema_version"] == 1,
                "Score project schema version is missing or unsupported")
        require(canonical_project["notes"] and canonical_project["source_events"],
                "Score project is missing canonical notes or source-event provenance")
        require(canonical_project["tab_notes"][0]["notation_note_id"] ==
                canonical_project["notes"][0]["id"],
                "Score project tablature does not link to its notation note")
        expressive_project = output / "expressive.json"
        expressive_config = output / "expressive.conf"
        expressive_config.write_text(
            "quantization=evidence\nquantization-strength=0.75\nminimum-rhythm-evidence=0.8\n",
            encoding="utf-8")
        expressive = subprocess.run(
            [str(executable), "--config", str(expressive_config), str(audio), "--quiet",
             "--tempo", "120", "--meter", "4/4", "--quantization", "expressive",
             "--quantization-strength", "1", "--minimum-rhythm-evidence", "0.65",
             "--score-project", str(expressive_project),
             "--musicxml", str(output / "expressive.musicxml"), "--tab", str(output / "expressive.tab")],
            capture_output=True, text=True, timeout=30)
        require(expressive.returncode == 0, expressive.stderr)
        expressive_score = json.loads(expressive_project.read_text())
        require(expressive_score["notes"], "Expressive CLI export contains no notes")
        for note in expressive_score["notes"]:
            require(not note["rhythm"]["symbolic"], "Expressive mode forced a symbolic rhythm")
            require(abs(note["onset_quarters"] - note["measured_onset_seconds"]*2) < 1e-8,
                    "Expressive mode moved a measured onset")
            require(abs(note["duration_quarters"] -
                        (note["measured_offset_seconds"]-note["measured_onset_seconds"])*2) < 1e-8,
                    "Expressive mode moved a measured offset")
        for flag, value in (("--quantization", "bad"), ("--quantization-strength", "nan"),
                            ("--quantization-strength", "1.1"), ("--minimum-rhythm-evidence", "-0.1")):
            invalid = subprocess.run([str(executable), str(audio), flag, value],
                                     capture_output=True, text=True, timeout=30)
            require(invalid.returncode != 0 and "Quantization" in invalid.stderr,
                    f"Invalid rhythm option was not rejected: {flag} {value}")
        score_parts = exported.findall("./part-list/score-part")
        require(len(score_parts) >= 2, "MusicXML lacks synchronized notation/tab parts")
        notation_id = score_parts[0].attrib["id"]
        notation = exported.find(f"./part[@id='{notation_id}']")
        require(notation is not None, "MusicXML notation part is missing")
        tablature = exported.find(f"./part[@id='{score_parts[1].attrib['id']}']")
        require(tablature is not None, "MusicXML tablature part is missing")
        require(
            notation.findtext(".//attributes/key/fifths") == "2",
            "MusicXML key override was not applied",
        )
        require(
            notation.findtext(".//attributes/key/mode") == "minor",
            "MusicXML mode override was not applied",
        )
        require(
            notation.findtext(".//attributes/time/beats") == "3"
            and notation.findtext(".//attributes/time/beat-type") == "4",
            "MusicXML meter override was not applied",
        )
        require(
            notation.find("./measure").attrib.get("implicit") == "yes",
            "MusicXML pickup measure was not marked implicit",
        )
        sound = exported.find(".//sound")
        require(sound is not None and float(sound.attrib["tempo"]) == 96.0,
                "MusicXML tempo override was not applied")

        require(result_json["notes"], "JSON export contains no note events")
        json_pitches = {note["midi"] for note in result_json["notes"]}
        require(json_pitches <= pitches_in_part(notation),
                "MusicXML notation pitches do not round-trip to the JSON note events")
        require(json_pitches <= pitches_in_part(tablature),
                "MusicXML tablature pitches do not round-trip to the JSON note events")
        require(result_json["metadata"]["tempo_bpm"] == 96.0,
                "JSON tempo does not match the CLI override")
        require(result_json["metadata"]["time_signature"] == "3/4",
                "JSON meter does not match the CLI override")
        midi_events = midi_notes(midi)
        require({event[0] for event in midi_events} == json_pitches,
                "MIDI pitches do not match JSON events")
        require(len(midi_events) == len(canonical_project["notes"]),
                "MIDI event count differs from the canonical score")
        expected_starts = sorted((note["sounding_midi_pitch"], int(note["onset_quarters"] * 480 + .5))
                                 for note in canonical_project["notes"])
        expected_ends = sorted((note["sounding_midi_pitch"],
                               int((note["onset_quarters"] + note["duration_quarters"]) * 480 + .5))
                              for note in canonical_project["notes"])
        require(sorted((pitch, start) for pitch, start, end in midi_events) == expected_starts and
                sorted((pitch, end) for pitch, start, end in midi_events) == expected_ends,
                "MIDI note-on/off times differ from canonical score quarters")
        require(tab.read_text(encoding="utf-8").strip(), "Tab export is empty")

        transposed_musicxml = output / "transposed.musicxml"
        transposed = subprocess.run(
            [
                str(executable), str(audio), "--quiet",
                "--meter", "9/8", "--pickup", "0.5",
                "--written-transposition", "2",
                "--musicxml", str(transposed_musicxml),
                "--tab", str(output / "transposed.tab"),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        require(transposed.returncode == 0, f"Constrained score export failed:\n{transposed.stderr}")
        transposed_root = ET.parse(transposed_musicxml).getroot()
        transposed_parts = transposed_root.findall("./part-list/score-part")
        transposed_notation = transposed_root.find(
            f"./part[@id='{transposed_parts[0].attrib['id']}']"
        )
        require(transposed_notation is not None, "Transposed notation part is missing")
        require(
            transposed_notation.findtext(".//attributes/time/beats") == "9"
            and transposed_notation.findtext(".//attributes/time/beat-type") == "8",
            "Compound meter override was not applied",
        )
        require(
            transposed_notation.find("./measure").attrib.get("implicit") == "yes",
            "Constrained pickup was not written as an implicit measure",
        )
        require(
            transposed_notation.findtext(".//attributes/transpose/chromatic") == "-2",
            "Written transposition was not represented in MusicXML",
        )
        require(
            pitches_in_part(transposed_notation) == {pitch + 2 for pitch in json_pitches},
            "Written transposition did not shift notation pitches by two semitones",
        )

        batch_input = output / "batch-input"
        batch_output = output / "batch-output"
        batch_input.mkdir()
        shutil.copyfile(audio, batch_input / "valid.wav")
        (batch_input / "broken.wav").write_bytes(b"not a wave file")
        batch = subprocess.run(
            [
                str(executable),
                "--batch",
                str(batch_input),
                "--output-dir",
                str(batch_output),
                "--quiet",
            ],
            capture_output=True,
            text=True,
        )
        require(batch.returncode != 0 and "1 of 2 files failed" in batch.stderr,
                "Batch mode did not report a recoverable per-file failure")
        require(
            (batch_output / "valid.musicxml").is_file()
            and (batch_output / "valid.tab.txt").is_file(),
            "Batch mode failed to preserve outputs for the successful file",
        )

        live_json = output / "live.json"
        events_path = output / "live-events.jsonl"
        proposal_path = output / "proposal.json"
        live = subprocess.run(
            [str(executable), str(audio), "--live-pipeline", "--quiet",
             "--musicxml", str(output / "live.musicxml"), "--tab", str(output / "live.txt"),
             "--json", str(live_json), "--live-events", str(events_path),
             "--live-target-ms", "200", "--live-stabilized-target-ms", "300", "--live-hop-ms", "100",
             "--refine", "--refine-backend", "builtin:yin", "--refine-mode", "windowed",
             "--proposal-json", str(proposal_path)],
            capture_output=True, text=True,
        )
        require(live.returncode == 0, f"Live pipeline CLI failed: {live.stderr}")
        live_notes = json.loads(live_json.read_text(encoding="utf-8"))["notes"]
        require(live_notes, "Live pipeline exported no notes")
        events = [json.loads(line) for line in events_path.read_text(encoding="utf-8").splitlines()]
        require(events and events[-1]["kind"] == "status" and events[-1]["message"] == "finished",
                "Live event stream did not finish cleanly")
        require([e["sequence"] for e in events] == sorted({e["sequence"] for e in events}),
                "Live event sequence numbers are not strictly increasing")
        require(all(e["note"]["id"] for e in events if e["note"] is not None),
                "Live note events lack stable identifiers")
        finalized = [e for e in events if e["kind"] == "note_finalized"]
        require(finalized and all(e["note"]["finalized"] for e in finalized),
                "Finished live events lack explicit final revisions")
        for arguments in (
                ("--live-target-ms", "200"),
                ("--live-pipeline", "--live-target-ms", "0"),
                ("--live-pipeline", "--live-target-ms", "400", "--live-stabilized-target-ms", "300"),
                ("--live-pipeline", "--live-hop-ms", "1")):
            invalid = subprocess.run([str(executable), str(audio), *arguments, "--quiet"],
                                     capture_output=True, text=True)
            require(invalid.returncode != 0, f"Invalid live timing accepted: {arguments}")
        proposal = json.loads(proposal_path.read_text(encoding="utf-8"))
        require(proposal["applied"] is False and proposal["notes"],
                "Refinement proposal must be separate from the live score")
        rejected = subprocess.run([str(executable), str(audio), "--refine"],
                                  capture_output=True, text=True)
        require(rejected.returncode != 0 and "--live-pipeline" in rejected.stderr,
                "--refine without --live-pipeline was not rejected")
        rejected_mode = subprocess.run(
            [str(executable), str(audio), "--refine-mode", "windowed"],
            capture_output=True, text=True,
        )
        require(rejected_mode.returncode != 0 and "--refine-mode" in rejected_mode.stderr,
                "--refine-mode without --refine was not rejected")

    print("CLI exports, MusicXML semantic parsing, batch partial-results and live pipeline checks passed")


if __name__ == "__main__":
    main()
