"""Optional independent MusicXML import/engraving checks using pinned local Verovio."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile

from interpretation_semantics import generated_references, render_case
from ui_workflow_browser import run_browser


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verovio-dist", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
    args = parser.parse_args()
    package = args.verovio_dist.parent
    metadata = json.loads((package / "package.json").read_text(encoding="utf-8"))
    if metadata.get("version") != "6.3.0" or metadata.get("license") != "LGPL-3.0-or-later":
        raise ValueError("Expected the licensed, pinned verovio@6.3.0 package")
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="transcriber-engraving-") as temporary:
        stage = Path(temporary)
        shutil.copytree(root / "ui", stage, dirs_exist_ok=True)
        shutil.copy2(root / "ui" / "index.html", stage / "workstation.html")
        vendor = stage / "vendor" / "verovio"
        vendor.mkdir(parents=True, exist_ok=True)
        for name in ("verovio.mjs", "verovio-module.mjs"):
            shutil.copy2(args.verovio_dist / name, vendor / name)
        shutil.copy2(root / "ui" / "notation-worker.js", stage / "notation-worker.js")
        shutil.copy2(root / "tests" / "notation_interop.html", stage / "index.html")
        cases = json.loads((root / "tests" / "fixtures" / "interpretation" /
                            "passages.json").read_text())["passages"] + generated_references()
        for kind, pitches in [("seven", [35, 40, 64]), ("eight", [30, 35, 64]),
                              ("four-reentrant", [60, 64, 67, 69]),
                              ("twelve", [40, 52, 64])]:
            cases.append(dict(id=f"guitar-{kind}", tempo=120, meter=[4, 4], fifths=0,
                              mode="legacy", exporter_fixture=f"guitar-{kind}",
                              notes=[[pitch, index*.5, (index+1)*.5]
                                     for index, pitch in enumerate(pitches)]))
        fixtures = []
        for case in cases:
            if case.get("expressive") or case["tempo"] == 0:
                continue
            _, prefix = render_case(args.runner.resolve(), stage, case)
            fixtures.append(prefix.name + ".roundtrip.musicxml")
        for name in ("common", "complex"):
            shutil.copy2(root / "tests" / "fixtures" / "notation" /
                         f"{name}.musicxml", stage / f"{name}.musicxml")
            fixtures.append(f"{name}.musicxml")
        long_case = dict(id="paginated-score", tempo=120, meter=[4, 4], fifths=0, mode="legacy",
                         notes=[[60+i % 8, i*.5, (i+1)*.5] for i in range(256)])
        _, prefix = render_case(args.runner.resolve(), stage, long_case)
        fixtures.append(prefix.name + ".roundtrip.musicxml")
        (stage / "manifest.json").write_text(json.dumps(fixtures), encoding="utf-8")
        return run_browser(stage, "index.html", realtime=True)


if __name__ == "__main__":
    raise SystemExit(main())
