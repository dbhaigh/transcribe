/* Browser integration tests; run through ui_workflow.html with a local HTTP server. */
window.runTranscriberWorkflowTests = async (studio) => {
  const doc = studio.document;
  const $ = (id) => doc.getElementById(id);
  const checks = [];
  const require = (condition, message) => {
    if (!condition) throw new Error(message);
  };
  const send = (name, detail) => studio.dispatchEvent(new studio.CustomEvent(`transcribe:${name}`, { detail }));
  const tick = () => new Promise(resolve => studio.requestAnimationFrame(() => studio.requestAnimationFrame(resolve)));
  const host = (message) => studio.__host({ data: message });
  const key = (value, extra = {}) => doc.dispatchEvent(new studio.KeyboardEvent("keydown",
    { key: value, bubbles: true, ...extra }));
  const timeline = studio.TranscribeTimeline;
  require(!$("journey-guide").hidden && $("journey-open").getAttribute("aria-current") === "step",
    "First-run workflow did not identify opening a recording");
  require(!$("instrument-prior").closest("details") && !$("quality-mode").closest("details"),
    "Essential setup is hidden in advanced settings");
  const explain = doc.querySelector('button[aria-controls="guitar-tuning-help"]');
  explain.click();
  require(!$("guitar-tuning-help").hidden && explain.getAttribute("aria-expanded") === "true" &&
    $("guitar-tuning").getAttribute("aria-describedby") === "guitar-tuning-help",
    "Context help is not available to keyboard/assistive interaction");
  explain.click();
  checks.push("first-run steps, visible instrument/quality setup and accessible contextual help");
  for (const id of ["open-folder-button", "status-output-folder"]) {
    $(id).click();
    require(studio.__messages.at(-1).startsWith("open-output|"), `${id} did not reach backend`);
  }
  const map = [{ start_seconds: 0, start_quarters: 0, beats_per_minute: 120 },
    { start_seconds: 2, start_quarters: 4, beats_per_minute: 60 }];
  require(timeline.quarters(3, map) === 5 && timeline.seconds(5, map) === 3, "Tempo-map conversion");
  require(timeline.samplePosition(timeline.sampleSeconds(144000, 48000), 48000) === 144000,
    "Sample position conversion");
  require(timeline.time(timeline.x(3, 10, 500), 10, 500) === 3, "Canvas coordinate inverse");
  checks.push("sample/second/quarter and canvas mappings");
  host({ type: "ready", onnx: true, asio: true, verovio: false });
  host({ type: "asio-devices", devices: [{ id: 0, name: "Test ASIO", default: true,
    channels: 2, sample_rates: [44100, 48000], preferred_sample_rate: 48000 }] });
  $("live-backend").value = "spectral";
  $("live-backend").dispatchEvent(new studio.Event("change", { bubbles: true }));
  doc.querySelector('.segment[data-mode="asio"]').click();
  $("live-hop-ms").value = "375";
  $("live-hop-ms").dispatchEvent(new studio.Event("change", { bubbles: true }));
  require(!$("live-hop-ms").disabled,
    "Windowed-backend hop interval stayed disabled for ASIO Spectral capture");
  require(JSON.parse(studio.localStorage.getItem("transcribe.paths") || "{}").liveHopMs === "375",
    "Windowed-backend hop interval was not persisted with workstation settings");
  $("record-button").click();
  require(studio.__messages.at(-1).startsWith("start-capture|") &&
    new URLSearchParams(studio.__messages.at(-1).split("|")[1]).get("version") === "1",
    "Record did not reach the production backend protocol");
  require(new URLSearchParams(studio.__messages.at(-1).split("|")[1]).get("live_hop_ms") === "375",
    "Windowed-backend hop interval was not exposed or sent with capture");
  require(!$("pause-analysis").classList.contains("hidden"), "Capture pause control is missing");
  host({ type: "live-status", version: 1, job: 1, state: "arming", status: "Starting capture" });
  host({ type: "live-status", version: 1, job: 1, state: "capturing",
    status: "Recording; live analysis active" });
  require(doc.body.classList.contains("engraving-draft"),
    "The editable canvas was not kept active during capture");
  host({ type: "audio-preview", offset: 0, replace: true, duration: 1, columns: 2,
    bands: 1, minimum_frequency_hz: 55, maximum_frequency_hz: 24000,
    waveform: [.4, .5], spectrogram: [.2, .4] });
  host({ type: "audio-preview", offset: 1, replace: false, duration: 1, columns: 1,
    bands: 1, minimum_frequency_hz: 55, maximum_frequency_hz: 24000,
    waveform: [1], spectrogram: [.6] });
  require(Number($("input-level-meter").value) === 1 &&
    $("clipping-warning").textContent.includes("Clipping detected"),
    `Live note rendering displaced the input meter or clipping warning: ` +
    `${$("input-level-meter").value}, ${$("clipping-warning").textContent}`);
  const delivered = [];
  studio.addEventListener("transcribe:live-events", event => delivered.push(...event.detail));
  const liveNote = { id: "take/live-1", midi: 64, name: "E4", start: .2, duration: .5,
    state: "provisional", revision: 1 };
  const observed = { count: 4, p50_ms: 12, p95_ms: 24, p99_ms: 30 };
  const sendBatch = (sequence, detail = {}, job = 1) => host({
    type: "live-event-batch",
    version: 1,
    job,
    sequence,
    sample_rate: 48000,
    resync: false,
    upserts: {},
    retractions: [],
    finalizations: [],
    gaps: [],
    events: [],
    metrics: {
      end_to_end: observed,
      analysis_window: observed,
      capture: observed,
      analysis_queue: observed,
      analysis: observed,
      stabilization: observed,
      confirmation: observed,
      publication_queue: observed,
      ui_dispatch: observed,
      target_provisional_seconds: .2,
      target_stabilized_seconds: .3,
      provisional_target_misses: 1,
      stabilized_target_misses: 2,
      input_queue_depth: 3,
      input_queue_high_water: 3,
      input_queue_capacity: 8,
      event_queue_depth: 1,
      event_queue_capacity: 64,
      blocks_refused: 0,
      events_dropped: 0,
      gap_frames: 0,
      sequence_gaps: 0,
      discontinuities: 0,
    },
    driver_input_latency_ms: 2.5,
    callback_period_ms: 10,
    status: { state: "capturing", message: "Recording; live analysis active" },
    ...detail,
  });
  const recordedLiveStream = [
    { sequence: 1, upserts: { [liveNote.id]: { revision: 1, note: liveNote } },
      recording_seconds: 12.3,
      events: [{ type: "provisional", id: liveNote.id, sequence: 1, midi: 64, time: .2 }] },
    { sequence: 2, upserts: { [liveNote.id]: { revision: 2,
      note: { ...liveNote, state: "stabilized", revision: 2 } } },
      recording_seconds: 12.3,
      events: [{ type: "stabilized", id: liveNote.id, sequence: 2, midi: 64, time: .2 }] },
    { sequence: 3, retractions: [{ id: liveNote.id, revision: 3 }],
      events: [{ type: "retracted", id: liveNote.id, sequence: 3, midi: 64, time: .2 }] },
    { sequence: 4, resync: true, recording_seconds: 30,
      upserts: { "take/live-2": { revision: 4, note: {
        id: "take/live-2", midi: 67, name: "G4", start: 29,
        duration: .4, state: "stabilized", revision: 4,
      } } },
      gaps: [{ first_frame: 1392000, end_frame: 1416000,
        start_seconds: 29, end_seconds: 29.5, message: "analysis skipped" }],
      events: [{ type: "gap", sequence: 4, first_frame: 1392000,
        end_frame: 1416000, time: 29, message: "analysis skipped" }] },
    { sequence: 5, recording_seconds: 30.1,
      upserts: { "take/live-2": { revision: 5, note: {
        id: "take/live-2", midi: 67, name: "G4", start: 29.1,
        duration: .4, state: "stabilized", revision: 5,
      } } },
      events: [{ type: "corrected", id: "take/live-2", sequence: 5,
        midi: 67, time: 29.1 }] },
  ];
  recordedLiveStream.slice(0, 2).forEach(batch => sendBatch(batch.sequence, batch));
  await tick();
  require(delivered.length === 2 && $("note-list").textContent.includes("STABILIZED") &&
    $("piano-roll").dataset.stabilizedNotes === "1",
    "Coalescing lost note revisions or stabilized rendering");
  require($("recording-timer").textContent === "00:12.3", "Recording timer is not sample-time based");
  require($("live-diagnostics").textContent.includes("queue 3/8") &&
    $("status-text").textContent === "Recording; live analysis active",
    "Detailed telemetry displaced concise, accessible capture status");
  require(!$("live-health-strip").hidden &&
    $("health-driver").textContent.includes("2.5 ms reported") &&
    $("health-callback").textContent.includes("10.0 ms") &&
    $("health-window").textContent === "p50 12.0 / p95 24.0 ms" &&
    $("health-ui-dispatch").textContent === "p50 12.0 / p95 24.0 ms" &&
    $("health-provisional-target").textContent.includes("1 misses / 4 observations") &&
    $("health-provisional-target").textContent.includes("target 200 ms") &&
    $("health-stabilized-target").textContent.includes("2 misses / 4 observations") &&
    $("health-stabilized-target").textContent.includes("target 300 ms"),
    "The live health strip omitted stage percentiles, dispatch or target misses");
  require($("follow-live").checked &&
    Math.abs(timeline.bounds(12.3).end - 12.3) < .01,
    "Live viewport did not follow the newest evidence edge");
  sendBatch(recordedLiveStream[2].sequence, recordedLiveStream[2]);
  await tick();
  require($("piano-roll").dataset.retractedNotes === "1" &&
    $("note-list").textContent.includes("RETRACTED"),
    "Retracted notes were not shown with a distinct fading state");
  await new Promise(resolve => setTimeout(resolve, 550));
  await tick();
  require(!$("note-list").textContent.includes("E4"),
    "Retracted note was not removed after its fade");

  recordedLiveStream.slice(3).forEach(batch => sendBatch(batch.sequence, batch));
  await tick();
  require($("note-list").textContent.includes("G4") &&
    !$("note-list").textContent.includes("E4") &&
    $("note-list").textContent.includes("CORRECTED"),
    "Resync did not replace stale notes or correction highlight was missing");
  require($("piano-roll").dataset.gapRegions === "1" &&
    $("staff-view").dataset.gapRegions === "1" &&
    $("tab-view").dataset.gapRegions === "1" &&
    $("tab-view").dataset.unresolvedTabNotes === "1",
    "Analysis gaps or unresolved tablature were not rendered across synchronized canvases");
  checks.push("mock-host live stream: provisional, stabilized, retraction fade, correction, resync and gap overlays");

  sendBatch(6, { finalizations: [{ id: "take/live-2", revision: 6,
    note: { id: "take/live-2", midi: 67, name: "G4", start: 29.1,
      duration: .4, revision: 6, state: "finalized", finalized: true } }],
    events: [{ type: "finalized", id: "take/live-2", sequence: 6,
      midi: 67, time: 29.5 }],
    driver_input_latency_ms: null,
    callback_period_ms: 0,
    metrics: {
      end_to_end: { count: 0, p50_ms: 0, p95_ms: 0 },
      analysis_window: { count: 0, p50_ms: 0, p95_ms: 0 },
      capture: { count: 0, p50_ms: 0, p95_ms: 0 },
      analysis_queue: { count: 0, p50_ms: 0, p95_ms: 0 },
      analysis: { count: 0, p50_ms: 0, p95_ms: 0 },
      stabilization: { count: 0, p50_ms: 0, p95_ms: 0 },
      publication_queue: { count: 0, p50_ms: 0, p95_ms: 0 },
      ui_dispatch: { count: 0, p50_ms: 0, p95_ms: 0 },
      target_provisional_seconds: .2,
      target_stabilized_seconds: .3,
      provisional_target_misses: 0,
      stabilized_target_misses: 0,
      overloaded: true,
      input_queue_depth: 8,
      input_queue_capacity: 8,
      event_queue_depth: 64,
      event_queue_capacity: 64,
      blocks_refused: 3,
      events_dropped: 2,
      gap_frames: 960,
      sequence_gaps: 2,
      discontinuities: 1,
      skipped_frames: 40,
      windows_coalesced: 1,
      tracked_notes_refused: 1,
    } });
  await tick();
  require($("piano-roll").dataset.finalizedNotes === "1" &&
    $("note-list").textContent.includes("FINALIZED"),
    "Finalized notes did not return to normal solid rendering");
  require($("health-driver").textContent === "unknown" &&
    $("health-callback").textContent === "unmeasured" &&
    $("health-analysis-execution").textContent === "unmeasured" &&
    $("health-ui-dispatch").textContent === "unmeasured" &&
    $("health-provisional-target").textContent.startsWith("unmeasured") &&
    $("health-overload").textContent.includes("OVERLOAD: active") &&
    $("health-overload").textContent.includes("3 blocks") &&
    $("health-input-gap").textContent.includes("INPUT GAP: detected") &&
    $("health-input-gap").textContent.includes("960 missing frames") &&
    $("live-diagnostics").textContent.includes("not physical end-to-end latency"),
    "Zero observations, queue overload and known input gaps were not reported distinctly");

  const paintCount = Number($("piano-roll").dataset.paintCount);
  for (let sequence = 7; sequence < 17; sequence += 1) {
    const id = `take/burst-${sequence}`;
    const note = { id, midi: 60 + sequence % 12, name: "C4", start: 20 + sequence / 10,
      duration: .2, state: "provisional", revision: 1 };
    sendBatch(sequence, { upserts: { [id]: { revision: 1, note } },
      recording_seconds: 30.1, events: [] });
  }
  await tick();
  require(Number($("piano-roll").dataset.paintCount) - paintCount <= 1,
    "Burst updates redrew the live timeline more than once per animation frame");
  $("timeline-scroll").value = "0";
  $("timeline-scroll").dispatchEvent(new studio.Event("input", { bubbles: true }));
  require(!$("follow-live").checked && !$("jump-live").hidden,
    "Manual timeline navigation did not disable Follow live and offer Jump to live");
  $("jump-live").click();
  require(Math.abs(timeline.bounds(30.1).end - 30.1) < .01 &&
    !$("follow-live").checked, "Jump to live failed or unexpectedly re-enabled following");
  $("follow-live").checked = true;
  $("follow-live").dispatchEvent(new studio.Event("change"));
  $("timeline-zoom-in").click();
  require(!$("follow-live").checked && !$("jump-live").hidden,
    "Manual zoom did not disable Follow live and expose Jump to live");

  $("pause-analysis").click();
  require(studio.__messages.at(-1) === "pause-analysis|paused=1", "Pause did not reach the backend");
  host({ type: "analysis-paused", paused: true });
  sendBatch(17, { recording_seconds: 31.4, events: [] });
  await tick();
  require($("note-list").textContent.includes("G4") &&
    $("recording-timer").textContent === "00:31.4" &&
    $("pause-analysis").getAttribute("aria-pressed") === "true",
    "Paused analysis stopped the capture timer or erased displayed notes");
  $("pause-analysis").click();
  require(studio.__messages.at(-1) === "pause-analysis|paused=0", "Resume did not reach the backend");
  host({ type: "analysis-paused", paused: false });
  $("cancel-button").click();
  require(studio.__messages.at(-1).startsWith("stop-capture|"),
    "Graceful stop capture did not reach the backend");
  checks.push("start/stop protocol, sample-time timer, pause/resume and live-follow controls");
  const metadata = { score_id: "workflow", score_revision: 1, tempo: 120, tempo_inferred: true,
    time_numerator: 4, time_denominator: 4, key_fifths: 0, tempo_map: map, source_sample_rate: 48000 };
  const notes = [
    { id: "one", midi: 64, name: "E4", start: 0, duration: .5, amplitude: .2 },
    { id: "two", midi: 69, name: "A4", start: 1, duration: .5, amplitude: .2 },
  ];
  host({ type: "complete", success: true, notes, metadata, fingering: {
    notes: [{ string: 1, fret: 0, candidates: [] }, { string: 1, fret: 5, candidates: [] }],
  }, summary: "fixture" });
  $("record-button").click();
  host({ type: "live-status", version: 1, job: 2, state: "capturing",
    status: "Recording; live analysis active" });
  const retakeNote = { id: "take/retake-1", midi: 65, name: "F4",
    start: .1, duration: .3, state: "provisional", revision: 1 };
  sendBatch(1, { job: 2, recording_seconds: .5,
    upserts: { [retakeNote.id]: { revision: 1, note: retakeNote } },
    events: [{ type: "provisional", id: retakeNote.id, midi: 65, time: .1 }] }, 2);
  await tick();
  require($("note-list").textContent.includes("F4") &&
    !$("note-list").textContent.includes("A4"),
    "A new live capture did not render on top of the existing accepted score");
  host({ type: "live-status", version: 1, job: 2, state: "finished",
    status: "Capture finished" });
  host({ type: "proposal", success: true, accepted_revision: 1, notes: [retakeNote] });
  require($("note-list").textContent.includes("E4") &&
    $("note-list").textContent.includes("A4") &&
    !$("note-list").textContent.includes("F4"),
    "Proposal-only completion did not restore the previously accepted score");
  send("engraving-select", { id: "one" });
  $("edit-duration").value = "2";
  $("apply-note-edit").click();
  require($("edit-instructions").textContent.includes("overlapping"),
    `Invalid duration edit was not rejected: ${$("edit-instructions").textContent}; ` +
    `string ${$("edit-string").value}, fret ${$("edit-fret").value}, start ${$("edit-start").value}, ` +
    `duration ${$("edit-duration").value}, selection ${$("selection-label").textContent}`);
  require($("edit-duration").value === "0.50", "Failed transaction did not roll back");
  key("ArrowUp");
  require($("edit-pitch").value === "65", "Pitch keyboard edit");
  require($("fingering-alternatives").textContent.includes("Unresolved"), "Pitch edit did not clear stale tablature");
  $("undo-button").click();
  require($("edit-pitch").value === "64", "Undo pitch/selection");
  $("redo-button").click();
  require($("edit-pitch").value === "65", "Redo pitch");
  for (let i = 0; i < 3; i += 1) { $("undo-button").click(); $("redo-button").click(); }
  require($("edit-pitch").value === "65", "Repeated undo/redo");
  $("undo-button").click();
  key("ArrowDown");
  require($("redo-button").disabled, "A new keyboard edit retained the old redo branch");
  checks.push("transaction rollback, synchronized pitch/tab, selection and repeated undo/redo");
  $("delete-note").click();
  require($("note-list").textContent.includes("A4") && !$("note-list").textContent.includes("D#4"),
    "Delete did not update note list");
  $("undo-button").click();
  require($("edit-pitch").value === "63", "Undo delete did not restore selected note");
  checks.push("delete and selection restoration");

  // Restore the canonical revision before testing timing-dependent controls.
  host({ type: "complete", success: true, notes, metadata: { ...metadata, score_id: "workflow-time" },
    fingering: { notes: [{ string: 1, fret: 0 }, { string: 1, fret: 5 }] }, summary: "fixture" });
  send("engraving-select", { id: "one" });
  $("tempo-auto").checked = false;
  $("tempo-auto").dispatchEvent(new studio.Event("change"));
  $("tempo-control").value = "60";
  $("tempo-control").dispatchEvent(new studio.Event("change"));
  send("engraving-select", { id: "two" });
  require($("edit-start").value === "2.00" && $("edit-duration").value === "1.00",
    "Tempo edit did not retain musical positions");
  $("undo-button").click();
  send("engraving-select", { id: "two" });
  require($("edit-start").value === "1.00" && $("edit-duration").value === "0.50", "Undo tempo projection");
  checks.push("tempo edit and undo preserve musical positions");

  host({ type: "proposal", success: true, accepted_revision: 1, notes: [notes[0]] });
  let confirmation = 0;
  studio.confirm = () => { confirmation += 1; return false; };
  $("accept-proposal").click();
  require(confirmation === 1 && !$("accept-proposal").disabled, "Proposal replaced edits without consent");
  checks.push("explicit consent before proposal replacement");
  host({ type: "live", notes: [{ id: "stale", midi: 20, name: "stale", start: 0, duration: 1 }] });
  await tick();
  require(!$("note-list").textContent.includes("stale"), "Background preview replaced an accepted draft");
  send("busy", { busy: true, preserveScore: true, liveCapture: false });
  const beforeBusy = $("edit-pitch").value;
  key("ArrowUp");
  $("staff-view").dispatchEvent(new studio.MouseEvent("dblclick", { clientX: 80, clientY: 80 }));
  require($("edit-pitch").value === beforeBusy, "Busy keyboard/insert changed a score being exported");
  send("busy", { busy: false });
  checks.push("stale live preview protection and busy edit isolation");

  const staff = $("staff-view");
  const staffRect = staff.getBoundingClientRect();
  staff.dispatchEvent(new studio.MouseEvent("dblclick", {
    clientX: staffRect.left + timeline.x(.75, 1.5, staffRect.width),
    clientY: staffRect.top + staffRect.height / 2,
  }));
  require($("note-list").querySelectorAll(".note-row").length === 3, "Insertion did not update all views");
  $("undo-button").click();
  require($("note-list").querySelectorAll(".note-row").length === 2, "Undo insertion");
  send("engraving-select", { id: "two" });
  const initialStart = Number($("edit-start").value);
  const pointerX = staffRect.left + timeline.x(1, 1.5, staffRect.width);
  const pointerY = staffRect.top + staffRect.height / 2 + 22 - 16.5;
  staff.setPointerCapture = () => {};
  staff.dispatchEvent(new studio.PointerEvent("pointerdown", {
    clientX: pointerX, clientY: pointerY, pointerId: 1,
  }));
  staff.dispatchEvent(new studio.PointerEvent("pointermove", {
    clientX: pointerX + 20, clientY: pointerY, pointerId: 1,
  }));
  staff.dispatchEvent(new studio.PointerEvent("pointercancel", { pointerId: 1 }));
  require(Number($("edit-start").value) === initialStart, "Cancelled drag committed partial timing");
  checks.push("insertion undo and cancelled drag rollback");
  send("busy", { busy: true, preserveScore: false, liveCapture: true });
  send("engraving-select", { id: "two" });
  $("delete-note").click();
  send("notes", notes);
  send("engraving-select", { id: "two" });
  require($("apply-note-edit").disabled, "Live delete tombstone did not suppress delayed note snapshot");
  $("undo-button").click();
  send("notes", notes);
  send("engraving-select", { id: "two" });
  require(!$("apply-note-edit").disabled, "Undo live delete retained the deletion tombstone");
  send("busy", { busy: false });
  send("metadata", metadata);
  checks.push("live delete tombstones and undo restoration");

  const calls = [];
  const waveformContext = $("waveform").getContext("2d");
  const originalMove = waveformContext.moveTo.bind(waveformContext);
  waveformContext.moveTo = (x, y) => { calls.push(x); originalMove(x, y); };
  host({ type: "audio-preview", replace: true, offset: 0, duration: 1, bands: 1,
    waveform: [.1, .2], spectrogram: [.2, .3] });
  host({ type: "audio-preview", replace: false, offset: 3, duration: 1, bands: 1,
    waveform: [.3, .4], spectrogram: [.3, .4] });
  await tick();
  require($("audio-time").textContent.includes("4.0"), "Incremental preview was dropped");
  const width = $("waveform").getBoundingClientRect().width;
  require(calls.some(x => Math.abs(x - timeline.x(3.25, 4, width)) < .01), "Timestamped audio gap was compressed");
  waveformContext.moveTo = originalMove;
  checks.push("incremental previews retain timestamped gaps and common alignment");

  let resume;
  let clock = 0;
  const started = [];
  class FakeAudioContext {
    constructor() { this.state = "running"; this.destination = {}; }
    get currentTime() { return clock; }
    resume() { return new Promise(resolve => { resume = resolve; }); }
    createOscillator() {
      return { frequency: {}, connect() {}, disconnect() {}, start(time) { started.push(time); }, stop() {} };
    }
    createGain() {
      return { gain: { setValueAtTime() {}, linearRampToValueAtTime() {}, setTargetAtTime() {} },
        connect() {}, disconnect() {} };
    }
  }
  studio.AudioContext = FakeAudioContext;
  const cursors = [];
  studio.addEventListener("transcribe:playback-time", event => cursors.push(event.detail));
  $("play-midi").click();
  $("stop-playback").click();
  resume();
  await tick();
  require(started.length === 0 && $("stop-playback").disabled, "Stopped async audio startup revived playback");
  $("play-midi").click();
  resume();
  await tick();
  require(started[0] === .08 && cursors.at(-1) === 0, "Cursor started before synthesized audio");
  clock = .33;
  await tick();
  require(Math.abs(cursors.at(-1) - .25) < .0001, "Cursor is not following AudioContext time");
  $("stop-playback").click();
  checks.push("async playback cancellation and audio-clock cursor alignment");
  $("input-path").value = "reference.wav";
  $("play-original").disabled = false;
  $("play-original").click();
  const playbackRequest = studio.__messages.filter(message => message.startsWith("play-original|")).at(-1);
  const token = new URLSearchParams(playbackRequest.split("|")[1]).get("token");
  host({ type: "playback-state", token, state: "playing", seconds: .7 });
  await tick();
  require(cursors.at(-1) === .7, "Original cursor ignored native playback position");
  host({ type: "playback-state", token: Number(token) - 1, state: "ended" });
  require(!$("stop-playback").disabled, "Stale playback termination stopped the current transport");
  host({ type: "playback-state", token, state: "ended" });
  require($("stop-playback").disabled, "Native playback termination did not clear transport");
  checks.push("native original-audio cursor, generation guard and termination");
  $("export-edits").click();
  const exportRequest = studio.__messages.filter(message => message.startsWith("export-edits|")).at(-1);
  const fields = new URLSearchParams(exportRequest.split("|")[1]);
  require(fields.has("tab_assignments") && fields.get("score_id") === metadata.score_id &&
    fields.get("base_revision") === "1", "Complete tab assignments and score identity/revision were not transmitted");
  host({ type: "exported", success: false, error: "Injected transaction rejection" });
  require($("note-list").querySelectorAll(".note-row").length === 2 &&
    $("status-text").textContent.includes("Injected transaction rejection"), "Backend rejection lost draft state");
  checks.push("authoritative export payload and failed-commit draft preservation");
  send("source-changed");
  require($("audio-time").textContent === "Waiting for audio" && $("stop-playback").disabled,
    "Source transition retained preview/playback state");
  require(studio.__errors.length === 0, `Browser errors: ${studio.__errors.join("; ")}`);
  checks.push("source transition cleanup and no browser errors");
  host({ type: "complete", success: true, notes: [],
    metadata: { ...metadata, score_id: "empty-project" }, fingering: { notes: [] }, summary: "empty" });
  const emptyRect = staff.getBoundingClientRect();
  staff.dispatchEvent(new studio.MouseEvent("dblclick", {
    clientX: emptyRect.left + 70, clientY: emptyRect.top + emptyRect.height / 2,
  }));
  require($("note-list").querySelectorAll(".note-row").length === 1 && !$("export-edits").disabled,
    "An inserted note in an empty accepted project cannot be exported");
  $("undo-button").click();
  require($("note-list").querySelectorAll(".note-row").length === 0 && !$("export-edits").disabled,
    "Deleting/undoing the last note cannot commit an empty score");
  checks.push("empty-project insertion and committing an empty score");
  require(studio.__errors.length === 0, `Browser errors: ${studio.__errors.join("; ")}`);
  host({ type: "complete", success: true, notes, metadata,
    fingering: { notes: [{ string: 1, fret: 0 }, { string: 1, fret: 5 }] }, summary: "split fixture" });
  send("engraving-select", { id: "one" });
  $("split-note").click();
  require($("note-list").querySelectorAll(".note-row").length === 3 &&
    $("edit-duration").value === "0.25", "Transactional split did not update the shared score");
  $("merge-note").click();
  require($("note-list").querySelectorAll(".note-row").length === 2 &&
    $("edit-duration").value === "0.50", "Same-pitch merge did not restore duration");
  $("undo-button").click();
  require($("note-list").querySelectorAll(".note-row").length === 3, "Undo merge");
  $("undo-button").click();
  require($("note-list").querySelectorAll(".note-row").length === 2, "Undo split");
  checks.push("transactional split, merge and undo across notation and tablature");
  await tick();
  const beforePaint = ["waveform", "spectrogram", "staff-view", "tab-view", "piano-roll"]
    .map(id => Number($(id).dataset.paintCount));
  for (let i = 0; i < 30; i += 1) send("playback-time", i / 100);
  await tick();
  require(beforePaint.every((value, index) => value === Number(
    $(["waveform", "spectrogram", "staff-view", "tab-view", "piano-roll"][index]).dataset.paintCount)),
    "Moving the playhead repainted static score/audio canvases");
  const overlays = doc.querySelectorAll(".timeline-playhead");
  require(overlays.length === 5 && Array.from(overlays).every(line => !line.hidden),
    "One shared playhead did not reach every time surface");
  timeline.setView(.25, .5, 4);
  require(Math.abs(timeline.time(timeline.x(.5, 4, 500), 4, 500) - .5) < 1e-9,
    "Viewport coordinate conversion");
  await tick();
  require($("timeline-window").textContent.includes("0.3"), "Viewport status did not update");
  timeline.setView(0, null, 4);
  send("playback-stop");
  checks.push("shared zoom/pan and cursor-only playback updates with no score rerender");
  const longNotes = Array.from({ length: 10000 }, (_, i) =>
    ({ start: i, duration: .25, midi: 60, id: `long-${i}` }));
  timeline.setView(5000, 10, 10000);
  const visible = timeline.index(longNotes)(10000);
  require(visible.length === 11 && visible[0].index === 5000,
    "Long-score interval index did not bound visible note work");
  timeline.setView(0, null, 4);
  checks.push("ten-thousand-note viewport query preserves original indices and bounded visible range");
  host({ type: "complete", success: true, notes, metadata, fingering: {
    notes: [{ string: 1, fret: 0 }, { string: 1, fret: 5 }],
    alternatives: [{ cost: { total: 3 }, positions: [
      { note_index: 0, string: 2, fret: 5 }, { note_index: 1, string: 2, fret: 10 },
    ] }],
  }, summary: "phrase fixture" });
  send("engraving-select", { id: "one" });
  doc.querySelector(".phrase-fingering").click();
  require($("edit-string").value === "2" && $("edit-fret").value === "5" &&
    $("edit-pitch").value === "64", "Phrase alternative changed sounding pitch");
  send("engraving-select", { id: "two" });
  require($("edit-string").value === "2" && $("edit-fret").value === "10",
    "Phrase alternative did not update the whole sequence");
  $("undo-button").click();
  send("engraving-select", { id: "two" });
  require($("edit-string").value === "1" && $("edit-fret").value === "5",
    "Phrase fingering undo did not restore the complete transaction");
  send("project-settings", { tuning: [64, 59, 55, 50, 45, 40, 35] });
  host({ type: "complete", success: true, metadata, notes: [
    { id: "extended", midi: 35, name: "B1", start: 0, duration: 1,
      guitar_technique: { gesture: "sustained pitch rise", explanation: "Bend or slide unresolved." } },
  ], fingering: { notes: [{ string: 7, fret: 0 }] }, summary: "extended guitar" });
  send("engraving-select", { id: "extended" });
  require($("edit-string").options.length === 8 && $("edit-string").value === "7" &&
    $("edit-instructions").textContent.includes("Bend or slide unresolved") &&
    $("fingering-alternatives").textContent.includes("not detected strings"),
    "Extended strings or separate technique/fingering uncertainty are missing");
  $("edit-fret").value = "0";
  $("apply-note-edit").click();
  require($("edit-pitch").value === "35" && $("edit-string").value === "7",
    "Editing a seven-string project rejected its valid string");
  send("project-settings", { tuning: [64, 59, 55, 50, 45, 40] });
  checks.push("joint phrase alternatives, atomic undo, extended strings and separate technique uncertainty");
  let engravingWorker;
  studio.Worker = class {
    constructor() { this.listeners = {}; this.requests = []; engravingWorker = this; }
    addEventListener(type, listener) { this.listeners[type] = listener; }
    postMessage(request) { this.requests.push(request); }
  };
  send("config", { verovio: true });
  const engraving = { score_revision: 10, notes: [],
    musicxml: '<score-partwise version="4.0"><part-list/></score-partwise>' };
  send("accepted-score", engraving);
  send("accepted-score", { ...engraving, score_revision: 11 });
  require(engravingWorker.requests.length === 1, "Engraving requests were not serialized");
  send("edited", {});
  engravingWorker.listeners.message({ data: { type: "rendered",
    requestId: engravingWorker.requests[0].requestId, pages: [] } });
  require(engravingWorker.requests.length === 1 && doc.body.classList.contains("engraving-draft"),
    "Queued accepted engraving replaced a newer draft");
  $("engraving-layout").dispatchEvent(new studio.Event("change"));
  require($("engraving-layout").disabled && engravingWorker.requests.length === 1,
    "Changing layout republished an old accepted score over a draft");
  checks.push("stale engraving and queued page-layout results cannot clear newer draft edits");
  doc.querySelector('[data-mode="file"]').click();
  $("input-path").value = "mixture.wav";
  $("musicxml-path").value = "mixed.musicxml";
  $("tab-path").value = "mixed.txt";
  $("instrument-prior").value = "bass";
  $("isolated-path").value = "bass-isolated.wav";
  $("isolation-enabled").checked = true;
  $("isolation-reviewed").checked = false;
  const beforeIsolation = studio.__messages.filter(message => message.startsWith("transcribe|")).length;
  $("transcribe-button").click();
  require(studio.__messages.filter(message => message.startsWith("transcribe|")).length === beforeIsolation,
    "Unreviewed isolation bypassed explicit acceptance");
  $("isolation-preview-start").value = "90";
  $("preview-isolated").click();
  let audition = new URLSearchParams(studio.__messages.at(-1).split("|")[1]);
  require(studio.__messages.at(-1).startsWith("audition-source|") &&
    audition.get("path") === "bass-isolated.wav" && audition.get("start") === "90",
    "Isolated audio cannot be auditioned across time before transcription");
  $("preview-mixture").click();
  audition = new URLSearchParams(studio.__messages.at(-1).split("|")[1]);
  require(audition.get("path") === "mixture.wav", "Mixture/stem comparison selected wrong source");
  $("isolation-reviewed").checked = true;
  $("preprocessing-fallback").checked = true;
  $("transcribe-button").click();
  const isolationRequest = new URLSearchParams(studio.__messages.filter(
    message => message.startsWith("transcribe|")).at(-1).split("|")[1]);
  require(isolationRequest.get("isolated") === "bass-isolated.wav" &&
    isolationRequest.get("instrument") === "bass" &&
    isolationRequest.get("isolation_enabled") === "1" &&
    isolationRequest.get("isolation_reviewed") === "1" &&
    isolationRequest.get("preprocessing_fallback") === "1",
    "Reviewed isolation was not wired to the production request");
  host({ type: "complete", success: true, notes, metadata, fingering: { notes: [] },
    summary: "mixed", preprocessing_diagnostic: "explicit fallback: original mixture" });
  require($("result-message").textContent.includes("explicit fallback"),
    "Fallback warning disappeared when transcription completed");
  $("isolated-path").dispatchEvent(new studio.Event("change"));
  require(!$("isolation-reviewed").checked, "Source change retained stale isolation approval");
  $("isolation-enabled").checked = false;
  $("transcribe-button").click();
  const disabledIsolation = new URLSearchParams(studio.__messages.filter(
    message => message.startsWith("transcribe|")).at(-1).split("|")[1]);
  require(disabledIsolation.get("isolation_enabled") === "0", "Cannot disable preprocessing");
  host({ type: "complete", success: false, error: "Test cancellation" });
  $("isolation-enabled").checked = true;
  $("isolation-reviewed").checked = true;
  host({ type: "complete", success: true, project_opened: true, notes, metadata,
    fingering: { notes: [] }, summary: "opened" });
  require(!$("isolation-enabled").checked && !$("isolation-reviewed").checked &&
    $("isolated-path").value === "", "Project open trusted stale preprocessing settings");
  checks.push("optional isolation audition, source changes, explicit acceptance, bass profile and disclosed fallback");

  const projectV2 = {
    project_path: "C:\\projects\\take.transcriber", title: "Version two",
    summary: "Project-owned copy", audio_path: "C:\\projects\\recording.wav",
    annotation_text: "Keep the expressive timing.",
    settings: { quantization: "evidence", quantization_strength: .35,
      minimum_rhythm_evidence: .8, retain_review_evidence: true, bounded_inference: false },
  };
  host({ type: "complete", success: true, notes, metadata, project: projectV2,
    fingering: { notes: [] }, summary: "Restored project" });
  require($("project-annotation").value === projectV2.annotation_text &&
    $("quantization-mode").value === "evidence" &&
    $("quantization-strength").value === "0.35" && $("rhythm-evidence").value === "0.8" &&
    $("review-mode").checked, "Project settings/annotations were not restored");
  $("project-annotation").value = "Reviewed phrase.";
  $("project-annotation").dispatchEvent(new studio.Event("input", { bubbles: true }));
  require($("save-state").textContent === "Unsaved edits", "Annotation edit was not marked unsaved");
  $("save-portable-project").click();
  require(studio.__messages.at(-1).includes("field=project-portable"),
    "Portable save did not ask for an explicit destination");
  host({ type: "field", field: "project-portable", value: "C:\\portable\\take.transcriber" });
  const portableFields = new URLSearchParams(studio.__messages.filter(
    message => message.startsWith("save-project|")).at(-1).split("|")[1]);
  require(portableFields.get("copy_audio") === "1" &&
    portableFields.get("annotation_text") === "Reviewed phrase." &&
    portableFields.get("quantization_strength") === "0.35", "Portable save lost copy consent/settings");
  host({ type: "exported", success: false, error: "Injected save failure" });
  $("relink-project-audio").click();
  require(studio.__messages.at(-1).includes("field=project-relink"),
    "Missing-audio workflow did not open the relocation picker");
  host({ type: "field", field: "project-relink", value: "D:\\original.wav" });
  const relinkFields = new URLSearchParams(studio.__messages.filter(
    message => message.startsWith("autosave-project|")).at(-1).split("|")[1]);
  require(relinkFields.get("relink_audio") === "D:\\original.wav" &&
    relinkFields.has("notes"), "Audio relocation omitted pending editor changes");
  host({ type: "project-relinked", project: { ...projectV2,
    audio_path: "D:\\original.wav", annotation_text: "Reviewed phrase." } });
  require($("input-path").value === "D:\\original.wav" &&
    $("note-list").querySelectorAll(".note-row").length === 2 &&
    $("save-state").textContent === "Unsaved edits", "Relink replaced score or hid unsaved reference");
  checks.push("versioned project settings/annotations, explicit portable save and score-preserving audio relocation");

  $("transcribe-button").click();
  host({ type: "progress", status: "Analyzing test phase", progress: 137.8 });
  require($("phase-progress").getAttribute("aria-valuenow") === "100" &&
    $("progress-value").textContent.includes("phase") &&
    $("activity-panel").dataset.state === "processing" &&
    $("workspace").getAttribute("aria-busy") === "true", "Progress was not clamped or phase-labelled");
  host({ type: "progress", status: "Initializing model" });
  require(!$("phase-progress").hasAttribute("aria-valuenow") &&
    $("phase-progress").classList.contains("indeterminate"), "Unknown progress invented a percentage");
  const cancelCount = studio.__messages.filter(message => message.startsWith("cancel|")).length;
  $("cancel-button").click();
  $("cancel-button").click();
  require($("cancel-button").disabled && $("activity-panel").dataset.state === "canceling" &&
    studio.__messages.filter(message => message.startsWith("cancel|")).length === cancelCount + 1,
    "Cancellation did not acknowledge once and wait");
  host({ type: "complete", success: false, cancelled: true,
    error: "Transcription cancelled; accepted score unchanged." });
  require($("activity-panel").dataset.state === "canceled" &&
    !$("result-message").classList.contains("error") && !$("transcribe-button").disabled &&
    $("note-list").querySelectorAll(".note-row").length === 2 &&
    $("workspace").getAttribute("aria-busy") === "false",
    "Cancellation appeared as an error or destroyed the accepted score");
  host({ type: "error", message: "Test corrupt audio; select another file." });
  require($("activity-panel").dataset.state === "error" &&
    $("activity-detail").textContent.includes("select another file"), "Actionable error state missing");
  host({ type: "complete", success: true, notes: [], metadata, fingering: { notes: [] }, summary: "Silence" });
  require($("activity-heading").textContent === "No notes detected" && !$("empty-roll").classList.contains("hidden"),
    "Successful empty analysis has no useful recovery state");
  host({ type: "complete", success: true, notes, metadata, fingering: { notes: [] }, summary: "Recovered" });
  require($("journey-review").getAttribute("aria-current") === "step", "Guide did not reach score review");
  checks.push("phase/indeterminate progress, one-shot cancellation, preserved score, errors and empty-result recovery");

  send("engraving-select", { id: "one" });
  require($("detected-duration").textContent === "0.500 s", "Inspector omitted editable score duration");
  key("ArrowRight");
  require($("edit-pitch").value === "69" && $("edit-start").value === "1.00",
    "Keyboard next-note selection did not synchronize inspector");
  key("ArrowLeft");
  key("ArrowUp");
  require($("save-state").textContent === "Unsaved edits" &&
    $("project-title").textContent.includes("*"), "Unnamed edited score has no unsaved indication");
  const primaryStatus = $("status-text").textContent;
  host({ type: "project-autosaved" });
  require($("save-state").textContent === "Unsaved edits" &&
    $("status-text").textContent === primaryStatus, "Recovery autosave hid unsaved edits or replaced primary status");
  const editedPitch = $("edit-pitch").value;
  $("help-button").click();
  key("ArrowUp");
  key("Delete");
  require($("help-dialog").open && $("edit-pitch").value === editedPitch &&
    $("note-list").querySelectorAll(".note-row").length === 2, "Dialog keyboard input edited the score underneath");
  $("close-help").click();
  $("edit-pitch").dispatchEvent(new studio.KeyboardEvent("keydown", { key: "ArrowUp", bubbles: true }));
  require($("edit-pitch").value === editedPitch, "Editor shortcuts intercepted a numeric input");
  $("undo-button").dispatchEvent(new studio.KeyboardEvent("keydown", { key: "ArrowUp", bubbles: true }));
  require($("edit-pitch").value === editedPitch, "Editor shortcuts intercepted a focused button");
  key("z", { ctrlKey: true });
  key("F1");
  require($("help-dialog").open, "F1 did not open keyboard help");
  $("show-guide").click();
  require(!$("help-dialog").open && !$("journey-guide").hidden, "Cannot restore onboarding from help");
  $("hide-guide").click();
  require(studio.localStorage.getItem("transcribe.guide.hidden") === "true", "Guide preference did not persist");
  $("help-button").click();
  $("show-guide").click();
  checks.push("keyboard note navigation, duration/evidence inspection, unsaved edits, modal/control isolation and help persistence");

  send("audio-preview", { offset: 0, duration: 4, replace: true, bands: 2,
    minimum_frequency_hz: 55, maximum_frequency_hz: 22050,
    waveform: [.2, .4], spectrogram: [.1, .3, .2, .4] });
  timeline.setView(1, 1, 4);
  await tick();
  require($("spectrogram").getAttribute("aria-label").includes("55 to 22050 Hz") &&
    $("waveform").getAttribute("aria-label").includes("1.00 to 2.00 seconds"),
    "Audio axes do not expose backend frequency bounds or shared viewport");
  require(!$("timeline-scroll").disabled && Number($("timeline-scroll").max) === 3,
    "Horizontal timeline control does not reflect zoom");
  $("timeline-scroll").value = "2";
  $("timeline-scroll").dispatchEvent(new studio.Event("input", { bubbles: true }));
  require(timeline.bounds(4).start === 2, "Horizontal slider did not pan every view");
  key("0");
  require($("timeline-scroll").disabled, "Full-take shortcut did not reset scrolling");
  key("+");
  require(!$("timeline-scroll").disabled, "Zoom shortcut did not create a scrollable view");
  checks.push("sample-time and real frequency axes, shared horizontal slider and zoom shortcuts");

  const largeScore = longNotes.map(note => ({ ...note, name: "C4" }));
  const loadStart = studio.performance.now();
  host({ type: "complete", success: true, notes: largeScore, metadata,
    fingering: { notes: [] }, summary: "Ten thousand notes" });
  timeline.setView(0, null, 10000);
  await tick();
  const fullLoadMs = studio.performance.now() - loadStart;
  require($("note-list").querySelectorAll(".note-row").length <= 80 &&
    Number($("piano-roll").dataset.paintedNotes) <= 4001 &&
    $("roll-duration").textContent.includes("sampled overview"), "Large-score overview work is unbounded or sampling undisclosed");
  const panStart = studio.performance.now();
  timeline.setView(5000, 10, 10000);
  await tick();
  const panMs = studio.performance.now() - panStart;
  require(Number($("piano-roll").dataset.visibleNotes) === 11, "Actual editor did not limit the visible ten-thousand-note range");
  require(fullLoadMs < 3000 && panMs < 1000,
    `Ten-thousand-note interaction budget exceeded: load ${fullLoadMs.toFixed(1)} ms, pan ${panMs.toFixed(1)} ms`);
  checks.push(`actual 10,000-note view: load ${fullLoadMs.toFixed(1)} ms, pan ${panMs.toFixed(1)} ms; bounded rows/overview`);
  $("theme-toggle").click();
  const theme = doc.body.dataset.theme;
  require(studio.localStorage.getItem("transcribe.theme") === theme, "Theme setting did not persist");
  const focusStyle = studio.getComputedStyle($("timeline-scroll"));
  require(focusStyle.minHeight !== "0px", "Timeline input lost scalable control sizing");
  host({ type: "complete", success: true, notes, metadata, fingering: { notes: [] }, summary: "Final fixture" });
  timeline.setView(0, null, 4);
  for (const width of [1440, 1024, 720, 480]) {
    studio.frameElement.style.width = `${width}px`;
    await tick();
    require(doc.documentElement.scrollWidth <= width + 1, `Horizontal page overflow at ${width}px`);
  }
  studio.frameElement.style.width = "1400px";
  await tick();
  const panelStates = ["source", "inspector"].map(name =>
    $(`toggle-${name}`).getAttribute("aria-expanded") === "true");
  for (const name of ["source", "inspector"]) {
    if ($(`toggle-${name}`).getAttribute("aria-expanded") === "true") $(`toggle-${name}`).click();
  }
  await tick();
  require(doc.querySelector(".score-panel").getBoundingClientRect().width >= 1390,
    "Collapsing both side panels did not reclaim the workspace");
  ["source", "inspector"].forEach((name, index) => {
    if (panelStates[index]) $(`toggle-${name}`).click();
  });
  const contrast = (foreground, background) => {
    const luminance = (value) => {
      const components = value.trim().replace("#", "").match(/../g).map(hex => {
        const v = parseInt(hex, 16) / 255;
        return v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4;
      });
      return components[0] * .2126 + components[1] * .7152 + components[2] * .0722;
    };
    const values = [luminance(foreground), luminance(background)].sort((a, b) => a - b);
    return (values[1] + .05) / (values[0] + .05);
  };
  for (let i = 0; i < 2; i += 1) {
    const style = studio.getComputedStyle(doc.body);
    for (const token of ["--text", "--muted", "--mint"]) {
      require(contrast(style.getPropertyValue(token), style.getPropertyValue("--panel")) >= 4.5,
        `Insufficient ${token} text contrast in ${doc.body.dataset.theme} theme`);
    }
    $("theme-toggle").click();
  }
  checks.push("480/720/1024/1440px layouts without page overflow and 4.5:1 core text-token contrast in both themes");
  require(studio.__errors.length === 0, `Browser errors: ${studio.__errors.join("; ")}`);
  return checks;
};
