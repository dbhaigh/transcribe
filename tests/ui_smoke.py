import re
import shutil
import subprocess
import sys
from html.parser import HTMLParser
from pathlib import Path


class UiParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.assets = []

    def handle_starttag(self, tag, attributes):
        values = dict(attributes)
        if "id" in values:
            self.ids.add(values["id"])
        if tag == "script" and values.get("src"):
            self.assets.append(values["src"])
        if tag == "link" and values.get("href"):
            self.assets.append(values["href"])


def main():
    ui_directory = Path(sys.argv[1])
    html_path = ui_directory / "index.html"
    parser = UiParser()
    parser.feed(html_path.read_text(encoding="utf-8"))
    for asset in parser.assets:
        require((ui_directory / asset).is_file(), f"Missing UI asset: {asset}")

    required_ids = {
        "waveform",
        "spectrogram",
        "staff-view",
        "tab-view",
        "confidence-heatmap",
        "live-events",
        "tempo-control",
        "key-control",
        "meter-control",
        "export-edits",
    }
    require(required_ids <= parser.ids, "UI is missing required workstation elements")

    javascript = [ui_directory / "app.js", ui_directory / "editor.js"]
    for source in javascript:
        text = source.read_text(encoding="utf-8")
        selectors = set(re.findall(r'\$\("([A-Za-z0-9_-]+)"\)', text))
        missing = selectors - parser.ids
        require(not missing, f"{source.name} references missing element IDs: {sorted(missing)}")

    app = javascript[0].read_text(encoding="utf-8")
    editor = javascript[1].read_text(encoding="utf-8")
    require("transcribe:notes" in app and "transcribe:notes" in editor,
            "Live note events are not connected to the score editor")
    require("transcribe:audio-preview" in app and "transcribe:audio-preview" in editor,
            "Audio previews are not connected to the visualizations")

    node = shutil.which("node")
    if node:
        for source in javascript:
            subprocess.run([node, "--check", str(source)], check=True)
    else:
        print("Node.js unavailable; JavaScript syntax check skipped")
    print("UI assets, required controls, and live-preview wiring passed")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


if __name__ == "__main__":
    main()
