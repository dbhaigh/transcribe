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
            require(values["id"] not in self.ids, f"Duplicate UI element ID: {values['id']}")
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
        "journey-guide",
        "journey-next",
        "activity-panel",
        "activity-heading",
        "activity-detail",
        "help-dialog",
        "help-button",
        "save-state",
        "phase-progress",
        "timeline-scroll",
        "detected-duration",
        "project-title",
        "new-project",
        "open-project",
        "save-project",
        "save-score-copy",
        "save-portable-project",
        "relink-project-audio",
        "project-annotation",
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
        "follow-live",
        "jump-live",
        "input-device-label",
        "input-level-meter",
        "clipping-warning",
        "status-capture",
        "live-health-strip",
        "health-driver",
        "health-callback",
        "health-window",
        "health-input-wait",
        "health-analysis-wait",
        "health-analysis-execution",
        "health-confirmation",
        "health-event-wait",
        "health-ui-dispatch",
        "health-provisional-target",
        "health-stabilized-target",
        "health-overload",
        "health-input-gap",
        "live-hop-ms",
        "json-path",
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
        ui_directory / "timeline.js",
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
    for control in ("quantization-mode", "quantization-strength", "rhythm-evidence"):
        require(control in parser.ids, f"Missing rhythm interpretation control: {control}")
    require('quantization: $("quantization-mode").value' in app and
            'quantization_strength: $("quantization-strength").value' in app and
            'minimum_rhythm_evidence: $("rhythm-evidence").value' in app and
            "parse_quantization_mode(quantization)" in webview_backend,
            "Rhythm interpretation controls must reach validated native request options")
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
    require("json: inputs.json.value" in app and
            'json: $("json-path").value' in editor and
            "serialize_score_document(result.score)" in webview_backend,
            "Accepted score JSON is not wired to the workstation export path")
    require('if (backend_id.starts_with("onnx:"))' in webview_backend and
            "options.windowed = window" in webview_backend,
            "Automatic live refinement must use windows only for ONNX backends")
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
    require("setBusy(busy, preserveScore = false, liveCapture = false, cancellable = false)" in app and
            "captureActive = Boolean(busy && liveCapture)" in app,
            "Offline worker operations are incorrectly exposed as active capture")
    require("(!cancellable && !captureActive)" in app and
            'liveCaptureState === "finishing" ? "Cancel capture" : "Stop capture"' in app and
            'send(mode === "asio" ? "start-capture" : "transcribe"' in app and
            'send("stop-capture")' in app and
            'setBusy(true, hasAcceptedScore, mode === "asio", true)' in app,
            "Versioned start, stop, and cancel capture controls are not wired")
    require('id="bounded-inference"' in html_path.read_text(encoding="utf-8") and
            "bounded_inference:" in app and
            "request.bounded_inference" in webview_backend,
            "Bounded neural windows are not wired through the interface")
    require("completion->job != state->active_transcription_job" in webview_backend and
            "post_coalesced(state, message_progress" in webview_backend and
            "state.pending_audio_preview" in webview_backend,
            "Transcription updates lack stale-job protection or bounded preview queues")
    require("metrics.overloaded" in app and "OVERLOAD:" in app and
            "function liveLatencyText(summary)" in app and
            '"unmeasured"' in app and "target_provisional_seconds" in app and
            "target_stabilized_seconds" in app and
            "health-input-gap" in app and "health-overload" in app and
            "not physical end-to-end latency" in app and
            'live_hop_ms: $("live-hop-ms").value' in app and
            "request.live_hop_ms = parse_number" in webview_backend and
            "The live hop interval is too large for this model's analysis window." in webview_backend and
            'message.type === "live-event-batch"' in app and
            "message.sequence <= lastLiveSequence" in app and
            "Number(item.revision)" in app and
            "mutation.revision <= currentRevision" in app and
            "const fingerings = message.fingerings || {}" in app and
            "stabilization" in webview_backend and
            "publication_queue" in webview_backend,
            "Versioned live deltas or latency diagnostics are not exposed")
    require(all(action in webview_backend for action in
                ("start-capture", "stop-capture", "cancel")) and
            "operation_allowed(action)" in webview_backend and
            "session->poll_events()" in webview_backend and
            "bridge->document.resync(batch.resync_snapshot)" in webview_backend and
            "SetTimer(window, live_session_timer, 17, nullptr)" in webview_backend,
            "UI-thread event draining, allowlisting, or resync is not wired")
    require(all(token in webview_backend for token in (
                "live-event-batch", "upserts", "retractions", "finalizations",
                "gaps", "metrics", "p50_ms", "p95_ms", "p99_ms",
                "ui_dispatch", "target_provisional_seconds",
                "target_stabilized_seconds", "sequence_gaps", "discontinuities",
                "recording_seconds", "callback_period_ms",
                "coalesced_events_dropped", r'\"sample_rate\":',
                r'\"start_seconds\":', r'\"end_seconds\":')) and
            "message.status" in app,
            "Versioned live event messages do not expose the required batch snapshot")
    require('id="live-hop-ms"' in html_path.read_text(encoding="utf-8") and
            'id="live-health-strip"' in html_path.read_text(encoding="utf-8") and
            "aria-label=\"Live pipeline stage timings; not physical end-to-end latency\"" in html_path.read_text(encoding="utf-8"),
            "Live health or windowed-backend hop controls are not present in the UI")
    timeline = javascript[5].read_text(encoding="utf-8")
    notation = javascript[3].read_text(encoding="utf-8")
    stylesheet = (ui_directory / "styles.css").read_text(encoding="utf-8")
    require("live_visual_state" in app and "liveRetractions" in app and
            "transcribe:live-overlays" in app and "drawUnanalysedRegions" in editor and
            "unresolvedTabNotes" in editor and
            all(token in stylesheet for token in (
                'data-live-state="provisional"', 'data-live-state="retracted"',
                ".note-legend .corrected", ".note-legend .analysis-gap",
                ".check-row:has(input:focus-visible)")),
            "Live note states, analysis gaps, or unresolved tablature are not visually wired")
    require("transcribe:viewport-manual" in timeline and
            "transcribe:viewport-manual" in app and
            "follow-live" in app and "jump-live" in app and
            "transcribe:live-capture-state" in notation,
            "Live following, manual viewport detection, or deferred engraving is not wired")
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
