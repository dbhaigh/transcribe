(() => {
  const $ = (id) => document.getElementById(id);
  const inputs = {
    input: $("input-path"),
    isolated: $("isolated-path"),
    model: $("model-path"),
    musicxml: $("musicxml-path"),
    tab: $("tab-path"),
    midi: $("midi-path"),
    json: $("json-path"),
  };
  const colors = ["#74e4bd", "#a994ff", "#e9c578", "#6ccfe0", "#ff9b8e", "#96b5ff"];
  let mode = "file";
  let notes = [];
  let duration = 1;
  let recentEvents = [];
  let queuedLiveMessage = null;
  let liveFrame = 0;
  let queuedAudioPreview = null;
  let audioPreviewFrame = 0;
  let inputPeak = 0;
  let hasAcceptedScore = false;
  let hasPendingProposal = false;
  let hasLiveRecording = false;
  let hasPendingRefinement = false;
  let hasUnexportedEdits = false;
  let projectDirty = false;
  let currentProjectPath = "";
  let activeProjectTitle = "";
  let projectSummaryText = "No project is open.";
  let pendingSaveAsScoreOnly = false;
  let autosaveTimer = 0;
  let projectEditGeneration = 0;
  let captureActive = false;
  let liveCaptureState = "idle";
  let liveStatusJob = 0;
  let lastLiveSequence = 0;
  let liveNotesById = new Map();
  let liveNoteRevisions = new Map();
  let liveRetractions = new Map();
  let liveCorrectionTimers = new Map();
  let liveGapRegions = [];
  let acceptedNotesBeforeLiveCapture = null;
  let liveViewportSpan = 12;
  let applyingLiveBatch = false;
  let stopRequested = false;
  let asioDevices = [];
  let savedAudioDeviceId = "";
  let savedSampleRate = "";
  let savedChannels = "";
  let timelineDuration = 1;
  let cursorTime = null;
  let sourceChanged = false;
  let selectedNoteId = "";
  let analysisPaused = false;
  let recordingSeconds = 0;
  let cancelPending = false;
  let rollIndex = window.TranscribeTimeline.index([]);
  let pitchLow = 60;
  let pitchHigh = 72;

  function clearQueuedPreviews() {
    cancelAnimationFrame(liveFrame);
    cancelAnimationFrame(audioPreviewFrame);
    liveFrame = 0;
    audioPreviewFrame = 0;
    queuedLiveMessage = null;
    queuedAudioPreview = null;
    for (const timer of liveCorrectionTimers.values()) clearTimeout(timer);
    liveCorrectionTimers.clear();
    liveRetractions.clear();
  }

  function send(action, values = {}) {
    if (window.chrome && window.chrome.webview) {
      const payload = ["start-capture", "stop-capture", "cancel"].includes(action)
        ? { ...values, version: "1" } : values;
      window.chrome.webview.postMessage(`${action}|${new URLSearchParams(payload).toString()}`);
    }
  }

  function updateReviewModeState() {
    const enabled = mode === "file" && $("review-mode").checked;
    $("review-mode").disabled = $("transcribe-button").disabled || mode !== "file";
    window.dispatchEvent(new CustomEvent("transcribe:review-enabled", {
      detail: { enabled },
    }));
  }

  function setStatus(text, percent) {
    if ($("status-text").textContent !== text) $("status-text").textContent = text;
    $("status-text").title = text;
    const known = Number.isFinite(percent);
    const value = known ? Math.round(Math.max(0, Math.min(100, percent))) : 0;
    $("progress-value").textContent = known ? `${value}% phase` : "Working";
    $("progress-bar").style.width = `${value}%`;
    $("phase-progress").classList.toggle("indeterminate", !known);
    $("phase-progress").setAttribute("aria-valuetext",
      known ? `${text}: ${value}% of current phase` : `${text}: progress unavailable`);
    if (known) $("phase-progress").setAttribute("aria-valuenow", String(value));
    else $("phase-progress").removeAttribute("aria-valuenow");
    if (["processing", "canceling"].includes($("activity-panel").dataset.state)) {
      $("activity-detail").textContent = text;
    }
    if (text.toLowerCase().includes("failed") || text.toLowerCase().includes("error")) {
      setCaptureState("Backend error", "error");
    }
  }

  function liveLatencyText(summary) {
    if (!summary || !(Number(summary.count) > 0)) return "unmeasured";
    const p50 = Number(summary.p50_ms);
    const p95 = Number(summary.p95_ms);
    if (!Number.isFinite(p50) || !Number.isFinite(p95)) return "unmeasured";
    return `p50 ${p50.toFixed(1)} / p95 ${p95.toFixed(1)} ms`;
  }

  function updateLiveHealth(message) {
    const metrics = message.metrics || {};
    const rawDriverLatency = message.driver_input_latency_ms;
    const driverLatency = Number(rawDriverLatency);
    const driverKnown = rawDriverLatency !== null && rawDriverLatency !== undefined &&
      Number.isFinite(driverLatency);
    $("health-driver").textContent = driverKnown
      ? `${driverLatency.toFixed(1)} ms reported` : "unknown";
    const callbackPeriod = Number(message.callback_period_ms);
    $("health-callback").textContent = message.callback_period_ms !== null &&
      message.callback_period_ms !== undefined && Number.isFinite(callbackPeriod) &&
      callbackPeriod > 0
      ? `${callbackPeriod.toFixed(1)} ms` : "unmeasured";
    $("health-window").textContent = liveLatencyText(metrics.analysis_window);
    $("health-input-wait").textContent = liveLatencyText(metrics.capture);
    $("health-analysis-wait").textContent = liveLatencyText(metrics.analysis_queue);
    $("health-analysis-execution").textContent = liveLatencyText(metrics.analysis);
    $("health-confirmation").textContent = liveLatencyText(metrics.stabilization);
    $("health-event-wait").textContent = liveLatencyText(metrics.publication_queue);
    $("health-ui-dispatch").textContent = liveLatencyText(metrics.ui_dispatch);

    const showTarget = (id, misses, summary, configuredTarget, defaultTarget) => {
      const count = Number(summary?.count);
      const targetSeconds = Number(configuredTarget);
      const targetMs = (Number.isFinite(targetSeconds) && targetSeconds > 0
        ? targetSeconds : defaultTarget) * 1000;
      $(id).textContent = Number.isFinite(count) && count > 0
        ? `${Number(misses) || 0} misses / ${count} observations · target ${targetMs.toFixed(0)} ms`
        : `unmeasured · target ${targetMs.toFixed(0)} ms`;
    };
    showTarget("health-provisional-target", metrics.provisional_target_misses,
      metrics.end_to_end, metrics.target_provisional_seconds, 0.2);
    showTarget("health-stabilized-target", metrics.stabilized_target_misses,
      metrics.stabilization, metrics.target_stabilized_seconds, 0.3);

    const count = (name) => Number(metrics[name]) || 0;
    const inputDepth = count("input_queue_depth");
    const inputCapacity = count("input_queue_capacity");
    const eventDepth = count("event_queue_depth");
    const eventCapacity = count("event_queue_capacity");
    const refusedBlocks = count("blocks_refused");
    const droppedEvents = count("events_dropped");
    const coalescedEvents = Number(message.coalesced_events_dropped) || 0;
    const skippedFrames = count("skipped_frames");
    const coalescedWindows = count("windows_coalesced");
    const refusedNotes = count("tracked_notes_refused");
    const overloaded = Boolean(metrics.overloaded);
    const overload = $("health-overload");
    overload.dataset.state = overloaded ? "active" : "none";
    overload.textContent = `OVERLOAD: ${overloaded ? "active" : "none observed"} · queues ${inputDepth}/${inputCapacity}, ${eventDepth}/${eventCapacity} · refused ${refusedBlocks} blocks/${refusedNotes} notes · dropped ${droppedEvents} events/${coalescedEvents} UI deltas · skipped ${skippedFrames} frames · coalesced ${coalescedWindows} windows`;

    const gapFrames = count("gap_frames");
    const sequenceGaps = count("sequence_gaps");
    const discontinuities = count("discontinuities");
    const inputGap = gapFrames > 0 || sequenceGaps > 0 || discontinuities > 0;
    const gap = $("health-input-gap");
    gap.dataset.state = inputGap ? "active" : "none";
    gap.textContent = `INPUT GAP: ${inputGap ? "detected" : "none observed"} · ${gapFrames} missing frames · ${sequenceGaps} sequence gaps · ${discontinuities} discontinuities`;

    const driver = driverKnown
      ? `${driverLatency.toFixed(1)} ms reported` : "unknown";
    const diagnostic = `Stage timings only; not physical end-to-end latency. Driver ${driver}, callback ${$("health-callback").textContent}, window ${$("health-window").textContent}, input wait ${$("health-input-wait").textContent}, analysis wait ${$("health-analysis-wait").textContent}, analysis ${$("health-analysis-execution").textContent}, confirmation ${$("health-confirmation").textContent}, event wait ${$("health-event-wait").textContent}, native UI dispatch ${$("health-ui-dispatch").textContent}. Input queue ${inputDepth}/${inputCapacity}, event queue ${eventDepth}/${eventCapacity}.`;
    $("live-diagnostics").textContent = diagnostic;
    $("live-health-strip").hidden = false;
  }

  function setActivity(state, heading, detail) {
    $("activity-panel").dataset.state = state;
    $("activity-heading").textContent = heading;
    $("activity-detail").textContent = detail;
    updateJourney();
  }

  function updateJourney() {
    const ready = Boolean(inputs.input.value.trim()) || mode === "asio";
    const reviewing = hasAcceptedScore && notes.length > 0;
    const step = reviewing ? "review"
      : $("transcribe-button").disabled ? "analyze" : ready ? "setup" : "open";
    ["open", "setup", "analyze", "review"].forEach((name) => {
      if (name === step) $(`journey-${name}`).setAttribute("aria-current", "step");
      else $(`journey-${name}`).removeAttribute("aria-current");
    });
    $("journey-next").textContent = reviewing
      ? "Select a note to inspect evidence and edit. Save the project to keep your work, or Export for other notation software."
      : step === "analyze"
        ? "Analysis runs in the backend. You can cancel transcription; saving and export must finish writing safely."
        : ready
          ? "Choose an instrument prior and analysis mode in the source panel, then select Transcribe. Existing output paths can be changed under export settings."
          : "Use Open audio to choose a recording. No model download is required.";
  }

  function setCaptureState(text, state = "ready") {
    $("status-capture").textContent = text;
    $("status-light").dataset.state = state;
  }

  function updateModelStatus() {
    const config = window.transcribeConfig || {};
    const model = inputs.model.value.trim();
    if (mode === "asio") {
      const backend = $("live-backend").value;
      if (backend === "spectral") {
        $("model-status").textContent =
          "Built-in spectral tracker · overlapping windows";
      } else if (backend === "onnx" && model && config.onnx) {
        $("model-status").textContent =
          `ONNX Runtime · CPU · overlapping windows · ${model.split(/[\\/]/).pop()}`;
      } else if (backend === "onnx") {
        $("model-status").textContent =
          "Choose an ONNX model; streaming uses overlapping windows";
      } else {
        $("model-status").textContent =
          "Built-in YIN · incremental · monophonic";
      }
      return;
    }
    if (model && config.onnx) {
      $("model-status").textContent =
        `ONNX Runtime · CPU · ${model.split(/[\\/]/).pop()}`;
    } else if (model) {
      $("model-status").textContent = "ONNX model selected but unavailable in this build";
    } else {
      $("model-status").textContent = "Built-in YIN · monophonic";
    }
  }

  function updateProjectTitle() {
    $("save-state").textContent = projectDirty || hasUnexportedEdits
      ? "Unsaved edits" : "No unsaved edits";
    $("save-state").dataset.dirty = String(projectDirty || hasUnexportedEdits);
    if (activeProjectTitle) {
      $("project-title").textContent =
        `${activeProjectTitle}${projectDirty ? " *" : ""}`;
      return;
    }
    const path = inputs.input.value.trim();
    $("project-title").textContent = (mode === "asio"
      ? "Live ASIO capture"
      : path ? path.split(/[\\/]/).pop() : "Untitled session") +
      (projectDirty || hasUnexportedEdits ? " *" : "");
  }

  function acceptProjectInfo(project, preserveSetup = false) {
    if (!project) return;
    currentProjectPath = project.project_path || "";
    activeProjectTitle = project.title || "Untitled project";
    updateProjectSummary(project);
    if (Object.hasOwn(project, "audio_path")) {
      inputs.input.value = project.audio_path || "";
      if (inputs.input.value) selectMode("file");
    }
    if (project.exports) {
      if (project.exports.musicxml) inputs.musicxml.value = project.exports.musicxml;
      if (project.exports.tab) inputs.tab.value = project.exports.tab;
      if (project.exports.midi) inputs.midi.value = project.exports.midi;
    }
    const settings = project.settings || {};
    $("project-annotation").value = project.annotation_text || "";
    if (!preserveSetup) {
      if (settings.quantization) $("quantization-mode").value = settings.quantization;
      if (Number.isFinite(settings.quantization_strength))
        $("quantization-strength").value = String(settings.quantization_strength);
      if (Number.isFinite(settings.minimum_rhythm_evidence))
        $("rhythm-evidence").value = String(settings.minimum_rhythm_evidence);
      if (Object.hasOwn(settings, "retain_review_evidence"))
        $("review-mode").checked = settings.retain_review_evidence;
      if (Object.hasOwn(settings, "bounded_inference"))
        $("bounded-inference").checked = settings.bounded_inference;
      if (Object.hasOwn(settings, "tempo_automatic")) {
        $("tempo-auto").checked = Boolean(settings.tempo_automatic);
      }
      if (Number.isFinite(Number(settings.tempo_bpm))) {
        $("tempo-control").value = String(Math.round(Number(settings.tempo_bpm)));
      }
      if (Object.hasOwn(settings, "meter_automatic")) {
        $("meter-control").value = settings.meter_automatic
          ? "auto"
          : `${settings.meter_numerator}/${settings.meter_denominator}`;
      }
      if (Object.hasOwn(settings, "key_automatic")) {
        $("key-control").value = settings.key_automatic
          ? "auto" : String(settings.key_fifths);
      }
      if (Object.hasOwn(settings, "minor_key")) {
        $("minor-control").checked = Boolean(settings.minor_key);
      }
      if (Number.isFinite(Number(settings.pickup_quarters))) {
        $("pickup-control").value = String(settings.pickup_quarters);
      }
      if (Number.isFinite(Number(settings.written_transposition))) {
        $("written-transposition-control").value =
          String(settings.written_transposition);
      }
      if (settings.tuning_preset) {
        $("guitar-tuning").value = settings.tuning_preset;
      }
      if (Number.isFinite(Number(settings.capo_fret))) {
        $("capo-fret").value = String(settings.capo_fret);
      }
      if (settings.instrument_prior) {
        $("instrument-prior").value = settings.instrument_prior === "none" ? "auto" : settings.instrument_prior;
      }
      if (Object.hasOwn(settings, "model_path")) inputs.model.value = settings.model_path;
      if (settings.live_backend) $("live-backend").value = settings.live_backend;
      if (Object.hasOwn(settings, "polyphonic")) {
        $("polyphonic").checked = Boolean(settings.polyphonic);
      }
      if (Object.hasOwn(settings, "fast_preview")) {
        $("quality-mode").value = settings.fast_preview ? "fast" : "quality";
      }
      if (Object.hasOwn(settings, "preserve_timing")) {
        $("preserve-timing").checked = Boolean(settings.preserve_timing);
      }
      if (Number.isFinite(Number(settings.latency_ms))) {
        $("latency-ms").value = String(settings.latency_ms);
      }
    }
    window.dispatchEvent(new CustomEvent("transcribe:project-active", {
      detail: { active: true },
    }));
    if (!preserveSetup) {
      window.dispatchEvent(new CustomEvent("transcribe:project-settings", {
        detail: settings,
      }));
    }
    projectDirty = false;
    updateTempoControlState();
    updateAnalysisControlState();
    updateModelStatus();
    savePaths();
    updateProjectTitle();
  }

  function updateProjectSummary(project) {
    projectSummaryText = project?.summary || "Project summary is unavailable.";
    $("project-summary-details").textContent = projectSummaryText;
  }

  function requestSaveProject(scoreOnly = false) {
    if (!activeProjectTitle) {
      showError("Create or open a project before saving score edits.");
      return;
    }
    pendingSaveAsScoreOnly = scoreOnly;
    if (scoreOnly || !currentProjectPath) {
      send("browse", {
        field: scoreOnly ? "project-copy" : "project-save",
        current: currentProjectPath,
      });
      return;
    }
    window.dispatchEvent(new CustomEvent("transcribe:project-save", {
      detail: { path: currentProjectPath, scoreOnly: false },
    }));
  }

  function openProjectPicker() {
    if (!confirmDiscardCurrentProject("open another project")) return;
    send("browse", { field: "project-open", current: currentProjectPath });
  }

  function scheduleProjectAutosave() {
    if (!hasAcceptedScore || !activeProjectTitle) return;
    projectEditGeneration += 1;
    window.clearTimeout(autosaveTimer);
    autosaveTimer = window.setTimeout(() => {
      window.dispatchEvent(new CustomEvent("transcribe:project-autosave", {
        detail: { generation: projectEditGeneration },
      }));
    }, 700);
  }

  function confirmDiscardCurrentProject(operation) {
    if (!projectDirty && !hasUnexportedEdits) return true;
    return window.confirm(
      `The current project has changes that may not be in its named project file. ` +
      `Autosave recovery is enabled. Continue to ${operation}?`
    );
  }

  function updateInputLevel(waveform, replace) {
    const peak = Array.isArray(waveform)
      ? waveform.reduce((maximum, sample) => Math.max(maximum, Math.abs(Number(sample) || 0)), 0)
      : 0;
    inputPeak = replace ? peak : Math.max(inputPeak, peak);
    $("input-level-meter").value = Math.min(1, inputPeak);
    $("input-peak").textContent = inputPeak > 0
      ? `${(20 * Math.log10(inputPeak)).toFixed(1)} dBFS`
      : "— dBFS";
    const clipped = inputPeak >= .999;
    $("clipping-warning").textContent = clipped
      ? "Clipping detected in preview samples" : "No clipping detected in preview samples";
    $("clipping-warning").parentElement.classList.toggle("clipping", clipped);
  }

  function updateAnalysisControlState() {
    $("bounded-inference").disabled = $("transcribe-button").disabled ||
      mode !== "file" || !window.transcribeConfig?.onnx;
    const builtInAnalysis = !inputs.model.value.trim();
    const disabled = $("transcribe-button").disabled || !builtInAnalysis;
    $("instrument-prior").disabled = $("transcribe-button").disabled;
    $("quality-mode").disabled = disabled;
    $("live-backend").disabled = $("transcribe-button").disabled;
    ["isolation-enabled", "isolated-path", "isolation-reviewed", "preprocessing-fallback",
      "isolation-preview-start", "preview-mixture", "preview-isolated"].forEach(id => {
      $(id).disabled = $("transcribe-button").disabled || mode !== "file";
    });
    const onnxOption = $("live-backend").querySelector('option[value="onnx"]');
    onnxOption.disabled = !window.transcribeConfig?.onnx;
    $("analysis-explanation").textContent = !builtInAnalysis
      ? "The selected local model defines analysis behavior. Instrument remains a prior; analysis mode does not override the model."
      : mode === "asio"
        ? "Live backend controls mono/polyphonic operation. Pause analysis keeps recording."
        : "Built-in YIN is monophonic. Instrument is a prior, not separation; fast preview may miss notes.";
  }

  function updateTempoControlState() {
    $("tempo-control").disabled =
      $("transcribe-button").disabled || $("tempo-auto").checked;
  }

  function setBusy(busy, preserveScore = false, liveCapture = false, cancellable = false) {
    cancelPending = false;
    $("workspace").setAttribute("aria-busy", String(busy));
    if (busy) setActivity("processing", liveCapture ? "Recording audio" : "Processing",
      $("status-text").textContent);
    captureActive = Boolean(busy && liveCapture);
    $("pause-analysis").classList.toggle("hidden", !captureActive);
    $("pause-analysis").disabled = !captureActive;
    if (!captureActive) {
      analysisPaused = false;
      $("pause-analysis").setAttribute("aria-pressed", "false");
      $("pause-analysis").textContent = "Pause analysis";
    }
    if (captureActive) {
      recordingSeconds = 0;
      $("recording-timer").textContent = "00:00.0";
    }
    $("transcribe-button").disabled = busy;
    $("record-button").disabled =
      busy || !window.transcribeConfig?.asio || !asioDevices.length;
    $("accept-proposal").disabled = busy || !hasPendingProposal;
    $("cancel-button").classList.toggle("hidden", !busy || (!cancellable && !captureActive));
    $("transcribe-button").querySelector("span:last-child").textContent =
      busy ? (captureActive ? "Recording..." : "Transcribing...")
        : (mode === "asio" ? "Start capture" : "Transcribe");
    document.querySelectorAll(".browse-button, .segment").forEach((button) => {
      if (busy) button.disabled = true;
      else if (button.dataset.field === "model") button.disabled = !window.transcribeConfig?.onnx;
      else if (button.dataset.mode === "asio") button.disabled = !window.transcribeConfig?.asio;
      else button.disabled = false;
    });
    Object.entries(inputs).forEach(([key, input]) => {
      input.disabled = busy || (key === "model" && !window.transcribeConfig?.onnx);
    });
    $("capture-seconds").disabled = busy;
    $("latency-ms").disabled = busy;
    $("live-hop-ms").disabled = busy || mode !== "asio" ||
      $("live-backend").value === "yin";
    $("asio-device").disabled = busy || !asioDevices.length;
    $("asio-sample-rate").disabled = busy || !asioDevices.length;
    $("asio-channels").disabled = busy || !asioDevices.length;
    ["tempo-auto", "meter-control", "key-control", "minor-control",
      "pickup-control", "written-transposition-control", "quantization-mode",
      "quantization-strength", "rhythm-evidence"].forEach((id) => {
      $(id).disabled = busy;
    });
    updateTempoControlState();
    updateReviewModeState();
    $("guitar-tuning").disabled = busy;
    $("capo-fret").disabled = busy;
    $("tab-alternatives").disabled = busy;
    $("preserve-timing").disabled = busy;
    updateAnalysisControlState();
    $("polyphonic").disabled = busy || !window.transcribeConfig?.onnx;
    $("open-folder-button").disabled = busy;
    $("status-output-folder").disabled = busy;
    $("drop-browse").disabled = busy;
    $("cancel-button").textContent = captureActive
      ? (liveCaptureState === "finishing" ? "Cancel capture" : "Stop capture")
      : "Cancel analysis";
    $("cancel-button").disabled = !busy || (!cancellable && !captureActive) ||
      cancelPending || (captureActive && stopRequested &&
        liveCaptureState !== "finishing") ||
      (captureActive && liveCaptureState === "finished");
    $("refine-live").disabled = busy || !hasLiveRecording;
    $("apply-refinement").disabled = busy || !hasPendingRefinement;
    $("discard-refinement").disabled = busy || !hasPendingRefinement;
    $("undo-refinement").disabled = busy || hasUnexportedEdits;
    ["new-project", "open-project", "save-project", "save-score-copy",
      "save-portable-project", "relink-project-audio",
      "project-summary-button"].forEach((id) => {
      $(id).disabled = busy;
    });
    window.dispatchEvent(new CustomEvent("transcribe:busy", {
      detail: { busy, preserveScore, liveCapture: captureActive },
    }));
    updateJourney();
  }

  function selectedAsioDevice() {
    return asioDevices.find((device) =>
      String(device.id) === $("asio-device").value) || null;
  }

  function populateCaptureFormats(device, restore = false) {
    const rates = Array.isArray(device?.sample_rates)
      ? [...new Set(device.sample_rates.map(Number).filter((rate) =>
        Number.isInteger(rate) && rate >= 2000))].sort((a, b) => a - b) : [];
    const rateSelect = $("asio-sample-rate");
    rateSelect.replaceChildren();
    rates.forEach((rate) => {
      const option = document.createElement("option");
      option.value = String(rate);
      option.textContent = `${rate.toLocaleString()} Hz`;
      rateSelect.append(option);
    });
    const preferred = Number(device?.preferred_sample_rate);
    const wantedRate = restore && rates.includes(Number(savedSampleRate))
      ? Number(savedSampleRate)
      : rates.includes(preferred) ? preferred
        : rates.includes(44100) ? 44100 : rates[0];
    if (wantedRate) rateSelect.value = String(wantedRate);

    const channelSelect = $("asio-channels");
    channelSelect.replaceChildren();
    const maximum = Math.min(32, Number(device?.channels) || 0);
    for (let channels = 1; channels <= maximum; channels += 1) {
      const option = document.createElement("option");
      option.value = String(channels);
      option.textContent = channels === 1 ? "1 · mono" : `${channels} · input`;
      channelSelect.append(option);
    }
    const wantedChannels = restore && Number(savedChannels) >= 1 &&
      Number(savedChannels) <= maximum ? Number(savedChannels) : 1;
    if (wantedChannels) channelSelect.value = String(wantedChannels);

    const valid = Boolean(device && rates.length && maximum);
    $("asio-device").disabled = !asioDevices.length || captureActive;
    rateSelect.disabled = !valid || captureActive;
    channelSelect.disabled = !valid || captureActive;
    $("record-button").disabled = !valid || captureActive ||
      !window.transcribeConfig?.asio;
    $("empty-record").disabled = !valid || !window.transcribeConfig?.asio;
    $("asio-mode").disabled = !valid || !window.transcribeConfig?.asio;
    $("input-device-label").textContent = valid
      ? `${device.name} · ${maximum} input channel${maximum === 1 ? "" : "s"}`
      : "No compatible ASIO input device is available.";
    savePaths();
  }

  function acceptAsioDevices(devices) {
    asioDevices = Array.isArray(devices) ? devices : [];
    const select = $("asio-device");
    select.replaceChildren();
    if (!asioDevices.length) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "No ASIO input devices found";
      select.append(option);
      populateCaptureFormats(null);
      return;
    }
    asioDevices.forEach((device) => {
      const option = document.createElement("option");
      option.value = String(device.id);
      option.textContent = `${device.name}${device.default ? " · default" : ""}`;
      select.append(option);
    });
    const saved = asioDevices.find((device) =>
      String(device.id) === savedAudioDeviceId);
    const selected = saved || asioDevices.find((device) => device.default) ||
      asioDevices[0];
    select.value = String(selected.id);
    populateCaptureFormats(selected, Boolean(saved));
  }

  function loadSavedPaths() {
    try {
      const saved = JSON.parse(localStorage.getItem("transcribe.paths") || "{}");
      Object.entries(inputs).forEach(([key, input]) => {
        if (saved[key]) input.value = saved[key];
      });
      if (saved.mode) selectMode(saved.mode);
      $("polyphonic").checked = Boolean(saved.polyphonic);
      if (saved.latencyMs) $("latency-ms").value = saved.latencyMs;
      if (saved.liveHopMs !== undefined) $("live-hop-ms").value = saved.liveHopMs;
      if (saved.tuning) $("guitar-tuning").value = saved.tuning;
      if (saved.capoFret !== undefined) $("capo-fret").value = saved.capoFret;
      if (saved.alternatives !== undefined) $("tab-alternatives").value = saved.alternatives;
      $("preserve-timing").checked = Boolean(saved.preserveTiming);
      if (saved.quantization) $("quantization-mode").value = saved.quantization;
      if (saved.quantizationStrength !== undefined) {
        $("quantization-strength").value = saved.quantizationStrength;
      }
      if (saved.rhythmEvidence !== undefined) $("rhythm-evidence").value = saved.rhythmEvidence;
      $("review-mode").checked = Boolean(saved.reviewMode);
      $("bounded-inference").checked = Boolean(saved.boundedInference);
      if (saved.instrument) $("instrument-prior").value = saved.instrument;
      if (saved.quality) $("quality-mode").value = saved.quality;
      if (saved.tempo) $("tempo-control").value = saved.tempo;
      $("tempo-auto").checked = saved.tempoAuto !== false;
      if (saved.meter) $("meter-control").value = saved.meter;
      if (saved.key !== undefined) $("key-control").value = saved.key;
      $("minor-control").checked = Boolean(saved.minor);
      if (saved.pickup !== undefined) $("pickup-control").value = saved.pickup;
      if (saved.transposition !== undefined) {
        $("written-transposition-control").value = saved.transposition;
      }
      savedAudioDeviceId = saved.asioDeviceId || "";
      savedSampleRate = saved.sampleRate || "";
      savedChannels = saved.channels || "";
      if (saved.liveBackend) $("live-backend").value = saved.liveBackend;
      updateAnalysisControlState();
      updateTempoControlState();
      updateModelStatus();
    } catch (error) {
      console.warn("Could not restore saved Transcriber paths.", error);
      $("status-text").textContent = "Saved paths could not be restored";
    }
  }

  function savePaths() {
    updateJourney();
    localStorage.setItem("transcribe.paths", JSON.stringify({
      input: inputs.input.value,
      model: inputs.model.value,
      musicxml: inputs.musicxml.value,
      tab: inputs.tab.value,
      midi: inputs.midi.value,
      json: inputs.json.value,
      mode,
      polyphonic: $("polyphonic").checked,
      latencyMs: $("latency-ms").value,
      liveHopMs: $("live-hop-ms").value,
      tuning: $("guitar-tuning").value,
      capoFret: $("capo-fret").value,
      alternatives: $("tab-alternatives").value,
      preserveTiming: $("preserve-timing").checked,
      quantization: $("quantization-mode").value,
      quantizationStrength: $("quantization-strength").value,
      rhythmEvidence: $("rhythm-evidence").value,
      reviewMode: $("review-mode").checked,
      boundedInference: $("bounded-inference").checked,
      instrument: $("instrument-prior").value,
      quality: $("quality-mode").value,
      tempo: $("tempo-control").value,
      tempoAuto: $("tempo-auto").checked,
      meter: $("meter-control").value,
      key: $("key-control").value,
      minor: $("minor-control").checked,
      pickup: $("pickup-control").value,
      transposition: $("written-transposition-control").value,
      asioDeviceId: $("asio-device").value,
      sampleRate: $("asio-sample-rate").value,
      channels: $("asio-channels").value,
      liveBackend: $("live-backend").value,
    }));
  }

  function selectMode(nextMode) {
    mode = nextMode;
    document.querySelectorAll(".segment").forEach((button) => {
      button.classList.toggle("selected", button.dataset.mode === mode);
    });
    $("file-source").classList.toggle("hidden", mode !== "file");
    $("asio-source").classList.toggle("hidden", mode !== "asio");
    $("transcribe-button").querySelector("span:last-child").textContent =
      mode === "asio" ? "Start capture" : "Transcribe";
    $("transcribe-button").title = mode === "asio"
      ? "Start capture from the selected ASIO input"
      : "Transcribe the selected audio file";
    $("record-button").classList.toggle("active", mode === "asio");
    updateProjectTitle();
    updateModelStatus();
    updateReviewModeState();
    updateAnalysisControlState();
    savePaths();
  }

  function renderNoteList() {
    const list = $("note-list");
    const visible = notes.slice(-80).map((note, offset) => ({
      note,
      index: notes.length - Math.min(notes.length, 80) + offset,
    })).reverse();
    const wanted = new Set(visible.map(({ note, index }) =>
      note.id || `${note.midi}:${note.start}:${index}`));
    list.querySelectorAll(".note-row").forEach((row) => {
      if (!wanted.has(row.dataset.key)) row.remove();
    });
    if (!visible.length) {
      list.replaceChildren(Object.assign(document.createElement("div"), {
        className: "empty-notes",
        textContent: "No note events yet.",
      }));
      return;
    }
    list.querySelector(".empty-notes")?.remove();
    const rows = new Map(Array.from(list.querySelectorAll(".note-row"),
      (row) => [row.dataset.key, row]));
    visible.forEach(({ note, index }) => {
      const key = note.id || `${note.midi}:${note.start}:${index}`;
      let row = rows.get(key);
      if (!row) {
        row = document.createElement("button");
        row.type = "button";
        row.className = "note-row";
        row.innerHTML =
          '<span class="note-main"><i class="note-color" aria-hidden="true"></i><span class="note-name"></span></span><span class="note-time"></span><span class="note-confidence"></span>';
        list.appendChild(row);
      }
      row.dataset.key = key;
      row.dataset.index = String(index);
      const visualState = note.live_visual_state ||
        (note.finalized ? "finalized" : note.state || "");
      row.dataset.liveState = visualState;
      row.setAttribute("aria-label",
        `${note.name}, onset ${Number(note.start).toFixed(2)} seconds` +
        (visualState ? `, ${visualState.replaceAll("_", " ")}` : ""));
      row.querySelector(".note-color").style.background =
        note.user_authored ? "#e9c578" : note.uncertain ? "#ff9b8e"
          : note.state === "provisional" ? "#96b5ff"
            : note.state === "stabilized" ? "#74e4bd"
              : colors[(note.midi % colors.length + colors.length) % colors.length];
      row.querySelector(".note-name").textContent = note.name;
      row.querySelector(".note-time").textContent =
        `${Number(note.start).toFixed(2)}s · ${Number(note.duration).toFixed(2)}s`;
      row.querySelector(".note-confidence").textContent = note.uncertain
        ? "UNCERTAIN"
        : visualState === "retracted" ? "RETRACTED"
          : visualState === "corrected" ? "CORRECTED"
          : visualState === "provisional" ? "PROVISIONAL"
          : visualState === "stabilized" ? "STABILIZED"
          : visualState === "finalized" ? "FINALIZED"
        : note.finalized ? "FINALIZED"
          : note.state ? String(note.state).replaceAll("_", " ").toUpperCase()
          : Number.isFinite(note.calibrated_confidence)
            ? `${Math.round(Math.max(0, Math.min(1, note.calibrated_confidence)) * 100)}% calibrated`
          : Number.isFinite(note.confidence)
            ? `score ${note.confidence.toFixed(2)}`
            : "—";
    });
  }

  function renderNotes(nextNotes, final = false, notifyEditor = true) {
    if (!final && notifyEditor && Array.isArray(nextNotes) && nextNotes.length === notes.length &&
        nextNotes.every((note, index) => {
          const previous = notes[index];
          return ["id", "revision", "midi", "start", "duration", "state", "finalized",
            "user_authored", "uncertain", "string", "fret", "calibrated_confidence",
            "live_visual_state", "live_visual_opacity"]
            .every(key => note[key] === previous[key]);
        })) return;
    notes = Array.isArray(nextNotes) ? nextNotes : [];
    rollIndex = window.TranscribeTimeline.index(notes);
    pitchLow = notes.reduce((low, note) => Math.min(low, note.midi), 127);
    pitchHigh = notes.reduce((high, note) => Math.max(high, note.midi), 0);
    duration = Math.max(1, notes.reduce((end, note) =>
      Math.max(end, note.start + note.duration), 0), timelineDuration);
    $("note-count").textContent = notes.length ? String(notes.length) : "—";
    $("empty-roll").classList.toggle("hidden", notes.length > 0);
    renderNoteList();
    updateJourney();
    if (final) $("preview-badge").textContent = "COMPLETE";
    drawRoll();
    if (notifyEditor) {
      window.dispatchEvent(new CustomEvent("transcribe:notes", { detail: notes }));
    }
  }

  function drawRoll() {
    const canvas = $("piano-roll");
    canvas.dataset.paintCount = String(Number(canvas.dataset.paintCount || 0) + 1);
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const scale = window.devicePixelRatio || 1;
    const backingWidth = Math.round(rect.width * scale);
    const backingHeight = Math.round(rect.height * scale);
    if (canvas.width !== backingWidth) canvas.width = backingWidth;
    if (canvas.height !== backingHeight) canvas.height = backingHeight;
    const context = canvas.getContext("2d");
    context.setTransform(scale, 0, 0, scale, 0, 0);
    const width = rect.width;
    const height = rect.height;
    context.fillStyle = getComputedStyle(document.body).getPropertyValue("--field");
    context.fillRect(0, 0, width, height);
    const view = window.TranscribeTimeline.bounds(duration);
    const visible = rollIndex(duration);
    const visibleGaps = liveGapRegions.filter((gap) =>
      gap.end_seconds > view.start && gap.start_seconds < view.end);
    canvas.dataset.gapRegions = String(visibleGaps.length);
    for (const gap of visibleGaps) {
      const left = Math.max(36, window.TranscribeTimeline.x(
        Math.max(view.start, gap.start_seconds), duration, width));
      const right = Math.min(width - 10, window.TranscribeTimeline.x(
        Math.min(view.end, gap.end_seconds), duration, width));
      if (right <= left) continue;
      context.save();
      context.beginPath();
      context.rect(left, 0, right - left, height);
      context.clip();
      context.fillStyle = "rgba(233, 197, 120, .12)";
      context.fillRect(left, 0, right - left, height);
      context.strokeStyle = "rgba(233, 197, 120, .5)";
      for (let x = left - height; x < right; x += 9) {
        context.beginPath();
        context.moveTo(x, height);
        context.lineTo(x + height, 0);
        context.stroke();
      }
      context.fillStyle = getComputedStyle(document.body).getPropertyValue("--text");
      context.font = 'bold 9px "DM Mono", Consolas, monospace';
      context.fillText("UNANALYSED", left + 4, 12);
      context.restore();
    }
    if (!notes.length) {
      canvas.dataset.visibleNotes = "0";
      canvas.dataset.paintedNotes = "0";
      canvas.dataset.provisionalNotes = "0";
      canvas.dataset.stabilizedNotes = "0";
      canvas.dataset.finalizedNotes = "0";
      canvas.dataset.retractedNotes = "0";
      canvas.dataset.correctedNotes = "0";
      return;
    }
    const low = pitchLow;
    const high = pitchHigh;
    const range = Math.max(12, high - low + 2);
    const minPitch = low - Math.max(0, (range - (high - low + 2)) / 2);
    const pitchHeight = height / range;
    for (let midi = Math.floor(minPitch); midi <= Math.ceil(minPitch + range); midi += 1) {
      const y = height - ((midi - minPitch) / range) * height;
      context.strokeStyle = midi % 12 === 0 ? "rgba(113,142,146,.18)" : "rgba(113,142,146,.07)";
      context.lineWidth = 1;
      context.beginPath();
      context.moveTo(0, Math.round(y) + .5);
      context.lineTo(width, Math.round(y) + .5);
      context.stroke();
    }
    for (let tick = 0; tick <= 4; tick += 1) {
      const x = tick * width / 4;
      context.strokeStyle = "rgba(113,142,146,.09)";
      context.beginPath();
      context.moveTo(Math.round(x) + .5, 0);
      context.lineTo(Math.round(x) + .5, height);
      context.stroke();
    }
    const stride = Math.max(1, Math.ceil(visible.length / 4000));
    let painted = 0;
    const visualCounts = { provisional: 0, stabilized: 0, finalized: 0,
      retracted: 0, corrected: 0 };
    visible.forEach(({ note }, index) => {
      const visualState = note.live_visual_state ||
        (note.finalized ? "finalized" : note.state || "");
      if (Object.hasOwn(visualCounts, visualState)) visualCounts[visualState] += 1;
      if (index % stride && note.id !== selectedNoteId) return;
      painted += 1;
      const x = window.TranscribeTimeline.x(note.start, duration, width);
      const w = Math.max(2, window.TranscribeTimeline.x(note.start + note.duration, duration, width) - x);
      const y = height - ((note.midi + .5 - minPitch) / range) * height;
      const opacity = Number.isFinite(note.live_visual_opacity)
        ? Math.max(0, Math.min(1, note.live_visual_opacity)) : 1;
      const alpha = opacity * (visualState === "provisional" ? .62 : .9);
      const color = note.user_authored ? "#c39b35" : note.uncertain ? "#e27467"
        : visualState === "provisional" ? "#819ee5" : visualState === "finalized" ? "#32ae8c" : "#8b83d0";
      context.globalAlpha = alpha;
      context.fillStyle = color;
      context.beginPath();
      const radius = Math.min(3, w / 2, pitchHeight / 2);
      context.roundRect(x, y - Math.max(2, pitchHeight * .33), w, Math.max(4, pitchHeight * .66), radius);
      if (visualState === "provisional") {
        context.setLineDash([3, 2]);
        context.lineWidth = 1.5;
        context.strokeStyle = color;
        context.stroke();
        context.setLineDash([]);
      } else {
        context.fill();
      }
      if (visualState === "stabilized") {
        context.fillStyle = color;
        context.font = 'bold 8px "DM Mono", Consolas, monospace';
        context.fillText("S", x + 2, y - 2);
      } else if (visualState === "corrected") {
        context.lineWidth = 2;
        context.strokeStyle = "#fff2bd";
        context.stroke();
      } else if (visualState === "retracted") {
        context.beginPath();
        context.moveTo(x, y - Math.max(2, pitchHeight * .33));
        context.lineTo(x + w, y + Math.max(2, pitchHeight * .33));
        context.strokeStyle = "#ff8888";
        context.stroke();
      }
      if (note.id === selectedNoteId) {
        context.strokeStyle = document.body.dataset.theme === "light" ? "#795100" : "#fff2bd";
        context.stroke();
      }
      context.globalAlpha = 1;
    });
    const pitchName = (midi) => `${["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][midi % 12]}${Math.floor(midi / 12) - 1}`;
    $("pitch-high").textContent = pitchName(high);
    $("pitch-low").textContent = pitchName(low);
    $("roll-end").textContent = `${view.end.toFixed(1)}s`;
    document.querySelector(".roll-time span").textContent = `${view.start.toFixed(1)}s`;
    canvas.dataset.visibleNotes = String(visible.length);
    canvas.dataset.paintedNotes = String(painted);
    canvas.dataset.provisionalNotes = String(visualCounts.provisional);
    canvas.dataset.stabilizedNotes = String(visualCounts.stabilized);
    canvas.dataset.finalizedNotes = String(visualCounts.finalized);
    canvas.dataset.retractedNotes = String(visualCounts.retracted);
    canvas.dataset.correctedNotes = String(visualCounts.corrected);
    $("roll-duration").textContent = stride > 1
      ? `${visible.length} notes · sampled overview; zoom to inspect every note` : "Time";
  }

  window.addEventListener("resize", drawRoll);

  function showError(message) {
    setActivity("error", "Action could not be completed", message);
    $("preview-badge").textContent = "ERROR";
    $("preview-badge").className = "preview-badge error";
    $("result-message").textContent = message;
    $("result-message").className = "result-message error";
    setStatus(message, 0);
  }

  function renderRefinement(message) {
    const panel = $("refinement-panel");
    const changes = $("refinement-changes");
    changes.replaceChildren();
    hasPendingRefinement = Boolean(message.success &&
      Array.isArray(message.changes) && message.changes.length);
    if (!message.success) {
      $("refinement-state").textContent = "Failed";
      $("refinement-summary").textContent =
        message.error || "Offline refinement failed.";
      $("apply-refinement").disabled = true;
      $("discard-refinement").disabled = true;
      panel.classList.remove("hidden");
      return;
    }
    const entries = message.changes || [];
    const conflicts = entries.filter((change) => change.conflict).length;
    $("refinement-state").textContent = `${entries.length} proposed changes`;
    $("refinement-summary").textContent =
      `${entries.length} changes from ${message.backend_id || "offline inference"} ` +
      `(${Number(message.inference_seconds || 0).toFixed(2)} s). ` +
      `${conflicts} conflict${conflicts === 1 ? "" : "s"} with manual edits; ` +
      "only explicitly approved conflicts can replace those edits.";
    entries.forEach((change) => {
      const row = document.createElement("label");
      row.className = "refinement-change";
      const description = document.createElement("span");
      const before = change.current?.name || "no note";
      const after = change.proposed?.name || "no note";
      description.textContent =
        `${String(change.kind).toUpperCase()} · ${before} → ${after} · ` +
        `${Number(change.proposed?.start ?? change.current?.start ?? 0).toFixed(2)} s` +
        (change.conflict ? " · conflicts with a manual edit" : "");
      if (change.conflict) {
        const approval = document.createElement("input");
        approval.type = "checkbox";
        approval.dataset.changeId = change.change_id;
        approval.setAttribute("aria-label",
          `Approve refinement conflict: ${description.textContent}`);
        row.append(approval);
      } else {
        const marker = document.createElement("span");
        marker.className = "refinement-automatic";
        marker.textContent = "Included";
        row.append(marker);
      }
      row.append(description);
      changes.append(row);
    });
    panel.classList.remove("hidden");
    $("apply-refinement").disabled = !entries.length;
    $("discard-refinement").disabled = false;
  }

  function notifyReview(message) {
    window.dispatchEvent(new CustomEvent("transcribe:review", {
      detail: {
        enabled: Boolean($("review-mode").checked &&
          document.querySelector(".segment.selected")?.dataset.mode === "file"),
        report: message.review || null,
        evidenceTruncated: Boolean(message.review_evidence_truncated),
      },
    }));
  }

  function updateLiveFollowControls() {
    const available = liveCaptureState !== "idle";
    $("follow-live").disabled = !available;
    $("jump-live").disabled = !available;
    $("jump-live").hidden = !available || $("follow-live").checked;
  }

  function setLiveCaptureState(state) {
    if (!state) return;
    if (liveCaptureState === state) {
      updateLiveFollowControls();
      return;
    }
    liveCaptureState = state;
    updateLiveFollowControls();
    window.dispatchEvent(new CustomEvent("transcribe:live-capture-state", {
      detail: { state },
    }));
  }

  function jumpToLive() {
    const edge = Math.max(1, recordingSeconds, timelineDuration);
    timelineDuration = edge;
    const start = Math.max(0, edge - liveViewportSpan);
    window.TranscribeTimeline.setView(start, liveViewportSpan, edge);
  }

  function scheduleLiveRender(flags = {}, message = null) {
    const previous = queuedLiveMessage || {};
    queuedLiveMessage = {
      ...previous,
      ...(message || {}),
      events: [...(previous.events || []), ...(message?.events || [])],
      coalesced_events_dropped:
        (Number(previous.coalesced_events_dropped) || 0) +
        (Number(message?.coalesced_events_dropped) || 0),
      noteChanges: Boolean(previous.noteChanges || flags.noteChanges),
      overlayChanges: Boolean(previous.overlayChanges || flags.overlayChanges),
      timelineChanged: Boolean(previous.timelineChanged || flags.timelineChanged),
      hasMessage: Boolean(previous.hasMessage || message),
    };
    if (liveFrame) return;
    liveFrame = requestAnimationFrame(() => {
      liveFrame = 0;
      const latest = queuedLiveMessage;
      queuedLiveMessage = null;
      if (!latest) return;

      let noteChanges = latest.noteChanges;
      const now = performance.now();
      for (const [id, fade] of liveRetractions) {
        if (liveNoteRevisions.get(id) !== fade.revision) {
          liveRetractions.delete(id);
          continue;
        }
        const remaining = 1 - (now - fade.started) / 450;
        if (remaining <= 0) {
          liveRetractions.delete(id);
          liveNotesById.delete(id);
          noteChanges = true;
          continue;
        }
        const note = liveNotesById.get(id);
        if (note && Math.abs((note.live_visual_opacity ?? 1) - remaining) > .025) {
          liveNotesById.set(id, { ...note, live_visual_opacity: remaining });
          noteChanges = true;
        }
      }

      applyingLiveBatch = true;
      let timelineChanged = latest.timelineChanged;
      if (Number.isFinite(latest.recording_seconds)) {
        recordingSeconds = Math.max(0, latest.recording_seconds);
        const minutes = Math.floor(recordingSeconds / 60);
        $("recording-timer").textContent =
          `${String(minutes).padStart(2, "0")}:${(recordingSeconds % 60).toFixed(1).padStart(4, "0")}`;
        const nextDuration = Math.max(1, recordingSeconds);
        if (nextDuration !== timelineDuration) {
          timelineDuration = nextDuration;
          timelineChanged = true;
          window.dispatchEvent(new CustomEvent("transcribe:timeline", {
            detail: { duration: timelineDuration },
          }));
        }
      }
      if (latest.status?.state) setLiveCaptureState(latest.status.state);
      if (latest.hasMessage && $("follow-live").checked &&
          ["arming", "capturing", "interrupted", "finishing"].includes(liveCaptureState)) {
        jumpToLive();
      }
      if (latest.overlayChanges) {
        window.dispatchEvent(new CustomEvent("transcribe:live-overlays", {
          detail: { gaps: liveGapRegions },
        }));
      }
      if (noteChanges && !sourceChanged) {
        const nextNotes = [...liveNotesById.values()]
          .sort((left, right) => left.start - right.start || left.midi - right.midi);
        renderNotes(nextNotes);
      } else if (timelineChanged || latest.overlayChanges) {
        drawRoll();
      }
      applyingLiveBatch = false;

      if (latest.hasMessage) {
        const liveEvents = latest.events || [];
        window.dispatchEvent(new CustomEvent("transcribe:live-events", {
          detail: liveEvents,
        }));
        if (liveEvents.length) {
          recentEvents = [...recentEvents, ...liveEvents].slice(-8);
          $("live-events").textContent = recentEvents.map(item => {
            if (item.type === "gap") {
              return `ANALYSIS GAP · ${Number(item.first_frame || 0)}–${Number(item.end_frame || 0)} frames${item.message ? ` · ${item.message}` : ""}`;
            }
            return `${String(item.type || "event").replaceAll("_", " ").toUpperCase()} MIDI ${item.midi} · ${Number(item.time).toFixed(2)}s · ${(Number(item.latency_ms) || 0).toFixed(0)}ms${item.message ? ` · ${item.message}` : ""}`;
          }).join("\n");
        }

        const metrics = latest.metrics || {};
        if (latest.metrics) updateLiveHealth(latest);
        const status = latest.status || {};
        const statusText = metrics.overloaded
          ? "Input overload; inspect live diagnostics"
          : metrics.gap_frames
            ? "Audio gaps detected; inspect live diagnostics"
            : status.message || "Recording; live analysis active";
        if ($("status-text").textContent !== statusText) $("status-text").textContent = statusText;
        $("status-text").title = $("live-diagnostics").textContent;
        const captureLabels = {
          arming: ["Starting capture", "busy"],
          capturing: [analysisPaused ? "Recording · analysis paused" : "Capturing · live inference", "active"],
          interrupted: ["Input interrupted · reconnecting", "error"],
          finishing: ["Finalizing live notes", "busy"],
          finished: ["Capture finished", "ready"],
          failed: ["Capture failed", "error"],
          idle: ["Ready", "ready"],
        };
        const label = captureLabels[status.state];
        if (label) setCaptureState(label[0], label[1]);
        if (status.state === "finishing" && captureActive) {
          $("cancel-button").textContent = "Cancel capture";
          $("cancel-button").disabled = cancelPending;
        } else if (["finished", "failed"].includes(status.state) && captureActive) {
          $("cancel-button").disabled = true;
        }
        $("preview-badge").textContent = status.state === "finished"
          ? "COMPLETE" : status.state === "failed" ? "FAILED" : "LISTENING";
        $("preview-badge").className = status.state === "failed"
          ? "preview-badge" : "preview-badge active";
      }
      if (liveRetractions.size) scheduleLiveRender({ noteChanges: false });
    });
  }

  function receiveLiveEventBatch(message) {
    if (Number(message.version) !== 1 ||
        !Number.isInteger(message.sequence)) return;
    const job = Number(message.job) || 0;
    if (job && job < liveStatusJob) return;
    if (job > liveStatusJob) {
      liveStatusJob = job;
      lastLiveSequence = 0;
      liveNotesById.clear();
      liveNoteRevisions.clear();
      liveRetractions.clear();
      for (const timer of liveCorrectionTimers.values()) clearTimeout(timer);
      liveCorrectionTimers.clear();
      liveGapRegions = [];
      recentEvents = [];
      scheduleLiveRender({ overlayChanges: true });
    }
    if (message.sequence <= lastLiveSequence) return;
    lastLiveSequence = message.sequence;

    const upserts = Object.entries(message.upserts || {}).map(([id, item]) => ({
      id,
      revision: Number(item.revision),
      note: item.note,
    }));
    const finalizations = (message.finalizations || []).map(item => ({
      id: item.id,
      revision: Number(item.revision),
      note: item.note,
      finalized: true,
    }));
    const retractions = (message.retractions || []).map(item => ({
      id: item.id,
      revision: Number(item.revision),
      note: null,
    }));
    const snapshotNotes = [...upserts, ...finalizations];
    if (message.resync) {
      liveNotesById.clear();
      liveNoteRevisions.clear();
      liveRetractions.clear();
      for (const timer of liveCorrectionTimers.values()) clearTimeout(timer);
      liveCorrectionTimers.clear();
      for (const item of snapshotNotes) {
        if (item.id) liveNoteRevisions.delete(item.id);
      }
    }

    let noteChanges = Boolean(message.resync);
    const mutations = [...upserts, ...finalizations, ...retractions]
      .filter(item => item.id && Number.isSafeInteger(item.revision))
      .sort((left, right) => left.revision - right.revision);
    for (const mutation of mutations) {
      const currentRevision = liveNoteRevisions.get(mutation.id) ?? -1;
      if (!message.resync && mutation.revision <= currentRevision) continue;
      liveNoteRevisions.set(mutation.id, mutation.revision);
      noteChanges = true;
      const correctionTimer = liveCorrectionTimers.get(mutation.id);
      if (correctionTimer) clearTimeout(correctionTimer);
      liveCorrectionTimers.delete(mutation.id);
      liveRetractions.delete(mutation.id);
      if (mutation.note) {
        const note = {
          ...mutation.note,
          id: mutation.id,
          revision: mutation.revision,
        };
        delete note.live_visual_state;
        delete note.live_visual_opacity;
        if (mutation.finalized || note.finalized) {
          note.finalized = true;
          note.state = "finalized";
        }
        liveNotesById.set(mutation.id, note);
      } else {
        const previous = liveNotesById.get(mutation.id);
        if (previous) {
          liveNotesById.set(mutation.id, {
            ...previous,
            revision: mutation.revision,
            live_visual_state: "retracted",
            live_visual_opacity: 1,
          });
          liveRetractions.set(mutation.id, {
            revision: mutation.revision,
            started: performance.now(),
          });
        } else {
          liveNotesById.delete(mutation.id);
        }
      }
    }

    const liveEvents = Array.isArray(message.events) ? message.events : [];
    for (const event of liveEvents) {
      if (event.type !== "corrected" || !event.id) continue;
      const note = liveNotesById.get(event.id);
      if (!note || note.live_visual_state === "retracted") continue;
      const revision = liveNoteRevisions.get(event.id);
      liveNotesById.set(event.id, { ...note, live_visual_state: "corrected" });
      noteChanges = true;
      const previousTimer = liveCorrectionTimers.get(event.id);
      if (previousTimer) clearTimeout(previousTimer);
      liveCorrectionTimers.set(event.id, setTimeout(() => {
        liveCorrectionTimers.delete(event.id);
        const current = liveNotesById.get(event.id);
        if (liveNoteRevisions.get(event.id) !== revision ||
            current?.live_visual_state !== "corrected") return;
        const ordinary = { ...current };
        delete ordinary.live_visual_state;
        liveNotesById.set(event.id, ordinary);
        scheduleLiveRender({ noteChanges: true });
      }, 900));
    }

    const nextGaps = [];
    const gapKeys = new Set(liveGapRegions.map(gap =>
      `${gap.first_frame}:${gap.end_frame}`));
    const sampleRate = Number(message.sample_rate);
    for (const gap of Array.isArray(message.gaps) ? message.gaps : []) {
      const firstFrame = Number(gap.first_frame);
      const endFrame = Number(gap.end_frame);
      const start = Number.isFinite(gap.start_seconds) ? Number(gap.start_seconds)
        : Number.isInteger(sampleRate) && sampleRate > 0 ? firstFrame / sampleRate : NaN;
      const end = Number.isFinite(gap.end_seconds) ? Number(gap.end_seconds)
        : Number.isInteger(sampleRate) && sampleRate > 0 ? endFrame / sampleRate : NaN;
      if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) continue;
      const key = `${firstFrame}:${endFrame}`;
      if (gapKeys.has(key)) continue;
      gapKeys.add(key);
      nextGaps.push({
        first_frame: firstFrame,
        end_frame: endFrame,
        start_seconds: start,
        end_seconds: end,
        message: gap.message || "",
      });
    }
    if (nextGaps.length) {
      liveGapRegions = [...liveGapRegions, ...nextGaps].slice(-64);
    }

    const fingerings = message.fingerings || {};
    for (const [id, note] of liveNotesById) {
      const position = fingerings[id];
      if (position) {
        if (note.string !== position.string || note.fret !== position.fret ||
            !note.manual_fingering) noteChanges = true;
        liveNotesById.set(id, {
          ...note,
          string: position.string,
          fret: position.fret,
          manual_fingering: true,
        });
      } else if (note.manual_fingering) {
        const withoutFingering = { ...note };
        delete withoutFingering.string;
        delete withoutFingering.fret;
        delete withoutFingering.manual_fingering;
        liveNotesById.set(id, withoutFingering);
        noteChanges = true;
      }
    }

    scheduleLiveRender({
      noteChanges,
      overlayChanges: nextGaps.length > 0 || Boolean(message.resync),
      timelineChanged: Number.isFinite(message.recording_seconds),
    }, message);
  }

  function handleMessage(event) {
    const message = event.data || {};
    if (message.type === "live-status") {
      if (Number(message.version) !== 1) return;
      const job = Number(message.job) || 0;
      if (job && job < liveStatusJob) return;
      if (job > liveStatusJob) {
        liveStatusJob = job;
        lastLiveSequence = 0;
        liveNotesById.clear();
        liveNoteRevisions.clear();
        liveRetractions.clear();
        liveGapRegions = [];
        recentEvents = [];
        scheduleLiveRender({ noteChanges: true, overlayChanges: true });
      }
      setLiveCaptureState(message.state || "failed");
      if (liveCaptureState === "finishing" && captureActive) {
        $("cancel-button").textContent = "Cancel capture";
        $("cancel-button").disabled = cancelPending;
      } else if (["finished", "failed"].includes(liveCaptureState) && captureActive) {
        $("cancel-button").disabled = true;
      }
      const statusLabels = {
        idle: ["Ready", "ready"],
        arming: ["Starting capture", "busy"],
        capturing: [analysisPaused ? "Recording · analysis paused" : "Capturing · live inference", "active"],
        interrupted: ["Input interrupted · reconnecting", "error"],
        finishing: ["Finalizing live notes", "busy"],
        finished: ["Capture finished", "ready"],
        failed: ["Capture failed", "error"],
      };
      const label = statusLabels[liveCaptureState];
      if (label) setCaptureState(label[0], label[1]);
      if (message.status) {
        $("status-text").textContent = message.status;
        $("status-text").title = message.status;
      }
      return;
    }
    if (message.type === "live-event-batch") {
      if (message.job && Number(message.job) < liveStatusJob) return;
      receiveLiveEventBatch(message);
      return;
    }
    if (message.type === "analysis-paused") {
      analysisPaused = Boolean(message.paused);
      $("pause-analysis").disabled = !captureActive;
      $("pause-analysis").setAttribute("aria-pressed", String(analysisPaused));
      $("pause-analysis").textContent = analysisPaused ? "Resume analysis" : "Pause analysis";
      setCaptureState(analysisPaused ? "Recording · analysis paused" : "Capturing · live inference", "active");
      return;
    }
    if (message.type === "asio-devices") {
      acceptAsioDevices(message.devices);
      return;
    }
    if (message.type === "ready") {
      window.transcribeConfig = message;
      window.dispatchEvent(new CustomEvent("transcribe:config", { detail: message }));
      if (!message.asio && mode === "asio") selectMode("file");
      $("asio-mode").disabled = !message.asio;
      document.querySelectorAll('[data-field="model"]').forEach((button) => button.disabled = !message.onnx);
      $("model-path").disabled = !message.onnx;
      $("polyphonic").disabled = !message.onnx;
      $("live-backend").querySelector('option[value="onnx"]').disabled = !message.onnx;
      if (!message.onnx && $("live-backend").value === "onnx") {
        $("live-backend").value = "yin";
      }
      if (message.ffmpeg) $("drop-zone").classList.add("can-drop");
      $("engine-label").textContent = message.asio ? "C++ · YIN · ASIO ready" : "C++ analysis engine";
      $("record-button").disabled = true;
      $("empty-record").disabled = true;
      $("asio-mode").title = message.asio
        ? "Record from a selected ASIO input" : "ASIO capture is unavailable in this build";
      updateAnalysisControlState();
      updateModelStatus();
      return;
    }
    if (message.type === "field") {
      if (message.field === "project-relink") {
        if (message.value) window.dispatchEvent(new CustomEvent("transcribe:project-relink", {
          detail: { path: message.value, generation: ++projectEditGeneration },
        }));
        return;
      }
      if (message.field === "project-open") {
        if (message.value) send("open-project", { path: message.value });
        return;
      }
      if (message.field === "project-save" ||
          message.field === "project-copy" || message.field === "project-portable") {
        const scoreOnly = message.field === "project-copy" ||
          pendingSaveAsScoreOnly;
        pendingSaveAsScoreOnly = false;
        if (!message.value) return;
        window.dispatchEvent(new CustomEvent(
          scoreOnly ? "transcribe:project-copy" : "transcribe:project-save",
          { detail: { path: message.value, scoreOnly, copyAudio: message.field === "project-portable" } },
        ));
        return;
      }
      const input = inputs[message.field];
      if (input) {
        input.value = message.value;
        if (message.field === "input" || message.field === "isolated") {
          $("isolation-reviewed").checked = false;
        }
        savePaths();
        if (message.field === "model") {
          updateAnalysisControlState();
          updateModelStatus();
        } else if (message.field === "input") {
          updateProjectTitle();
          $("preview-badge").textContent = "READY TO TRANSCRIBE";
          $("preview-badge").className = "preview-badge";
          setCaptureState("Audio selected");
          setStatus("Audio file selected; choose Transcribe to analyze it", 0);
        }
      }
      return;
    }
    if (message.type === "recovery-available") {
      if (window.confirm(
        "A recoverable project autosave was found. Open it now? Choosing No discards that recovery copy."
      )) {
        send("open-project", { path: message.path });
      } else {
        send("discard-recovery");
      }
      return;
    }
    if (message.type === "recovery-discarded") {
      setStatus("Autosave recovery copy discarded", 0);
      return;
    }
    if (message.type === "project-autosaving") {
      window.dispatchEvent(new CustomEvent("transcribe:project-autosaving", {
        detail: message,
      }));
      $("save-state").title = "Writing a recovery copy. Unsaved edits still require Save.";
      return;
    }
    if (message.type === "project-relinked") {
      acceptProjectInfo(message.project);
      projectDirty = true;
      hasUnexportedEdits = true;
      $("isolation-reviewed").checked = false;
      inputs.isolated.value = "";
      updateProjectTitle();
      setStatus("Recording relocated. Score edits preserved; Save to keep the new reference. Non-WAV timing identity must be checked manually.", 100);
      return;
    }
    if (message.type === "project-autosaved") {
      $("save-state").title = "Recovery copy saved. This does not replace an explicit project Save.";
      return;
    }
    if (message.type === "project-autosave-error") {
      showError(`Project autosave failed: ${message.message || "Unknown error"}`);
      return;
    }
    if (message.type === "project-reset") {
      clearQueuedPreviews();
      window.clearTimeout(autosaveTimer);
      autosaveTimer = 0;
      hasAcceptedScore = true;
      hasPendingProposal = false;
      hasLiveRecording = false;
      hasPendingRefinement = false;
      hasUnexportedEdits = false;
      projectDirty = false;
      captureActive = false;
      analysisPaused = false;
      stopRequested = false;
      cancelPending = false;
      sourceChanged = true;
      timelineDuration = 1;
      recordingSeconds = 0;
      liveViewportSpan = 12;
      lastLiveSequence = 0;
      liveStatusJob = 0;
      acceptedNotesBeforeLiveCapture = null;
      liveNotesById.clear();
      liveNoteRevisions.clear();
      liveRetractions.clear();
      liveGapRegions = [];
      recentEvents = [];
      acceptProjectInfo(message.project, true);
      $("isolation-enabled").checked = false;
      $("isolation-reviewed").checked = false;
      $("preprocessing-fallback").checked = false;
      inputs.isolated.value = "";
      $("follow-live").checked = true;
      $("recording-timer").textContent = "00:00.0";
      $("live-events").textContent = "";
      $("live-health-strip").hidden = true;
      $("pause-analysis").classList.add("hidden");
      $("pause-analysis").setAttribute("aria-pressed", "false");
      $("proposal-details").className = "result-message hidden";
      $("proposal-summary").textContent = "";
      $("accept-proposal").hidden = true;
      $("accept-proposal").disabled = true;
      $("refinement-panel").classList.add("hidden");
      $("undo-refinement").hidden = true;
      $("preview-badge").textContent = "EMPTY PROJECT";
      $("preview-badge").className = "preview-badge";
      $("tempo-value").textContent = "—";
      $("key-value").textContent = "—";
      $("result-message").textContent = "New project created.";
      $("result-message").className = "result-message";
      inputPeak = 0;
      updateInputLevel([], true);
      setLiveCaptureState("idle");
      renderNotes(Array.isArray(message.notes) ? message.notes : [], true);
      window.dispatchEvent(new CustomEvent("transcribe:fingering", {
        detail: message.fingering,
      }));
      window.dispatchEvent(new CustomEvent("transcribe:metadata", {
        detail: message.metadata,
      }));
      window.dispatchEvent(new CustomEvent("transcribe:accepted-score", {
        detail: message,
      }));
      window.TranscribeTimeline.setView(0, null, 1);
      window.dispatchEvent(new CustomEvent("transcribe:source-changed"));
      window.dispatchEvent(new CustomEvent("transcribe:live-events", {
        detail: [],
      }));
      window.dispatchEvent(new CustomEvent("transcribe:live-overlays", {
        detail: { gaps: [] },
      }));
      notifyReview({});
      setActivity("empty", "Ready for a recording",
        "Open audio, choose your setup, then select Transcribe.");
      setCaptureState("Empty project");
      setStatus("New project created", 100);
      projectEditGeneration += 1;
      window.dispatchEvent(new CustomEvent("transcribe:project-autosave", {
        detail: { generation: projectEditGeneration },
      }));
      return;
    }
    if (message.type === "progress") {
      if (!cancelPending) setActivity("processing", "Processing", message.status || "Analyzing audio");
      const capture = (message.status || "").toLowerCase().includes("captur");
      if ((message.status || "").toLowerCase().includes("opening project")) {
        setBusy(true, true, false);
      }
      setCaptureState(capture ? "Capturing" : "Processing", capture ? "active" : "busy");
      $("preview-badge").textContent = "ANALYZING";
      $("preview-badge").className = "preview-badge active";
      setStatus(message.status || "Analyzing audio", cancelPending ? undefined : message.progress);
      return;
    }
    if (message.type === "audio-preview") {
      if (sourceChanged) return;
      updateInputLevel(message.waveform, message.replace);
      window.dispatchEvent(new CustomEvent("transcribe:audio-preview", { detail: message }));
      return;
    }
    if (message.type === "live") {
      if (hasAcceptedScore || sourceChanged) return;
      const events = [...(queuedLiveMessage?.events || []), ...(message.events || [])];
      queuedLiveMessage = { ...message, events };
      if (!liveFrame) {
        liveFrame = requestAnimationFrame(() => {
          liveFrame = 0;
          const latest = queuedLiveMessage;
          queuedLiveMessage = null;
          if (!latest) return;
          if (Number.isFinite(latest.recording_seconds)) {
            recordingSeconds = latest.recording_seconds;
            const minutes = Math.floor(recordingSeconds / 60);
            $("recording-timer").textContent =
              `${String(minutes).padStart(2, "0")}:${(recordingSeconds % 60).toFixed(1).padStart(4, "0")}`;
          }
          if (!analysisPaused) renderNotes(latest.notes);
          window.dispatchEvent(new CustomEvent("transcribe:live-events", { detail: latest.events || [] }));
          if (Array.isArray(latest.events) && latest.events.length) {
            recentEvents = [...recentEvents, ...latest.events].slice(-8);
            $("live-events").textContent = recentEvents.map((item) =>
              `${String(item.type || "event").replaceAll("_", " ").toUpperCase()} MIDI ${item.midi} · ${Number(item.time).toFixed(2)}s · ${(Number(item.latency_ms) || 0).toFixed(0)}ms${item.message ? ` · ${item.message}` : ""}`
            ).join("\n");
          }
          if (latest.metrics) {
            updateLiveHealth(latest);
            const status = latest.metrics.overloaded ? "Input overload; inspect live diagnostics"
              : latest.metrics.gap_frames ? "Audio gaps detected; inspect live diagnostics"
              : analysisPaused ? "Recording continues; live analysis paused" : "Recording; live analysis active";
            if ($("status-text").textContent !== status) $("status-text").textContent = status;
            $("status-text").title = $("live-diagnostics").textContent;
          }
          setCaptureState(analysisPaused ? "Recording · analysis paused" :
            mode === "asio" ? "Capturing · live inference" : "Analyzing", "active");
          $("preview-badge").textContent = "LISTENING";
          $("preview-badge").className = "preview-badge active";
        });
      }
      return;
    }
    if (message.type === "refinement-proposal") {
      setBusy(false, true);
      renderRefinement(message);
      if (message.success) {
        setCaptureState("Refinement ready");
        setStatus("Review the offline refinement before applying it", 100);
      } else {
        setCaptureState("Refinement failed", "error");
        setStatus(message.error || "Offline refinement failed.", 0);
      }
      return;
    }
    if (message.type === "refinement-discarded") {
      hasPendingRefinement = false;
      $("refinement-changes").replaceChildren();
      $("refinement-state").textContent = "No proposal";
      $("refinement-summary").textContent = "The proposal was discarded; the accepted score was not changed.";
      $("apply-refinement").disabled = true;
      $("discard-refinement").disabled = true;
      $("refinement-panel").classList.toggle("hidden", !hasLiveRecording);
      return;
    }
    if (message.type === "proposal") {
      setBusy(false);
      if (acceptedNotesBeforeLiveCapture) {
        renderNotes(acceptedNotesBeforeLiveCapture, false);
        acceptedNotesBeforeLiveCapture = null;
      }
      setLiveCaptureState("idle");
      hasPendingProposal = true;
      setActivity("completed", "New proposal ready", "Review the proposal before replacing the accepted score.");
      $("result-message").textContent = message.summary ||
        `A new analysis proposal is available. Accepted score revision ${message.accepted_revision} remains unchanged.`;
      $("result-message").className = "result-message";
      const proposalNotes = Array.isArray(message.notes) ? message.notes : [];
      $("proposal-summary").textContent = proposalNotes.length
        ? `Proposal preview (not applied): ${proposalNotes.slice(0, 12).map((note) =>
          `${note.name} at ${Number(note.start).toFixed(2)}s`).join(" · ")}${proposalNotes.length > 12 ? " · …" : ""}`
        : "The analysis proposal contains no notes.";
      $("proposal-details").className = "result-message";
      $("accept-proposal").hidden = false;
      $("accept-proposal").disabled = !proposalNotes.length;
      setStatus("Proposal ready; accepted score preserved", 100);
      setCaptureState("Proposal ready");
      window.dispatchEvent(new CustomEvent("transcribe:proposal", {
        detail: message,
      }));
      notifyReview(message);
      return;
    }
    if (message.type === "complete") {
      clearQueuedPreviews();
      setBusy(false);
      stopRequested = false;
      $("proposal-details").className = "result-message hidden";
      if (message.success) {
        acceptedNotesBeforeLiveCapture = null;
        setLiveCaptureState("idle");
        hasAcceptedScore = true;
        hasPendingProposal = false;
        hasUnexportedEdits = false;
        hasLiveRecording = Boolean(message.live_capture);
        if (message.project) acceptProjectInfo(message.project);
        $("refinement-panel").classList.toggle("hidden", !hasLiveRecording);
        $("refine-live").disabled = !hasLiveRecording;
        $("undo-refinement").hidden = true;
        $("preview-badge").textContent = "COMPLETE";
        $("preview-badge").className = "preview-badge done";
        $("note-count").textContent = String(message.notes.length);
        $("tempo-value").textContent = message.metadata.tempo_inferred
          ? `${message.metadata.tempo.toFixed(0)} bpm` : "—";
        $("key-value").textContent = message.metadata.key || "—";
        $("result-message").textContent = message.project_opened && message.project
          ? `Opened ${message.project.title}. ${message.project.summary}`
          : message.summary;
        $("result-message").className = "result-message";
        renderNotes(message.notes, true);
        window.dispatchEvent(new CustomEvent("transcribe:fingering", {
          detail: message.fingering,
        }));
        window.dispatchEvent(new CustomEvent("transcribe:metadata", { detail: message.metadata }));
        window.TranscribeTimeline.setView(0, null, duration);
        window.dispatchEvent(new CustomEvent("transcribe:accepted-score", {
          detail: message,
        }));
        if (message.project_warning) {
          $("result-message").textContent += `\n${message.project_warning}`;
        }
        if (message.preprocessing_diagnostic) {
          $("result-message").textContent += `\n${message.preprocessing_diagnostic}`;
        }
        if (message.project_opened) {
          $("isolation-enabled").checked = false;
          $("isolation-reviewed").checked = false;
          inputs.isolated.value = "";
        }
        notifyReview(message);
        setStatus(message.project_opened ? "Project opened" : "Transcription complete", 100);
        setCaptureState(message.project_opened ? "Project ready" : "Score ready");
        setActivity("completed", message.notes.length ? "Score ready to review" : "No notes detected",
          message.notes.length
            ? "Select notes to inspect or edit. Save the project to preserve your work, or export the score."
            : "Try another instrument or supported model, and inspect the recording level. Silence or unsupported material may produce no notes.");
        updateProjectTitle();
      } else {
        if (acceptedNotesBeforeLiveCapture) {
          renderNotes(acceptedNotesBeforeLiveCapture, false);
          acceptedNotesBeforeLiveCapture = null;
        }
        setLiveCaptureState("idle");
        if (!hasAcceptedScore) {
          renderNotes([]);
          window.dispatchEvent(new CustomEvent("transcribe:source-changed"));
        }
        if (message.cancelled) {
          $("preview-badge").textContent = "CANCELED";
          $("preview-badge").className = "preview-badge";
          $("result-message").textContent = message.error || "Analysis canceled. Accepted score unchanged.";
          $("result-message").className = "result-message";
          setStatus("Analysis canceled; accepted score unchanged", 0);
          setCaptureState("Canceled");
          setActivity("canceled", "Analysis canceled",
            "The accepted score was not replaced. Adjust the setup or select Transcribe to try again.");
        } else {
          showError(message.error || "Transcription failed.");
          setCaptureState("Transcription failed", "error");
        }
      }
      return;
    }
    if (message.type === "exported") {
      setBusy(false);
      if (message.success && Array.isArray(message.notes)) {
        hasAcceptedScore = true;
        hasPendingProposal = false;
        hasUnexportedEdits = false;
        renderNotes(message.notes);
        window.dispatchEvent(new CustomEvent("transcribe:fingering", {
          detail: message.fingering,
        }));
        window.dispatchEvent(new CustomEvent("transcribe:metadata", {
          detail: message.metadata,
        }));
        window.dispatchEvent(new CustomEvent("transcribe:accepted-score", {
          detail: message,
        }));
        if (message.review) notifyReview(message);
        if (message.refinement_applied || message.refinement_undo) {
          hasLiveRecording = true;
          $("refinement-panel").classList.remove("hidden");
          $("undo-refinement").hidden = !message.can_undo_refinement;
          $("refinement-state").textContent = message.refinement_undo
            ? "Pre-refinement score restored" : "Refinement applied";
          $("refinement-summary").textContent =
            message.refinement_undo
              ? "The score was restored. Your original live recording remains available."
              : `${Number(message.withheld_conflicts || 0)} conflicting change(s) were withheld. ` +
                "The original live event stream and revision history remain available.";
        } else if (message.can_undo_refinement === false) {
          $("undo-refinement").hidden = true;
        }
      }
      setCaptureState(message.success ? "Export complete" : "Export failed",
        message.success ? "ready" : "error");
      if (!message.success) {
        $("result-message").textContent = message.error || "Score export failed.";
        $("result-message").className = "result-message error";
      }
      if (message.success) $("accept-proposal").hidden = true;
      if (message.success && message.project) {
        if (message.project_saved) {
          acceptProjectInfo(message.project);
        } else {
          updateProjectSummary(message.project);
          projectDirty = true;
          updateProjectTitle();
        }
        if (message.success) {
          setStatus(message.project_saved ? "Project saved" : "Score exported", 100);
          setActivity("completed", message.project_saved ? "Project saved" : "Score exported",
            "Your score is ready. Export files and project saves are separate; a recovery autosave does not replace Save.");
          updateProjectTitle();
        }
      }
      window.dispatchEvent(new CustomEvent("transcribe:edited-export", { detail: message }));
      return;
    }
    if (message.type === "error") {
      window.dispatchEvent(new CustomEvent("transcribe:playback-error"));
      setBusy(false);
      if (acceptedNotesBeforeLiveCapture) {
        renderNotes(acceptedNotesBeforeLiveCapture, false);
        acceptedNotesBeforeLiveCapture = null;
        setLiveCaptureState("idle");
      }
      setCaptureState("Backend error", "error");
      showError(message.message || "The application encountered an error.");
    }
    if (message.type === "playback-state") {
      window.dispatchEvent(new CustomEvent("transcribe:playback-state", { detail: message }));
    }
  }

  document.querySelectorAll(".segment").forEach((button) => {
    button.addEventListener("click", () => selectMode(button.dataset.mode));
  });
  document.querySelectorAll(".browse-button").forEach((button) => {
    button.addEventListener("click", () => {
      const field = button.dataset.field;
      send("browse", { field, current: inputs[field].value });
    });
  });
  $("new-project").addEventListener("click", () => {
    if (confirmDiscardCurrentProject("create a new project")) {
      window.clearTimeout(autosaveTimer);
      autosaveTimer = 0;
      send("new-project");
    }
  });
  $("open-project").addEventListener("click", openProjectPicker);
  $("save-project").addEventListener("click", () => requestSaveProject(false));
  $("save-score-copy").addEventListener("click", () => requestSaveProject(true));
  $("save-portable-project").addEventListener("click", () => {
    if (!activeProjectTitle) return showError("Create or open a project before saving.");
    pendingSaveAsScoreOnly = false;
    send("browse", { field: "project-portable", current: currentProjectPath });
  });
  $("relink-project-audio").addEventListener("click", () =>
    send("browse", { field: "project-relink", current: inputs.input.value }));
  $("project-annotation").addEventListener("input", () => {
    if (!activeProjectTitle) return;
    projectDirty = true;
    updateProjectTitle();
    scheduleProjectAutosave();
  });
  window.addEventListener("transcribe:save-request", () => requestSaveProject(false));
  $("project-summary-button").addEventListener("click", () => {
    $("project-summary-dialog").showModal();
  });
  $("close-project-summary").addEventListener("click", () => {
    $("project-summary-dialog").close();
  });
  $("project-summary-dialog").addEventListener("click", (event) => {
    if (event.target === $("project-summary-dialog")) {
      $("project-summary-dialog").close();
    }
  });
  $("drop-browse").addEventListener("click", () => send("browse", { field: "input", current: inputs.input.value }));
  $("record-button").addEventListener("click", () => {
    if (!window.transcribeConfig?.asio) {
      showError("ASIO capture is unavailable in this build.");
      return;
    }
    selectMode("asio");
    $("transcribe-button").click();
  });
  $("empty-record").addEventListener("click", () => $("record-button").click());
  $("transcribe-button").addEventListener("click", () => {
    if (mode === "file" && !inputs.input.value.trim()) {
      showError("Choose an audio file first.");
      return;
    }
    if (!inputs.musicxml.value.trim() || !inputs.tab.value.trim() ||
        !inputs.json.value.trim()) {
      showError("Choose MusicXML, guitar tab, and score JSON output paths.");
      return;
    }
    if (mode === "asio" && (!(Number($("capture-seconds").value) > 0) || Number($("capture-seconds").value) > 300)) {
      showError("Enter a capture duration greater than 0 and no more than 300 seconds.");
      return;
    }
    if (mode === "asio" &&
        (!(Number($("latency-ms").value) >= 20) || Number($("latency-ms").value) > 50)) {
      showError("Realtime analysis latency must be between 20 and 50 milliseconds.");
      return;
    }
    if (mode === "asio" && $("live-backend").value !== "yin" &&
        (!Number.isFinite(Number($("live-hop-ms").value)) ||
         Number($("live-hop-ms").value) < 10 || Number($("live-hop-ms").value) > 1800)) {
      showError("Windowed live hop interval must be between 10 and 1800 milliseconds.");
      return;
    }
    if (mode === "asio" && !selectedAsioDevice()) {
      showError("Select an available ASIO input device and format.");
      return;
    }
    if (Number($("capo-fret").value) < 0 || Number($("capo-fret").value) > 24 ||
        Number($("tab-alternatives").value) < 0 ||
        Number($("tab-alternatives").value) > 5) {
      showError("Capo must be 0–24 and alternative fingerings must be 0–5.");
      return;
    }
    if (["quantization-strength", "rhythm-evidence"].some((id) => {
      const value = $(id).value;
      return !value.trim() || !Number.isFinite(Number(value)) ||
        Number(value) < 0 || Number(value) > 1;
    })) {
      showError("Quantization strength and minimum rhythm evidence must be between 0 and 1.");
      return;
    }
    if (mode === "file" && $("isolation-enabled").checked &&
        (!inputs.isolated.value.trim() || !$("isolation-reviewed").checked ||
         $("instrument-prior").value === "auto")) {
      showError("Choose an instrument and aligned isolated WAV, then audition and confirm it or disable preprocessing.");
      return;
    }
    recentEvents = [];
    clearQueuedPreviews();
    sourceChanged = false;
    if (!hasAcceptedScore) timelineDuration = 1;
    inputPeak = 0;
    updateInputLevel([], true);
    $("live-events").textContent = "Waiting for realtime note events.";
    $("live-health-strip").hidden = true;
    if (!hasAcceptedScore) {
      renderNotes([]);
    }
    savePaths();
    $("result-message").className = "result-message hidden";
    $("proposal-details").className = "result-message hidden";
    $("accept-proposal").hidden = true;
    hasPendingProposal = false;
    $("preview-badge").textContent = "STARTING";
    $("preview-badge").className = "preview-badge active";
    if (mode === "asio") {
      acceptedNotesBeforeLiveCapture = hasAcceptedScore
        ? notes.map(note => ({ ...note })) : null;
      liveGapRegions = [];
      liveViewportSpan = 12;
      recordingSeconds = 0;
      timelineDuration = 1;
      $("follow-live").checked = true;
      setLiveCaptureState("arming");
      window.dispatchEvent(new CustomEvent("transcribe:timeline", {
        detail: { duration: timelineDuration },
      }));
      jumpToLive();
      liveStatusJob = 0;
      lastLiveSequence = 0;
      liveNotesById.clear();
      liveNoteRevisions.clear();
      liveRetractions.clear();
      for (const timer of liveCorrectionTimers.values()) clearTimeout(timer);
      liveCorrectionTimers.clear();
      stopRequested = false;
      window.dispatchEvent(new CustomEvent("transcribe:live-overlays", {
        detail: { gaps: liveGapRegions },
      }));
    }
    setStatus(mode === "asio" ? "Connecting to ASIO input..." : "Preparing audio...", 0);
    setCaptureState(mode === "asio" ? "Starting capture" : "Loading audio", "busy");
    setBusy(true, hasAcceptedScore, mode === "asio", true);
    send(mode === "asio" ? "start-capture" : "transcribe", {
      mode,
      input: inputs.input.value,
      duration: $("capture-seconds").value,
      isolated: inputs.isolated.value,
      isolation_enabled: mode === "file" && $("isolation-enabled").checked ? "1" : "0",
      isolation_reviewed: $("isolation-reviewed").checked ? "1" : "0",
      preprocessing_fallback: $("preprocessing-fallback").checked ? "1" : "0",
      latency_ms: $("latency-ms").value,
      live_hop_ms: $("live-hop-ms").value,
      device_id: $("asio-device").value,
      sample_rate: $("asio-sample-rate").value,
      channels: $("asio-channels").value,
      live_backend: $("live-backend").value,
      tuning: $("guitar-tuning").value,
      capo: $("capo-fret").value,
      alternatives: $("tab-alternatives").value,
      preserve_timing: $("preserve-timing").checked ? "1" : "0",
      quantization: $("quantization-mode").value,
      quantization_strength: $("quantization-strength").value,
      minimum_rhythm_evidence: $("rhythm-evidence").value,
      instrument: $("instrument-prior").value,
      quality: $("quality-mode").value,
      tempo: $("tempo-auto").checked ? "auto" : $("tempo-control").value,
      meter: $("meter-control").value,
      key_fifths: $("key-control").value,
      minor: $("minor-control").checked ? "1" : "0",
      pickup: $("pickup-control").value,
      written_transposition: $("written-transposition-control").value,
      model: inputs.model.value,
      polyphonic: $("polyphonic").checked ? "1" : "0",
      musicxml: inputs.musicxml.value,
      tab: inputs.tab.value,
      midi: inputs.midi.value,
      json: inputs.json.value,
      review_mode: mode === "file" && $("review-mode").checked ? "1" : "0",
      bounded_inference: mode === "file" && window.transcribeConfig?.onnx &&
        $("bounded-inference").checked ? "1" : "0",
    });
  });
  $("cancel-button").addEventListener("click", () => {
    if (captureActive && liveCaptureState !== "finishing") {
      if (stopRequested) return;
      stopRequested = true;
      $("cancel-button").disabled = true;
      $("cancel-button").textContent = "Stopping capture…";
      setActivity("canceling", "Stopping capture",
        "Finalizing queued notes before saving the recording.");
      send("stop-capture");
      return;
    }
    if (cancelPending) return;
    cancelPending = true;
    $("cancel-button").disabled = true;
    $("cancel-button").textContent = captureActive ? "Canceling capture…" : "Canceling…";
    setActivity("canceling", captureActive ? "Canceling capture" : "Cancel requested",
      "Waiting for the backend to reach a safe processing boundary. The accepted score remains unchanged.");
    setStatus("Waiting for a safe cancellation boundary");
    send("cancel");
  });
  ["input-path", "isolated-path", "instrument-prior"].forEach(id =>
    $(id).addEventListener("change", () => {
      $("isolation-reviewed").checked = false;
      send("stop-playback");
    }));
  ["preview-mixture", "preview-isolated"].forEach(id =>
    $(id).addEventListener("click", () => {
      const start = Number($("isolation-preview-start").value);
      if (!Number.isFinite(start) || start < 0) {
        setStatus("Choose a nonnegative audition position.", 0);
        return;
      }
      send("audition-source", { path: id === "preview-mixture" ? inputs.input.value : inputs.isolated.value,
        start: String(start), end: String(start + 30) });
    }));
  $("stop-isolation-preview").addEventListener("click", () => send("stop-playback"));
  $("accept-proposal").addEventListener("click", () => {
    if (!hasPendingProposal) return;
    if (hasUnexportedEdits && !window.confirm(
      "Replace your unexported edits with this transcription proposal? Undo history for the current score will be discarded."
    )) return;
    $("accept-proposal").disabled = true;
    setBusy(true, true);
    setCaptureState("Applying proposal", "busy");
    setStatus("Applying proposed score and writing exports", 0);
    send("accept-proposal");
  });
  $("refine-live").addEventListener("click", () => {
    if (!hasLiveRecording || captureActive) return;
    setBusy(true, true, false);
    $("refinement-state").textContent = "Analyzing";
    $("refinement-summary").textContent =
      "Running the offline refinement pass. The accepted score is unchanged.";
    $("refinement-panel").classList.remove("hidden");
    setCaptureState("Refining recording", "busy");
    setStatus("Running offline refinement", 0);
    send("refine-live");
  });
  $("apply-refinement").addEventListener("click", () => {
    if (!hasPendingRefinement) return;
    const approved = [...$("refinement-changes")
      .querySelectorAll('input[type="checkbox"]:checked')]
      .map((input) => input.dataset.changeId);
    setBusy(true, true, false);
    setCaptureState("Applying refinement", "busy");
    setStatus("Applying reviewed score changes and writing exports", 0);
    send("apply-refinement", { approved: approved.join(";") });
  });
  $("discard-refinement").addEventListener("click", () => {
    if (!hasPendingRefinement) return;
    send("discard-refinement");
  });
  $("undo-refinement").addEventListener("click", () => {
    if (hasUnexportedEdits) {
      showError("Export or discard your current edits before undoing refinement.");
      return;
    }
    if (window.confirm(
      "Restore the score revision from before offline refinement? This does not remove the recording or its event history."
    )) {
      setBusy(true, true, false);
      setCaptureState("Restoring score", "busy");
      setStatus("Restoring the pre-refinement score", 0);
      send("undo-refinement");
    }
  });
  window.addEventListener("transcribe:edited", (event) => {
    hasUnexportedEdits = hasUnexportedEdits || Boolean(event.detail?.dirty) ||
      (Array.isArray(event.detail?.changes) && event.detail.changes.length > 0);
    $("undo-refinement").disabled = hasUnexportedEdits;
    $("undo-refinement").title = hasUnexportedEdits
      ? "Export or discard edits before undoing refinement"
      : "Restore the score from before offline refinement";
    if (hasAcceptedScore) {
      projectDirty = true;
      updateProjectTitle();
      scheduleProjectAutosave();
    }
    updateProjectTitle();
  });
  ["open-folder-button", "status-output-folder"].forEach((id) =>
    $(id).addEventListener("click", () => send("open-output", { path: inputs.musicxml.value })));
  $("drop-zone").addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      send("browse", { field: "input", current: inputs.input.value });
    }
  });
  inputs.input.addEventListener("keydown", (event) => {
    if (event.key === "Enter") $("transcribe-button").click();
  });
  Object.values(inputs).forEach((input) => input.addEventListener("change", savePaths));
  inputs.input.addEventListener("change", () => {
    updateProjectTitle();
    sourceChanged = true;
    clearQueuedPreviews();
    timelineDuration = 1;
    window.dispatchEvent(new CustomEvent("transcribe:source-changed"));
  });
  inputs.model.addEventListener("input", updateAnalysisControlState);
  inputs.model.addEventListener("input", updateModelStatus);
  $("latency-ms").addEventListener("change", savePaths);
  $("live-hop-ms").addEventListener("change", savePaths);
  $("asio-device").addEventListener("change", () => {
    populateCaptureFormats(selectedAsioDevice());
  });
  ["asio-sample-rate", "asio-channels", "live-backend"].forEach((id) => {
    $(id).addEventListener("change", () => {
      savePaths();
      updateModelStatus();
      updateAnalysisControlState();
    });
  });
  $("guitar-tuning").addEventListener("change", savePaths);
  $("capo-fret").addEventListener("change", savePaths);
  $("tab-alternatives").addEventListener("change", savePaths);
  $("preserve-timing").addEventListener("change", savePaths);
  $("review-mode").addEventListener("change", () => {
    savePaths();
    updateReviewModeState();
  });
  $("pause-analysis").addEventListener("click", () => {
    if (!captureActive) return;
    $("pause-analysis").disabled = true;
    send("pause-analysis", { paused: analysisPaused ? "0" : "1" });
  });
  $("instrument-prior").addEventListener("change", savePaths);
  $("quality-mode").addEventListener("change", savePaths);
  $("tempo-auto").addEventListener("change", () => {
    updateTempoControlState();
    savePaths();
  });
  ["key-control", "minor-control", "pickup-control", "quantization-mode",
    "quantization-strength", "rhythm-evidence",
    "written-transposition-control"].forEach((id) => {
    $(id).addEventListener("change", savePaths);
  });
  $("meter-control").addEventListener("change", savePaths);
  $("tempo-control").addEventListener("change", savePaths);
  window.addEventListener("transcribe:edited", (event) => {
    renderNotes(event.detail.notes, false, false);
    if (captureActive && Array.isArray(event.detail.changes)) {
      if (event.detail.changes.length) send("live-edit", {
        batch: event.detail.changes.map(change => new URLSearchParams(change).toString()).join(";"),
      });
    }
  });
  window.addEventListener("transcribe:export-started", () => setBusy(true, true));
  window.addEventListener("transcribe:timeline", (event) => {
    timelineDuration = event.detail.duration;
    duration = Math.max(timelineDuration, 1);
    if (!applyingLiveBatch) drawRoll();
  });
  window.addEventListener("transcribe:playback-time", (event) => {
    cursorTime = Number(event.detail);
  });
  window.addEventListener("transcribe:playback-stop", () => {
    cursorTime = null;
  });
  window.addEventListener("transcribe:score-selection", (event) => {
    selectedNoteId = event.detail?.id || "";
    drawRoll();
  });
  $("piano-roll").addEventListener("click", (event) => {
    if (!notes.length) return;
    const rect = $("piano-roll").getBoundingClientRect();
    const seconds = window.TranscribeTimeline.time(event.clientX - rect.left, duration, rect.width);
    const low = pitchLow;
    const high = pitchHigh;
    const range = Math.max(12, high - low + 2);
    const minimum = low - Math.max(0, (range - (high - low + 2)) / 2);
    const pitch = minimum + (1 - (event.clientY - rect.top) / rect.height) * range - .5;
    const note = rollIndex(duration).find(({ note: item }) =>
      seconds >= item.start && seconds <= item.start + item.duration &&
      Math.abs(item.midi - pitch) <= .75)?.note;
    window.dispatchEvent(new CustomEvent("transcribe:engraving-select", { detail: { id: note?.id || "" } }));
  });
  window.addEventListener("resize", drawRoll);
  window.addEventListener("transcribe:viewport", () => {
    if (!applyingLiveBatch) drawRoll();
  });
  window.addEventListener("transcribe:viewport-manual", (event) => {
    const view = event.detail || {};
    const span = Number(view.end) - Number(view.start);
    if (Number.isFinite(span) && span > 0) liveViewportSpan = span;
    if (liveCaptureState !== "idle" && $("follow-live").checked) {
      $("follow-live").checked = false;
      updateLiveFollowControls();
    }
  });
  window.addEventListener("transcribe:theme", drawRoll);
  window.addEventListener("DOMContentLoaded", () => {
    $("follow-live").addEventListener("change", () => {
      updateLiveFollowControls();
      if ($("follow-live").checked) jumpToLive();
    });
    $("jump-live").addEventListener("click", jumpToLive);
    updateLiveFollowControls();
    loadSavedPaths();
    updateReviewModeState();
    updateProjectTitle();
    updateJourney();
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.addEventListener("message", handleMessage);
      send("ready");
    }
  });
})();
