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
        "workspace",
        "project-title",
        "new-project",
        "open-project",
        "save-project",
        "save-score-copy",
        "project-summary-button",
        "project-summary-dialog",
        "project-summary-details",
        "close-project-summary",
        "source-panel",
        "inspector-panel",
        "source-resizer",
        "inspector-resizer",
        "source-collapse",
        "inspector-collapse",
        "toggle-source",
        "toggle-inspector",
        "open-audio",
        "empty-record",
        "record-button",
        "transcribe-button",
        "cancel-button",
        "play-midi",
        "play-original",
        "stop-playback",
        "loop-toggle",
        "measure-prev",
        "measure-next",
        "measure-display",
        "tempo-auto",
        "follow-playhead",
        "input-device-label",
        "input-level-meter",
        "clipping-warning",
        "status-capture",
        "latency-value",
        "model-status",
        "detected-pitch",
        "detected-onset",
        "detected-offset",
        "detected-confidence",
        "detected-evidence",
        "accept-proposal",
        "refinement-panel",
        "refine-live",
        "refinement-state",
        "refinement-summary",
        "refinement-changes",
        "apply-refinement",
        "discard-refinement",
        "undo-refinement",
        "waveform",
        "spectrogram",
        "staff-view",
        "tab-view",
        "fingering-alternatives",
        "fingering-note",
        "confidence-heatmap",
        "review-mode",
        "review-card",
        "review-status",
        "review-limitation",
        "review-queue",
        "review-items",
        "audition-event",
        "live-events",
        "tempo-control",
        "key-control",
        "meter-control",
        "pickup-control",
        "written-transposition-control",
        "export-edits",
        "engraving-controls",
        "engraving-host",
        "engraving-status",
        "engraving-license",
        "engraving-zoom-in",
        "engraving-zoom-out",
        "engraving-fit",
    }
    require(required_ids <= parser.ids, "UI is missing required workstation elements")

    javascript = [
        ui_directory / "workspace.js",
        ui_directory / "app.js",
        ui_directory / "editor.js",
        ui_directory / "notation.js",
        ui_directory / "notation-worker.js",
    ]
    for source in javascript:
        text = source.read_text(encoding="utf-8")
        selectors = set(re.findall(r'\$\("([A-Za-z0-9_-]+)"\)', text))
        missing = selectors - parser.ids
        require(not missing, f"{source.name} references missing element IDs: {sorted(missing)}")

    workspace = javascript[0].read_text(encoding="utf-8")
    app = javascript[1].read_text(encoding="utf-8")
    editor = javascript[2].read_text(encoding="utf-8")
    webview_backend = (ui_directory.parent / "src" / "gui_webview.cpp").read_text(encoding="utf-8")
    transcription_backend = (ui_directory.parent / "src" / "gui_transcription.cpp").read_text(encoding="utf-8")
    review_backend = (ui_directory.parent / "src" / "review.cpp").read_text(encoding="utf-8")
    require("transcribe:notes" in app and "transcribe:notes" in editor,
            "Live note events are not connected to the score editor")
    require("transcribe:audio-preview" in app and "transcribe:audio-preview" in editor,
            "Audio previews are not connected to the visualizations")
    require("transcribe:fingering" in app and "transcribe:fingering" in editor,
            "Ranked fingering assignments are not connected to the score editor")
    require("note.midi = open + fret" not in editor,
            "Tablature edits must not change a note's sounding pitch")
    require("transcribe:accepted-score" in app and "musicxml" in app,
            "Accepted score MusicXML is not connected to the renderer")
    require("transcribe:engraving-select" in editor and
            "note.id === event.detail?.id" in editor,
            "Engraved note selection is not connected to canonical editor IDs")
    require("new Worker(\"notation-worker.js\"" in
            javascript[3].read_text(encoding="utf-8"),
            "The optional engraving worker is not configured")
    require("setPointerCapture" in workspace and "ArrowRight" in workspace and
            "source-collapsed" in workspace and "inspector-collapsed" in workspace,
            "Workspace resize and collapse controls are not wired")
    stylesheet = (ui_directory / "styles.css").read_text(encoding="utf-8")
    require(".transport-context { flex: 1 1 100%; min-width: 0; margin-left: 0; }" in stylesheet,
            "The mobile transport context must wrap without horizontal overflow")
    require("requestAnimationFrame" in app and "queuedLiveMessage" in app and
            "queuedAudioPreview" in app,
            "Frequent live updates are not coalesced")
    require('if (!message.asio && mode === "asio") selectMode("file")' in app,
            "A saved ASIO mode is not reset when capture is unavailable")
    require('tempo: $("tempo-auto").checked ? "auto" : $("tempo-control").value' in app and
            'key_fifths: $("key-control").value' in app and
            'written_transposition: $("written-transposition-control").value' in app,
            "Interpretation context controls are not sent with transcription requests")
    require('window.dispatchEvent(new CustomEvent("transcribe:metadata", { detail: {} }))' not in app and
            'if (currentMetadata.meter_inferred)' in editor and
            'if (currentMetadata.key_inferred)' in editor,
            "User interpretation constraints are discarded while analysis starts or completes")
    require("request.interpretation_options.tempo_bpm = tempo" in webview_backend and
            "request.interpretation_options.meter =" in webview_backend and
            "request.interpretation_options.pickup_quarters = pickup" in webview_backend and
            "interpret_notes(notes, interpretation_options)" in transcription_backend,
            "Transcription context constraints are not applied by the interpretation backend")
    require("pauseMidi" in editor and "loop-toggle" in editor and
            "measure-next" in editor and "contextBeforeLastChange" in editor,
            "Transport, measure navigation, or context undo is not wired")
    require("No alternate pitch hypotheses were supplied" in editor and
            "candidate.midi" in editor and "Math.max(10" not in editor,
            "Inspector must not invent alternate pitch results")
    require("accept-proposal" in app and "accept-proposal" in
            (ui_directory.parent / "src" / "gui_webview.cpp").read_text(encoding="utf-8"),
            "Score proposal replacement is not explicitly connected to the backend")
    require(all(action in app and action in webview_backend for action in (
                "refine-live", "apply-refinement", "discard-refinement",
                "undo-refinement")),
            "Offline refinement review controls are not connected to the backend")
    require("conflict" in app and "approved" in app and
            "conflicts_with_manual_edit" in webview_backend and
            "completion->refinement_applied" in webview_backend,
            "Manual refinement conflicts are not reviewed before application")
    require("review-mode" in app and "review_mode" in app and
            "retain_review_evidence" in transcription_backend and
            "transcribe:review" in app and "transcribe:review" in editor and
            "prioritizedReviewNotes" in editor and "chooseNote(entry.index)" in editor,
            "Optional review mode is not connected to evidence retention and inspector data")
    require("ranking_basis" in webview_backend and
            "RecordReviewDecision" in webview_backend and
            "review_decisions" in editor and "review_decisions" in webview_backend,
            "Review outcomes are not recorded through canonical score revisions")
    require("audition-event" in editor and "audition-event" in webview_backend and
            "mciSendStringW" in webview_backend,
            "Event-local audio audition is not connected to the WAV backend")
    require("not a probability" in editor and
            "No representative held-out labelled calibration dataset is configured" in review_backend,
            "Review UI does not distinguish raw ranking evidence from calibrated probabilities")
    require("Number.isFinite(note.calibrated_confidence)" in editor and
            "calibrated_confidence\\\":null" in webview_backend and
            "Calibrated correctness estimate" in editor,
            "Uncalibrated legacy detector scores are displayed as correctness probabilities")
    require("source_event_ids" in webview_backend and
            'note.source_event_ids?.includes(selectedId)' in editor and
            'note.id = source_id' not in webview_backend and
            'fingerings.find(live_id)' in webview_backend,
            "Live score notes must keep distinct canonical IDs and retain source-event identity")
    require("setBusy(busy, preserveScore = false, liveCapture = false)" in app and
            "captureActive = Boolean(busy && liveCapture)" in app,
            "Offline worker operations are incorrectly exposed as active capture")
    require("unsupported operation" in
            (ui_directory.parent / "src" / "gui_webview.cpp").read_text(encoding="utf-8"),
            "WebView2 bridge does not reject unknown commands")
    require(all(action in app and action in editor for action in (
                "transcribe:project-active", "transcribe:project-settings",
                "transcribe:project-save", "transcribe:project-copy",
                "transcribe:project-autosave")),
            "Project settings, save, or autosave are not connected to the score editor")
    require(all(action in webview_backend for action in (
                "start_project_open", "save_project_file_atomic",
                "recovery-available", "project-autosave-error")),
            "Project loading, atomic saves, recovery, or error reporting is missing")

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
