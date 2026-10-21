import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def notes(root):
    return root.findall(".//note")


def main():
    fixture_dir = Path(sys.argv[1])
    viewer_path = Path(sys.argv[2])
    worker_path = Path(sys.argv[3])
    common_path = fixture_dir / "common.musicxml"
    complex_path = fixture_dir / "complex.musicxml"
    common = ET.parse(common_path).getroot()
    complex_score = ET.parse(complex_path).getroot()
    common_notes = notes(common)
    complex_notes = notes(complex_score)

    for name, elements in (("common", common_notes), ("complex", complex_notes)):
        identifiers = [note.get("id") for note in elements]
        require(all(identifiers), f"{name} notation fixture has a note without a stable ID")
        require(len(identifiers) == len(set(identifiers)),
                f"{name} notation fixture has duplicate note IDs")
        require(len(elements) >= 6, f"{name} notation fixture is too small to review")

    require(common.find(".//time/beats") is not None and
            common.find(".//rest") is not None and
            common.find(".//dot") is not None,
            "Common fixture must exercise meter, rests, and dotted rhythm")
    require(complex_score.find(".//beats[.='6']") is not None and
            complex_score.find(".//beat-type[.='8']") is not None,
            "Complex fixture must exercise compound meter")
    require(complex_score.find(".//chord") is not None and
            complex_score.find(".//time-modification") is not None and
            complex_score.find(".//tie") is not None and
            complex_score.find(".//backup") is not None and
            complex_score.find(".//technical/string") is not None,
            "Complex fixture must exercise chords, tuplets, ties, voices, and tablature")
    viewer = viewer_path.read_text(encoding="utf-8")
    worker = worker_path.read_text(encoding="utf-8")
    require("common" in viewer and "complex" in viewer and
            "notation-worker.js" in viewer,
            "Visual fixture page does not render both fixture scores")
    require("expectedNoteIds" in viewer and "missingIds" in viewer and
            "DOMParser" in viewer,
            "Visual fixture page does not assert that MusicXML note IDs appear in SVG")
    require("renderToSVG" in worker and "loadData" in worker,
            "Notation worker does not load and engrave MusicXML")
    print("Notation fixture XML and visual-renderer wiring passed")


if __name__ == "__main__":
    main()
