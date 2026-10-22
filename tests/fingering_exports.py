"""Semantic ASCII/MusicXML checks against the same pitch and fingering assignments."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def pitches(part):
    result = []
    for note in part.findall(".//note"):
        p = note.find("pitch")
        if p is not None and note.find("tie[@type='stop']") is None:
            result.append((int(p.findtext("octave"))+1)*12 +
                          dict(C=0, D=2, E=4, F=5, G=7, A=9, B=11)[p.findtext("step")] +
                          int(p.findtext("alter", "0")))
    return sorted(result)


def main():
    runner = Path(sys.argv[1]).resolve()
    cases = [
        ("standard", [64, 59, 55, 50, 45, 40], 0, [40, 64, 88], False),
        ("drop-d", [64, 59, 55, 50, 45, 38], 0, [38, 40, 64], False),
        ("dadgad", [62, 57, 55, 50, 45, 38], 0, [38, 57, 62], False),
        ("open-d", [62, 57, 54, 50, 45, 38], 0, [38, 54, 62], False),
        ("open-g", [62, 59, 55, 50, 43, 38], 0, [38, 59, 62], False),
        ("custom", [67, 60, 55, 48, 43, 36], 0, [36, 60, 67], False),
        ("capo", [64, 59, 55, 50, 45, 40], 2, [42, 61, 66], False),
        ("high", [64, 59, 55, 50, 45, 40], 0, [100], False),
        ("slide", [64, 59, 55, 50, 45, 40], 0, [64, 69], False),
        ("standard", [64, 59, 55, 50, 45, 40], 0, [100], False),
        ("standard", [64, 59, 55, 50, 45, 40], 0, [64, 67, 71], True),
        ("standard", [64, 59, 55, 50, 45, 40], 0, [40, 42, 44, 46, 48, 50, 52], True),
        ("edit-pitch", [64, 59, 55, 50, 45, 40], 0, [64], False),
        ("seven", [64, 59, 55, 50, 45, 40, 35], 0, [35, 40, 64], False),
        ("eight", [64, 59, 55, 50, 45, 40, 35, 30], 0, [30, 35, 64], False),
        ("four-reentrant", [69, 64, 60, 67], 0, [60, 64, 67, 69], True),
        ("twelve", [64, 64, 59, 59, 55, 67, 50, 62, 45, 57, 40, 52], 0, [40, 52, 64], False),
    ]
    with tempfile.TemporaryDirectory(prefix="transcriber-guitar-exports-") as temporary:
        for index, (kind, tuning, capo, midi, chord) in enumerate(cases):
            prefix = Path(temporary) / str(index)
            payload = "120 4 4 0 expressive 1 .65 0\n"
            for i, pitch in enumerate(midi):
                onset = 0 if chord else i*(.1 if kind == "slide" else .4)
                payload += f"{pitch} {onset} {onset+(.05 if kind == 'slide' else .2)}\n"
            subprocess.run([str(runner), str(prefix), "guitar-"+kind],
                           input=payload, text=True, capture_output=True, check=True)
            score = json.loads(prefix.with_suffix(".json").read_text())
            by_id = {note["id"]: note for note in score["notes"]}
            positions = []
            for tab in score["tab_notes"]:
                note = by_id[tab["notation_note_id"]]
                if tab["string_number"] is not None:
                    require(tuning[tab["string_number"]-1]+capo+tab["fret"] == note["sounding_midi_pitch"],
                            "stored fingering changed musical pitch")
                    positions.append((tab["string_number"], tab["fret"]))
            for suffix in (".tab.txt", ".raw.tab.txt"):
                text = Path(str(prefix)+suffix).read_text()
                require("performed strings and fingers are unknown" in text, "missing suggestion disclaimer")
                body = text.split("\nNotes:\n")[1].split("\nAlternative fingerings:")[0]
                printed = [(int(a), int(b)) for a, b in re.findall(r"string (\d+) fret (\d+)", body)]
                expected_positions = [(1, 0)] if kind == "edit-pitch" and suffix == ".raw.tab.txt" else positions
                require(sorted(printed) == sorted(expected_positions), "ASCII differs from canonical fingering")
                if len(expected_positions) < len(midi):
                    require("fingering unassigned" in body, "ASCII hides infeasible assignment")
                if kind == "slide":
                    require("0/" in text and "heuristic suggestions" in text, "missing labelled slide suggestion")
                else:
                    grid = text.split("\nNotes:\n")[0]
                    tab_lines = [line for line in grid.splitlines() if re.match(r"^[A-G](?:#)?\|", line)]
                    require(all(not re.search(r"\d[hp/]", line) for line in tab_lines),
                            "default tablature invents a performance technique")
            for suffix in (".musicxml", ".roundtrip.musicxml"):
                root = ET.parse(str(prefix)+suffix).getroot()
                notation, tabpart = root.findall("part")
                require(int(tabpart.findtext("measure/attributes/staff-details/staff-lines")) == len(tuning),
                        "MusicXML staff lines do not match configured strings")
                require(len(tabpart.findall("measure/attributes/staff-details/staff-tuning")) == len(tuning),
                        "MusicXML lost string tuning")
                expected_midi = [65] if kind == "edit-pitch" and suffix == ".roundtrip.musicxml" else sorted(midi)
                require(pitches(notation) == expected_midi == pitches(tabpart),
                        "notation/tab pitches diverged or unresolved note disappeared")
                technical = tabpart.findall(".//technical")
                actual = [(int(t.findtext("string")), int(t.findtext("fret"))) for t in technical]
                expected_positions = [(1, 0)] if kind == "edit-pitch" and suffix == ".musicxml" else positions
                require(sorted(actual) == sorted(expected_positions), "MusicXML changed stored string/fret assignment")
                if len(expected_positions) < len(midi):
                    require(tabpart.find(".//other-notation") is not None, "MusicXML hides unresolved fingering")
                for t in technical:
                    require(t.find("fingering") is None, "MusicXML asserts a performed finger without evidence")
            if chord:
                require(len({p[0] for p in positions}) == len(positions), "simultaneous string collision")
    print(f"{len(cases)} guitar export cases passed: ASCII/direct MusicXML/canonical round trip pitches and assignments")


if __name__ == "__main__":
    main()
