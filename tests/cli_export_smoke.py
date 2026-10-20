import json
import shutil
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


def main():
    executable = Path(sys.argv[1])
    audio = Path(sys.argv[2])
    help_result = subprocess.run(
        [str(executable), "--help"], check=True, capture_output=True, text=True
    )
    for flag in ("--live", "--quality", "--tempo", "--key-fifths", "--mode", "--meter"):
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
                "--musicxml",
                str(musicxml),
                "--tab",
                str(tab),
                "--midi",
                str(midi),
                "--json",
                str(notes_json),
                "--quiet",
            ],
            capture_output=True,
            text=True,
        )
        require(result.returncode == 0, f"CLI export failed:\n{result.stderr}")

        for path in (musicxml, tab, midi, notes_json):
            require(path.is_file() and path.stat().st_size > 0, f"Missing output: {path.name}")

        exported = ET.parse(musicxml).getroot()
        require(exported.tag == "score-partwise", "MusicXML root is not score-partwise")
        result_json = json.loads(notes_json.read_text(encoding="utf-8"))
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
        require(midi.read_bytes().startswith(b"MThd"), "MIDI header is invalid")
        require(tab.read_text(encoding="utf-8").strip(), "Tab export is empty")

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

    print("CLI exports, MusicXML semantic parsing, and batch partial-results checks passed")


if __name__ == "__main__":
    main()
