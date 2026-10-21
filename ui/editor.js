(() => {
  const $ = (id) => document.getElementById(id);
  const semitoneNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  const tuningPitches = {
    standard: [64, 59, 55, 50, 45, 40],
    "drop-d": [64, 59, 55, 50, 45, 38],
    dadgad: [62, 57, 55, 50, 45, 38],
    "open-d": [62, 57, 54, 50, 45, 38],
    "open-g": [62, 59, 55, 50, 43, 38],
  };
  const tuningLabels = {
    standard: "Standard E",
    "drop-d": "Drop D",
    dadgad: "DADGAD",
    "open-d": "Open D",
    "open-g": "Open G",
    custom: "Project tuning",
  };
  let notes = [];
  let tabPositions = [];
  let selectedIndex = -1;
  let duration = 1;
  let audioStart = 0;
  let audioEnd = 0;
  let waveform = [];
  let spectrum = [];
  let spectrumBands = 40;
  let history = [];
  let redoHistory = [];
  let dragState = null;
  let exportReady = false;
  let edited = false;
  let transcriptionBusy = false;
  let liveCaptureActive = false;
  let editBaselineNotes = new Map();
  let editBaselineFingerings = new Map();
  let pendingLiveDeletes = new Set();
  let playbackMode = "";
  let pausedMode = "";
  let pausedAt = 0;
  let playbackStarted = 0;
  let playbackEnd = 0;
  let playbackFrame = 0;
  let audioContext = null;
  let activeOscillators = [];
  let currentMetadata = {};
  let currentProjectSettings = null;
  let currentTuningOverride = null;
  let projectActive = false;
  let scoreId = "";
  let scoreRevision = 0;
  let nextUserNoteId = 1;
  let rejectedFingeringCorrectionCount = 0;
  let fingeringSearchTruncated = false;
  let reviewEnabled = false;
  let reviewByNote = new Map();
  let prioritizedReviewNotes = [];
  let reviewDecisions = [];
  let dismissedReviewSuggestions = new Set();
  const contextControlIds = [
    "tempo-control", "tempo-auto", "key-control", "minor-control", "meter-control",
    "pickup-control", "written-transposition-control", "guitar-tuning", "capo-fret",
  ];
  let contextBeforeLastChange = null;

  function send(action, values = {}) {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(`${action}|${new URLSearchParams(values).toString()}`);
    }
  }

  function noteName(midi) {
    return `${semitoneNames[(midi % 12 + 12) % 12]}${Math.floor(midi / 12) - 1}`;
  }

  function currentTuning() {
    return currentTuningOverride ||
      tuningPitches[$("guitar-tuning").value] || tuningPitches.standard;
  }

  function currentCapo() {
    return Math.max(0, Math.min(24, Number($("capo-fret").value) || 0));
  }

  function totalDuration() {
    const noteEnd = notes.reduce((end, note) =>
      Math.max(end, note.start + note.duration), 1);
    return Math.max(noteEnd, audioEnd, 1);
  }

  function fitCanvas(canvas) {
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    const ratio = window.devicePixelRatio || 1;
    const width = Math.round(rect.width * ratio);
    const height = Math.round(rect.height * ratio);
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const context = canvas.getContext("2d");
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    return { context, width: rect.width, height: rect.height };
  }

  function canvasColor(name, fallback) {
    return getComputedStyle(document.body).getPropertyValue(name).trim() || fallback;
  }

  function staffPosition(midi, baseY, step) {
    const pitchClasses = [0, 2, 4, 5, 7, 9, 11];
    const pitchClass = (midi % 12 + 12) % 12;
    const octave = Math.floor(midi / 12) - 1;
    let naturalDegree = pitchClasses.findIndex((pitch) => pitch === pitchClass);
    if (naturalDegree < 0) {
      const upperDegree = pitchClasses.findIndex((pitch) => pitch > pitchClass);
      naturalDegree = Math.max(0, upperDegree - 1);
    }
    const degree = naturalDegree >= 0
      ? octave * 7 + naturalDegree
      : octave * 7 + pitchClasses.findIndex((pitch) => pitch > pitchClass);
    const naturalMidi = (Math.floor(degree / 7) + 1) * 12 +
      pitchClasses[(degree % 7 + 7) % 7];
    return { y: baseY - (degree - 30) * step, accidental: naturalMidi !== midi };
  }

  function naturalPitch(degree) {
    const pitchClasses = [0, 2, 4, 5, 7, 9, 11];
    const octave = Math.floor(degree / 7);
    return (octave + 1) * 12 + pitchClasses[(degree % 7 + 7) % 7];
  }

  function staffMidiAtY(y, height) {
    const step = 5.5;
    const baseY = height / 2 + 22;
    return naturalPitch(30 + Math.round((baseY - y) / step));
  }

  function noteX(note, width) {
    const margin = 36;
    return margin + note.start / totalDuration() * Math.max(1, width - margin - 12);
  }

  function drawPlayhead(context, x, height) {
    if (!playbackMode) return;
    const margin = 36;
    const point = margin + playbackTime() / totalDuration() * Math.max(1, x - margin - 12);
    context.save();
    context.strokeStyle = "#ff8e78";
    context.globalAlpha = 0.86;
    context.lineWidth = 1.5;
    context.beginPath();
    context.moveTo(point, 0);
    context.lineTo(point, height);
    context.stroke();
    context.restore();
  }

  function playbackTime() {
    if (playbackMode) {
      return Math.max(0, (performance.now() - playbackStarted) / 1000);
    }
    return pausedMode ? pausedAt : 0;
  }

  function drawStaff() {
    const canvas = $("staff-view");
    const widestRange = notes.length
      ? Math.max(...notes.map((note) => note.midi)) - Math.min(...notes.map((note) => note.midi))
      : 0;
    canvas.style.height = `${Math.max(190, Math.min(380, 150 + widestRange * 2.2))}px`;
    const frame = fitCanvas(canvas);
    if (!frame) return;
    const { context, width, height } = frame;
    const background = canvasColor("--field", "#0c1319");
    const muted = canvasColor("--muted", "#81919b");
    context.clearRect(0, 0, width, height);
    context.fillStyle = background;
    context.fillRect(0, 0, width, height);
    const margin = 36;
    const lineGap = 11;
    const step = lineGap / 2;
    const baseY = height / 2 + 22;
    context.strokeStyle = "rgba(123, 148, 157, .38)";
    context.lineWidth = 1;
    for (let line = 0; line <= 4; line += 1) {
      const y = baseY - line * lineGap;
      context.beginPath();
      context.moveTo(margin, y + .5);
      context.lineTo(width - 10, y + .5);
      context.stroke();
    }
    context.fillStyle = muted;
    context.font = '10px "DM Mono", Consolas, monospace';
    context.fillText("G", 12, baseY - lineGap * 2 + 4);
    if (!notes.length) {
      context.fillStyle = "#6e8089";
      context.font = "11px Segoe UI, sans-serif";
      context.fillText("Detected notes will be placed on the staff as they stabilize.", margin, height / 2);
      return;
    }
    const colors = ["#74e4bd", "#a994ff", "#e9c578", "#6ccfe0", "#ff9b8e", "#96b5ff"];
    notes.forEach((note, index) => {
      const x = noteX(note, width);
      const { y, accidental } = staffPosition(note.midi, baseY, step);
      const radius = index === selectedIndex ? 6 : 5;
      const selected = index === selectedIndex;
      const stateColor = note.user_authored || note.state === "manually_corrected"
        ? "#e9c578" : note.uncertain ? "#ff9b8e"
          : note.state === "provisional" ? "#96b5ff"
            : note.state === "stabilized" ? "#74e4bd"
              : colors[index % colors.length];
      context.save();
      context.strokeStyle = "rgba(207, 223, 225, .4)";
      const diatonic = 30 + Math.round((baseY - y) / step);
      for (let ledger = 4; ledger <= 62; ledger += 2) {
        if ((ledger < 30 && ledger >= diatonic) ||
            (ledger > 38 && ledger <= diatonic)) {
          const ledgerY = baseY - (ledger - 30) * step;
          context.beginPath();
          context.moveTo(x - 8, ledgerY);
          context.lineTo(x + 8, ledgerY);
          context.stroke();
        }
      }
      context.fillStyle = selected ? "#fff2bd" : stateColor;
      context.globalAlpha = note.state === "provisional" ? .68 : .88;
      if (note.state === "provisional") context.setLineDash([2, 2]);
      context.beginPath();
      context.ellipse(x, y, radius, radius * .72, -.28, 0, Math.PI * 2);
      context.globalAlpha = 1;
      context.strokeStyle = selected ? "#fff2bd" : stateColor;
      const beats = note.duration * (Number($("tempo-control").value) || 120) / 60;
      if (beats < 1.8) context.fill();
      else context.stroke();
      if (beats < 3.8) {
        const stemUp = y >= baseY - lineGap * 2;
        const stemX = stemUp ? x + radius - 1 : x - radius + 1;
        const stemEnd = y + (stemUp ? -25 : 25);
        context.beginPath();
        context.moveTo(stemX, y);
        context.lineTo(stemX, stemEnd);
        context.stroke();
        if (beats < .55) {
          context.beginPath();
          context.moveTo(stemX, stemEnd);
          context.quadraticCurveTo(stemX + 12, stemEnd + 4, stemX + 6, stemEnd + 11);
          context.stroke();
        }
      }
      if (accidental) {
        context.fillStyle = "#d4dee0";
        context.font = '10px "DM Mono", Consolas, monospace';
        context.fillText("#", x - 12, y + 4);
      }
      if (selected) {
        context.fillStyle = "#f2f6f6";
        context.font = '8px "DM Mono", Consolas, monospace';
        context.fillText(noteName(note.midi), x - 12, y - 10);
      }
      context.setLineDash([]);
      context.restore();
    });
    context.fillStyle = muted;
    context.font = '8px "DM Mono", Consolas, monospace';
    context.fillText("0s", margin, height - 8);
    context.fillText(`${totalDuration().toFixed(1)}s`, Math.max(margin, width - 44), height - 8);
    drawPlayhead(context, width, height);
  }

  function assignTabPositions() {
    const tuning = currentTuning();
    const capo = currentCapo();
    const active = [];
    const previous = tabPositions;
    tabPositions = new Array(notes.length);
    const ordered = notes.map((note, noteIndex) => ({ note, noteIndex }))
      .sort((left, right) => left.note.start - right.note.start);
    ordered.forEach(({ note, noteIndex }) => {
      for (let index = active.length - 1; index >= 0; index -= 1) {
        if (active[index].end <= note.start) active.splice(index, 1);
      }
      const candidates = tuning.map((open, stringIndex) => ({
        string: stringIndex + 1,
        fret: note.midi - open - capo,
      })).filter((position) => position.fret >= 0 && position.fret <= 24 &&
        !active.some((item) => item.string === position.string));
      candidates.sort((left, right) => left.fret - right.fret);
      const prior = previous[noteIndex];
      const priorIsValid = prior && prior.manual &&
        prior.string > 0 && prior.fret >= 0 && prior.fret <= 24 &&
        tuning[prior.string - 1] + capo + prior.fret === note.midi &&
        !active.some((item) => item.string === prior.string);
      const position = priorIsValid
        ? { string: prior.string, fret: prior.fret, manual: true }
        : candidates[0] || { string: 0, fret: -1, manual: false };
      if (position.string) {
        active.push({ string: position.string, end: note.start + note.duration });
      }
      tabPositions[noteIndex] = {
        noteIndex, string: position.string, fret: position.fret,
        manual: Boolean(position.manual),
        correctionRejected: Boolean(prior && prior.manual && !priorIsValid),
        candidates: priorIsValid ? prior.candidates || [] : [],
      };
    });
  }

  function drawTab() {
    const canvas = $("tab-view");
    const frame = fitCanvas(canvas);
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    const margin = 36;
    const top = 20;
    const gap = Math.min(15, (height - 38) / 5);
    context.strokeStyle = "rgba(123, 148, 157, .42)";
    context.lineWidth = 1;
    for (let string = 0; string < 6; string += 1) {
      const y = top + string * gap;
      context.beginPath();
      context.moveTo(margin, y + .5);
      context.lineTo(width - 10, y + .5);
      context.stroke();
      context.fillStyle = "#80919a";
      context.font = '8px "DM Mono", Consolas, monospace';
      context.fillText(String(string + 1), 14, y + 3);
    }
    const tuning = currentTuning();
    $("tab-tuning-label").textContent =
      `${tuningLabels[$("guitar-tuning").value] || tuningLabels.standard} · capo ${currentCapo()}`;
    const colors = ["#74e4bd", "#a994ff", "#e9c578", "#6ccfe0", "#ff9b8e", "#96b5ff"];
    notes.forEach((note, index) => {
      const position = tabPositions[index] || { string: 0, fret: -1 };
      if (!position.string) return;
      const y = top + (position.string - 1) * gap;
      const x = noteX(note, width);
      context.fillStyle = index === selectedIndex ? "#fff2bd" : colors[index % colors.length];
      context.font = index === selectedIndex
        ? 'bold 10px "DM Mono", Consolas, monospace'
        : '9px "DM Mono", Consolas, monospace';
      context.textAlign = "center";
      context.fillText(String(position.fret), x, y + 4);
      context.textAlign = "left";
    });
    context.fillStyle = "#768992";
    context.font = '8px "DM Mono", Consolas, monospace';
    context.fillText("0s", margin, height - 8);
    context.fillText(`${totalDuration().toFixed(1)}s`, Math.max(margin, width - 44), height - 8);
    drawPlayhead(context, width, height);
  }

  function drawWaveform() {
    const frame = fitCanvas($("waveform"));
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    context.strokeStyle = "rgba(116, 228, 189, .76)";
    context.lineWidth = 1;
    context.beginPath();
    const middle = height / 2;
    if (waveform.length) {
      waveform.forEach((peak, index) => {
        const x = index * width / Math.max(1, waveform.length - 1);
        const amplitude = Math.max(1, peak * height * .44);
        context.moveTo(x, middle - amplitude);
        context.lineTo(x, middle + amplitude);
      });
    } else {
      context.moveTo(0, middle);
      context.lineTo(width, middle);
    }
    context.stroke();
  }

  function drawSpectrogram() {
    const frame = fitCanvas($("spectrogram"));
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    if (!spectrum.length || !spectrumBands) return;
    const columns = spectrum.length / spectrumBands;
    const cellWidth = width / Math.max(1, columns);
    const cellHeight = height / spectrumBands;
    for (let column = 0; column < columns; column += 1) {
      for (let band = 0; band < spectrumBands; band += 1) {
        const strength = spectrum[column * spectrumBands + band];
        if (strength < .05) continue;
        context.fillStyle = `rgba(${Math.round(36 + strength * 80)},${Math.round(93 + strength * 162)},${Math.round(115 + strength * 97)},${Math.min(.95, .18 + strength * .82)})`;
        context.fillRect(column * cellWidth, height - (band + 1) * cellHeight,
          Math.ceil(cellWidth), Math.ceil(cellHeight));
      }
    }
  }

  function drawConfidence() {
    const frame = fitCanvas($("confidence-heatmap"));
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    if (!notes.length) return;
    const scale = Math.max(1, width - 8) / totalDuration();
    notes.forEach((note) => {
      const x = 4 + note.start * scale;
      const w = Math.max(2, note.duration * scale);
      if (!Number.isFinite(note.calibrated_confidence)) {
        context.fillStyle = "rgba(120, 137, 145, .38)";
        context.fillRect(x, height * .25, w, height * .5);
        return;
      }
      const value = Math.max(0, Math.min(1, note.calibrated_confidence));
      const hue = 4 + value * 150;
      context.fillStyle = `hsla(${hue}, 68%, 58%, ${.35 + value * .65})`;
      context.fillRect(x, height * .25, w, height * .5);
    });
  }

  function drawAll() {
    drawStaff();
    drawTab();
    drawWaveform();
    drawSpectrogram();
    drawConfidence();
  }

  function computeTabPosition(note, noteIndex) {
    const tuning = currentTuning();
    const capo = currentCapo();
    let best = null;
    tuning.forEach((open, index) => {
      const fret = note.midi - open - capo;
      if (fret < 0 || fret > 24) return;
      const score = fret + Math.abs(index - 2) * .1;
      if (!best || score < best.score) best = { noteIndex, string: index + 1, fret, score };
    });
    return best
      ? { ...best, manual: false, candidates: [] }
      : { noteIndex, string: 0, fret: -1, manual: false, candidates: [] };
  }

  function readScoreContext() {
    const context = Object.fromEntries(contextControlIds.map((id) => [
      id,
      $(id).type === "checkbox" ? $(id).checked : $(id).value,
    ]));
    context._customTuning = currentTuningOverride
      ? [...currentTuningOverride] : null;
    return context;
  }

  function saveSnapshot(target = history, source = notes, settings = readScoreContext()) {
    target.push({
      notes: source.map((note) => ({ ...note })),
      positions: tabPositions.map((position) => ({ ...position })),
      settings: { ...settings },
      reviewDecisions: reviewDecisions.map((decision) => ({ ...decision })),
      dismissedReviewSuggestions: [...dismissedReviewSuggestions],
    });
    if (target.length > 100) target.shift();
  }

  function updateHistoryButtons() {
    $("undo-button").disabled = history.length === 0;
    $("redo-button").disabled = redoHistory.length === 0;
  }

  function markEdited() {
    edited = true;
    $("export-edits").textContent = "Apply edits & export";
    $("export-edits").disabled = !exportReady;
    const current = new Map(notes.filter((note) => note.id)
      .map((note) => [note.id, note]));
    const changes = [];
    editBaselineNotes.forEach((previous, id) => {
      if (!current.has(id)) changes.push({ kind: "delete", note_id: id });
    });
    current.forEach((note, id) => {
      const previous = editBaselineNotes.get(id);
      if (!previous || previous.midi !== note.midi ||
          Math.abs(previous.start - note.start) > .0001 ||
          Math.abs(previous.duration - note.duration) > .0001) {
        changes.push({
          kind: previous || !id.startsWith("user-") ? "update" : "add",
          note_id: id,
          midi: note.midi,
          start: note.start,
          duration: note.duration,
        });
      }
    });
    const currentFingerings = new Map();
    notes.forEach((note, index) => {
      const position = tabPositions[index];
      if (note.id && position?.manual && position.string > 0 && position.fret >= 0) {
        currentFingerings.set(note.id, `${position.string}:${position.fret}`);
      }
    });
    currentFingerings.forEach((value, id) => {
      if (editBaselineFingerings.get(id) === value) return;
      const [string, fret] = value.split(":").map(Number);
      changes.push({ kind: "fingering", note_id: id, string, fret });
    });
    editBaselineNotes = new Map([...current].map(([id, note]) =>
      [id, { midi: note.midi, start: note.start, duration: note.duration }]));
    editBaselineFingerings = currentFingerings;
    window.dispatchEvent(new CustomEvent("transcribe:edited", {
      detail: { notes, changes },
    }));
  }

  function updateMeasureDisplay(time = selectedIndex >= 0
    ? notes[selectedIndex].start : playbackTime()) {
    const tempo = Math.max(1, Number($("tempo-control").value) || 120);
    const [numerator, denominator] = ($("meter-control").value === "auto"
      ? `${currentMetadata.time_numerator || 4}/${currentMetadata.time_denominator || 4}`
      : $("meter-control").value).split("/").map(Number);
    const barQuarters = Math.max(.25, numerator * 4 / denominator);
    const pickup = Math.max(0, Number($("pickup-control").value) || 0);
    const quarters = Math.max(0, time * tempo / 60);
    const measure = pickup > 0 && quarters < pickup
      ? 1
      : Math.floor(Math.max(0, quarters - pickup) / barQuarters) +
        (pickup > 0 ? 2 : 1);
    $("measure-display").textContent = `Measure ${measure}`;
    return measure;
  }

  function updateContextSummary() {
    const tempo = Number($("tempo-control").value);
    $("tempo-value").textContent = $("tempo-auto").checked
      ? (currentMetadata.tempo_inferred
        ? `${Math.round(currentMetadata.tempo)} bpm` : "—")
      : Number.isFinite(tempo) ? `${tempo} bpm` : "—";
    const key = $("key-control");
    $("key-value").textContent = key.value === "auto"
      ? (currentMetadata.key || "—")
      : `${key.selectedOptions[0].textContent}${$("minor-control").checked ? " minor" : " major"}`;
    updateMeasureDisplay();
  }

  function navigateMeasure(direction) {
    if (!notes.length) return;
    const currentMeasure = updateMeasureDisplay();
    const nextMeasure = Math.max(1, currentMeasure + direction);
    const tempo = Math.max(1, Number($("tempo-control").value) || 120);
    const [numerator, denominator] = ($("meter-control").value === "auto"
      ? `${currentMetadata.time_numerator || 4}/${currentMetadata.time_denominator || 4}`
      : $("meter-control").value).split("/").map(Number);
    const barQuarters = Math.max(.25, numerator * 4 / denominator);
    const pickup = Math.max(0, Number($("pickup-control").value) || 0);
    const startQuarters = nextMeasure === 1 ? 0
      : pickup + (nextMeasure - (pickup > 0 ? 2 : 1)) * barQuarters;
    const endQuarters = nextMeasure === 1 && pickup > 0
      ? pickup : startQuarters + barQuarters;
    const noteIndex = notes.findIndex((note) => {
      const quarters = note.start * tempo / 60;
      return quarters >= startQuarters && quarters < endQuarters;
    });
    const fallback = notes.reduce((best, note, index) =>
      Math.abs(note.start * tempo / 60 - startQuarters) <
        Math.abs(notes[best].start * tempo / 60 - startQuarters)
        ? index : best, 0);
    chooseNote(noteIndex >= 0 ? noteIndex : fallback);
    $("measure-display").textContent = `Measure ${nextMeasure}`;
  }

  function renderSelected() {
    const note = notes[selectedIndex];
    document.querySelectorAll(".note-row").forEach((row) => {
      row.classList.toggle("selected", Number(row.dataset.index) === selectedIndex);
    });
    const fields = ["edit-pitch", "edit-start", "edit-duration", "edit-string", "edit-fret"];
    const editDisabled = transcriptionBusy && !liveCaptureActive;
    fields.forEach((id) => { $(id).disabled = !note || editDisabled; });
    $("apply-note-edit").disabled = !note || editDisabled;
    $("delete-note").disabled = !note || editDisabled;
    if (!note) {
      $("selection-label").textContent = "No selection";
      $("edit-instructions").textContent = "Click a note to inspect it";
      $("detected-pitch").textContent = "No note selected";
      $("detected-source").textContent =
        "Select a note in notation, tablature, or the timeline.";
      $("detected-onset").textContent = "—";
      $("detected-offset").textContent = "—";
      $("detected-confidence").textContent = "—";
      $("detected-evidence").textContent = "—";
      $("alternatives-list").textContent =
        "No alternate pitch hypotheses were supplied by the active backend.";
      $("alternative-note").textContent = "Select a note";
      $("fingering-note").textContent = "Select a note";
      $("fingering-alternatives").textContent =
        [
          fingeringSearchTruncated
            ? "Fingering search bound reached; review suggestions."
            : "",
          rejectedFingeringCorrectionCount
            ? `${rejectedFingeringCorrectionCount} saved fingering correction(s) could not be matched.`
            : "",
        ].filter(Boolean).join(" ") ||
        "Ranked, pitch-preserving fingering suggestions appear for the selected note.";
      renderReview(null);
      return;
    }
    const position = tabPositions[selectedIndex] || computeTabPosition(note, selectedIndex);
    $("selection-label").textContent = `${noteName(note.midi)} · ${note.start.toFixed(2)}s`;
    $("detected-pitch").textContent = `${noteName(note.midi)} · MIDI ${note.midi}`;
    const detectedPitch = Number.isFinite(note.detected_midi)
      ? `Detected ${noteName(Math.round(note.detected_midi))} · MIDI ${note.detected_midi.toFixed(2)}`
      : note.user_authored ? "User-authored · no detector evidence" : "Detected pitch unavailable";
    const detectorSource = [note.source_model, note.detection_method, note.evidence_kind]
      .filter(Boolean).join(" · ");
    $("detected-source").textContent = detectorSource
      ? `${detectedPitch} · ${detectorSource}` : detectedPitch;
    if (note.state) {
      const lifecycle = String(note.state).replaceAll("_", " ");
      $("detected-source").textContent += note.uncertain
        ? ` · UNCERTAIN (${lifecycle}; onset crosses an audio gap)`
        : ` · ${lifecycle}`;
    }
    $("detected-onset").textContent = Number.isFinite(note.measured_start)
      ? `${note.measured_start.toFixed(3)} s` : "Not measured";
    $("detected-offset").textContent = Number.isFinite(note.measured_end)
      ? `${note.measured_end.toFixed(3)} s` : "Not measured";
    $("detected-confidence").textContent = Number.isFinite(note.calibrated_confidence)
      ? `${Math.round(Math.max(0, Math.min(1, note.calibrated_confidence)) * 100)}%`
      : "Not provided";
    const evidence = Array.isArray(note.raw_evidence) ? note.raw_evidence : [];
    const evidenceCount = Number.isInteger(note.raw_evidence_count)
      ? note.raw_evidence_count : evidence.length;
    const strengths = evidence.map((item) => item.raw_strength)
      .filter(Number.isFinite);
    const meanStrength = strengths.length
      ? strengths.reduce((sum, value) => sum + value, 0) / strengths.length
      : null;
    $("detected-evidence").textContent = evidenceCount
      ? `${evidenceCount} samples${evidenceCount > evidence.length ? ` · ${evidence.length} shown` : ""}${meanStrength === null ? "" : ` · mean detector strength ${meanStrength.toFixed(3)} · not a probability`}`
      : Number.isFinite(note.raw_strength)
        ? `Raw backend strength ${note.raw_strength.toFixed(3)} · not a probability`
        : "No raw evidence";
    $("edit-instructions").textContent = Number.isFinite(note.calibrated_confidence)
      ? `Calibrated correctness estimate ${Math.round(note.calibrated_confidence * 100)}%`
      : "No calibrated correctness estimate is available";
    $("edit-pitch").value = note.midi;
    $("edit-start").value = note.start.toFixed(2);
    $("edit-duration").value = note.duration.toFixed(2);
    $("edit-string").value = position.string || "1";
    $("edit-fret").value = Math.max(0, position.fret);
    $("alternative-note").textContent = noteName(note.midi);
    const container = $("alternatives-list");
    container.replaceChildren();
    const alternatives = Array.isArray(note.pitch_alternatives)
      ? note.pitch_alternatives : [];
    if (!alternatives.length) {
      container.textContent = note.user_authored
        ? "User-authored pitch; no detector alternatives are available."
        : "No alternate pitch hypotheses were supplied by the active backend.";
    }
    alternatives.forEach((candidate) => {
      if (!Number.isInteger(candidate.midi)) return;
      const suggestion = document.createElement("button");
      suggestion.type = "button";
      suggestion.className = "alternative-pitch";
      const rawValue = Number(candidate.raw_value);
      suggestion.textContent = `${noteName(candidate.midi)} · MIDI ${candidate.midi}${Number.isFinite(rawValue) ? ` · raw ${rawValue.toFixed(3)}` : ""}`;
      suggestion.title = "Alternative raw backend activation. It is not a probability of correctness.";
      suggestion.addEventListener("click", () => {
        saveSnapshot();
        notes[selectedIndex].midi = candidate.midi;
        notes[selectedIndex].name = noteName(candidate.midi);
        notes[selectedIndex].user_authored = true;
        notes[selectedIndex].state = "manually_corrected";
        tabPositions[selectedIndex] = computeTabPosition(notes[selectedIndex], selectedIndex);
        renderSelected();
        drawAll();
        markEdited();
        updateHistoryButtons();
      });
      container.appendChild(suggestion);
    });
    const fingeringContainer = $("fingering-alternatives");
    fingeringContainer.replaceChildren();
    $("fingering-note").textContent = noteName(note.midi);
    if (fingeringSearchTruncated) {
      const warning = document.createElement("span");
      warning.textContent = "Fingering search bound reached; review these suggestions.";
      fingeringContainer.appendChild(warning);
    }
    if (rejectedFingeringCorrectionCount > 0) {
      const warning = document.createElement("span");
      warning.textContent =
        `${rejectedFingeringCorrectionCount} saved fingering correction(s) could not be matched.`;
      fingeringContainer.appendChild(warning);
    }
    if (position.correctionRejected) {
      const warning = document.createElement("span");
      warning.textContent = "Saved fingering no longer matches this note or tuning; choose a replacement.";
      fingeringContainer.appendChild(warning);
    } else if (position.ambiguous) {
      const warning = document.createElement("span");
      warning.textContent = "Several positions have similar playability costs.";
      fingeringContainer.appendChild(warning);
    }
    if (!position.string) {
      const warning = document.createElement("span");
      warning.textContent =
        "No playable string/fret assignment is available under the current tuning and capo.";
      fingeringContainer.appendChild(warning);
    }
    (position.candidates || []).forEach((candidate) => {
      const suggestion = document.createElement("button");
      suggestion.type = "button";
      suggestion.className = "alternative-pitch";
      suggestion.textContent =
        `S${candidate.string} · F${candidate.fret} · ${candidate.sequenceCost.toFixed(1)}`;
      const costs = candidate.costComponents;
      suggestion.title = costs && Number.isFinite(costs.total)
        ? `Sequence cost: fret ${costs.fret_height.toFixed(2)}, preferred ${costs.preferred_position.toFixed(2)}, hand movement ${costs.hand_movement.toFixed(2)}, string preference ${costs.string_preference.toFixed(2)}, chord stretch ${costs.chord_stretch.toFixed(2)}, unassigned ${costs.unassigned_notes.toFixed(2)}. Choose without changing pitch.`
        : "Choose this fingering without changing the sounding pitch.";
      suggestion.addEventListener("click", () => {
        const open = currentTuning()[candidate.string - 1] + currentCapo();
        if (open + candidate.fret !== notes[selectedIndex].midi) return;
        saveSnapshot();
        redoHistory = [];
        tabPositions[selectedIndex] = {
          ...position,
          noteIndex: selectedIndex,
          string: candidate.string,
          fret: candidate.fret,
          manual: true,
          correctionRejected: false,
        };
        renderSelected();
        drawAll();
        markEdited();
        updateHistoryButtons();
      });
      fingeringContainer.appendChild(suggestion);
    });
    renderReview(note);
  }

  function renderReview(note) {
    const card = $("review-card");
    card.classList.toggle("hidden", !reviewEnabled);
    renderReviewQueue();
    const list = $("review-items");
    list.replaceChildren();
    $("audition-event").disabled = !reviewEnabled || !note ||
      liveCaptureActive ||
      !/\.wav$/i.test($("input-path").value);
    if (!reviewEnabled) {
      $("review-status").textContent = "Disabled";
      list.textContent = "Enable Evidence review before transcription.";
      return;
    }
    if (!note) {
      $("review-status").textContent = "Select a note";
      list.textContent = "Select a note to inspect review findings.";
      return;
    }
    const review = reviewByNote.get(note.id);
    if (!review) {
      $("review-status").textContent = "No review data";
      list.textContent = "Run an offline transcription with evidence review enabled.";
      return;
    }
    $("review-status").textContent =
      `${review.evidence_samples} raw evidence sample(s)`;
    const assessments = Array.isArray(review.assessments)
      ? review.assessments : [];
    assessments.forEach((assessment) => {
      const item = document.createElement("p");
      item.className = `review-assessment state-${assessment.state}`;
      item.textContent = `${assessment.dimension}: ${assessment.explanation}`;
      list.appendChild(item);
    });
    const findings = Array.isArray(review.findings) ? review.findings : [];
    findings.forEach((finding) => {
      const item = document.createElement("p");
      item.className = "review-finding";
      item.textContent =
        `${finding.category.replaceAll("-", " ")} · rank ${Number(finding.ranking_score).toFixed(2)}: ${finding.explanation}`;
      list.appendChild(item);
    });
    const suggestions = Array.isArray(review.suggestions)
      ? review.suggestions : [];
    const available = suggestions.filter((suggestion) =>
      !dismissedReviewSuggestions.has(suggestion.id));
    available.forEach((suggestion) => {
      const wrapper = document.createElement("div");
      wrapper.className = "review-suggestion";
      const detail = document.createElement("p");
      detail.textContent =
        `${suggestion.explanation} Rank ${Number(suggestion.ranking_score).toFixed(2)} (not a probability). ${suggestion.ranking_basis}`;
      wrapper.appendChild(detail);
      const controls = document.createElement("div");
      controls.className = "review-actions";
      const accept = document.createElement("button");
      accept.type = "button";
      accept.className = "button button-small";
      accept.textContent = suggestion.kind === "split-repeated-note"
        ? "Split note" : "Apply suggestion";
      accept.addEventListener("click", () => acceptReviewSuggestion(suggestion));
      const dismiss = document.createElement("button");
      dismiss.type = "button";
      dismiss.className = "button button-small button-quiet";
      dismiss.textContent = "Dismiss";
      dismiss.addEventListener("click", () => decideReviewSuggestion(
        suggestion, "rejected"));
      controls.append(accept, dismiss);
      wrapper.appendChild(controls);
      list.appendChild(wrapper);
    });
    if (!list.childElementCount) {
      list.textContent = "No ranked correction suggestions for this note.";
    }
  }

  function rebuildReviewQueue() {
    prioritizedReviewNotes = [];
    if (!reviewEnabled) return;
    notes.forEach((note, index) => {
      const review = reviewByNote.get(note.id);
      if (!review) return;
      const findings = Array.isArray(review.findings) ? review.findings : [];
      const suggestions = (Array.isArray(review.suggestions)
        ? review.suggestions : []).filter((suggestion) =>
        !dismissedReviewSuggestions.has(suggestion.id));
      const ambiguous = (Array.isArray(review.assessments)
        ? review.assessments : []).filter((assessment) =>
        assessment.state === "ambiguous");
      const ranked = findings.map((item) => ({
        score: Number(item.ranking_score),
        label: String(item.category || "finding").replaceAll("-", " "),
      })).concat(suggestions.map((item) => ({
        score: Number(item.ranking_score),
        label: String(item.kind || "suggestion").replaceAll("-", " "),
      }))).filter((item) => Number.isFinite(item.score));
      if (!ranked.length && !ambiguous.length) return;
      ranked.sort((left, right) => right.score - left.score);
      const hasRank = ranked.length > 0;
      prioritizedReviewNotes.push({
        index,
        note,
        score: hasRank ? ranked[0].score : null,
        sortScore: hasRank ? ranked[0].score : -1,
        label: hasRank ? ranked[0].label
          : `ambiguous ${ambiguous[0].dimension}`,
      });
    });
    prioritizedReviewNotes.sort((left, right) =>
      right.sortScore - left.sortScore ||
      Number(left.note.measured_start ?? left.note.start) -
        Number(right.note.measured_start ?? right.note.start));
  }

  function renderReviewQueue() {
    const queue = $("review-queue");
    queue.replaceChildren();
    if (!reviewEnabled) {
      queue.textContent = "Enable evidence review before transcription.";
      return;
    }
    if (!prioritizedReviewNotes.length) {
      queue.textContent = reviewByNote.size
        ? "No ranked findings need review."
        : "Run an offline transcription with evidence review enabled.";
      return;
    }
    prioritizedReviewNotes.slice(0, 12).forEach((entry) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "review-queue-item";
      button.setAttribute("aria-current",
        notes[selectedIndex]?.id === entry.note.id ? "true" : "false");
      const time = Number(entry.note.measured_start ?? entry.note.start);
      const rank = entry.score === null
        ? "unranked" : `rank ${entry.score.toFixed(2)}`;
      button.textContent =
        `${noteName(entry.note.midi)} · ${time.toFixed(2)} s · ` +
        `${entry.label} · ${rank}`;
      button.title = "Select this event to inspect its evidence and audition the source audio.";
      button.addEventListener("click", () => chooseNote(entry.index));
      queue.appendChild(button);
    });
  }

  function decideReviewSuggestion(suggestion, outcome) {
    if (dismissedReviewSuggestions.has(suggestion.id)) return;
    saveSnapshot();
    redoHistory = [];
    reviewDecisions.push({
      id: suggestion.id,
      note_id: suggestion.note_id,
      outcome,
    });
    dismissedReviewSuggestions.add(suggestion.id);
    rebuildReviewQueue();
    markEdited();
    renderReview(notes[selectedIndex] || null);
    updateHistoryButtons();
  }

  function acceptReviewSuggestion(suggestion) {
    const index = notes.findIndex((candidate) =>
      candidate.id === suggestion.note_id);
    if (index < 0 || dismissedReviewSuggestions.has(suggestion.id)) return;
    const note = notes[index];
    let scoreSplit = null;
    if (suggestion.kind === "split-repeated-note") {
      const measuredDuration = Number(note.measured_end) -
        Number(note.measured_start);
      const measuredSplit = Number(suggestion.split_time_seconds);
      if (!Number.isFinite(measuredSplit) || measuredDuration <= 0 ||
          measuredSplit <= Number(note.measured_start) ||
          measuredSplit >= Number(note.measured_end)) {
        $("review-status").textContent = "Split suggestion no longer fits this event";
        return;
      }
      const fraction = (measuredSplit - Number(note.measured_start)) /
        measuredDuration;
      scoreSplit = note.start + note.duration * fraction;
    } else {
      const pitch = Number(suggestion.suggested_midi_pitch);
      if (!Number.isInteger(pitch) || pitch < 0 || pitch > 127) {
        $("review-status").textContent = "Suggestion pitch is outside MIDI range";
        return;
      }
    }
    saveSnapshot();
    redoHistory = [];
    if (suggestion.kind === "split-repeated-note") {
      const second = {
        ...note,
        id: `user-${scoreId || "score"}-${nextUserNoteId++}`,
        start: scoreSplit,
        duration: note.start + note.duration - scoreSplit,
        confidence: null,
        amplitude: null,
        detected_midi: null,
        raw_evidence: [],
        pitch_alternatives: [],
        source_event_ids: [],
        user_authored: true,
        state: "manually_corrected",
      };
      note.duration = scoreSplit - note.start;
      note.user_authored = true;
      note.state = "manually_corrected";
      notes.splice(index + 1, 0, second);
      tabPositions.splice(index + 1, 0, computeTabPosition(second, index + 1));
    } else {
      const pitch = Number(suggestion.suggested_midi_pitch);
      note.midi = pitch;
      note.name = noteName(pitch);
      note.user_authored = true;
      note.state = "manually_corrected";
      tabPositions[index] = computeTabPosition(note, index);
    }
    reviewDecisions.push({
      id: suggestion.id,
      note_id: suggestion.note_id,
      outcome: "accepted",
    });
    dismissedReviewSuggestions.add(suggestion.id);
    rebuildReviewQueue();
    selectedIndex = index;
    renderSelected();
    drawAll();
    markEdited();
    updateHistoryButtons();
  }

  function chooseNote(index) {
    selectedIndex = Number.isInteger(index) && index >= 0 && index < notes.length
      ? index : -1;
    window.dispatchEvent(new CustomEvent("transcribe:score-selection", {
      detail: { id: selectedIndex >= 0 ? notes[selectedIndex].id : "" },
    }));
    renderSelected();
    if (selectedIndex >= 0) updateMeasureDisplay(notes[selectedIndex].start);
    drawAll();
  }

  function scoreX(event, canvas) {
    const rect = canvas.getBoundingClientRect();
    return Math.max(0, Math.min(1,
      (event.clientX - rect.left - 36) / Math.max(1, rect.width - 48)));
  }

  function hitStaffNote(x, y, canvas) {
    const frame = canvas.getBoundingClientRect();
    const baseY = frame.height / 2 + 22;
    const step = 5.5;
    let best = -1;
    let bestDistance = 12;
    notes.forEach((note, index) => {
      const notePixelX = noteX(note, frame.width);
      const notePixelY = staffPosition(note.midi, baseY, step).y;
      const distance = Math.hypot(x - notePixelX, y - notePixelY);
      if (distance < bestDistance) {
        best = index;
        bestDistance = distance;
      }
    });
    return best;
  }

  function hitTabNote(x, y, canvas) {
    const rect = canvas.getBoundingClientRect();
    const top = 20;
    const gap = Math.min(15, (rect.height - 38) / 5);
    let best = -1;
    let bestDistance = 14;
    notes.forEach((note, index) => {
      const position = tabPositions[index];
      if (!position || !position.string) return;
      const distance = Math.hypot(
        x - noteX(note, rect.width),
        y - (top + (position.string - 1) * gap));
      if (distance < bestDistance) {
        best = index;
        bestDistance = distance;
      }
    });
    return best;
  }

  function beginDrag(event, canvas, type) {
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    const index = type === "staff"
      ? hitStaffNote(x, y, canvas) : hitTabNote(x, y, canvas);
    if (index < 0) {
      chooseNote(-1);
      return;
    }
    chooseNote(index);
    const note = notes[index];
    const endX = noteX(note, rect.width) +
      note.duration / totalDuration() * (rect.width - 48);
    dragState = {
      type, index, x, y, lastX: x, lastY: y, changed: false,
      resize: type === "staff" && Math.abs(x - endX) < 8,
      start: note.start, duration: note.duration, midi: note.midi,
      position: { ...tabPositions[index] },
    };
    canvas.setPointerCapture(event.pointerId);
  }

  function moveDrag(event, canvas) {
    if (!dragState) return;
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    const deltaX = x - dragState.x;
    const deltaY = y - dragState.y;
    if (!dragState.changed && Math.hypot(deltaX, deltaY) > 3) {
      saveSnapshot();
      redoHistory = [];
      dragState.changed = true;
    }
    if (!dragState.changed) return;
    const note = notes[dragState.index];
    const deltaTime = deltaX / Math.max(1, rect.width - 48) * totalDuration();
    if (dragState.resize) {
      note.duration = Math.max(.05, dragState.duration + deltaTime);
    } else {
      note.start = Math.max(0, dragState.start + deltaTime);
    }
    if (dragState.type === "staff" && Math.abs(deltaY) > 1) {
      note.midi = Math.max(0, Math.min(127, staffMidiAtY(y, rect.height)));
      note.name = noteName(note.midi);
      tabPositions[dragState.index] = computeTabPosition(note, dragState.index);
    } else if (dragState.type === "tab" && Math.abs(deltaY) > 3) {
      const gap = Math.min(15, (rect.height - 38) / 5);
      const string = Math.max(1, Math.min(6, Math.round((y - 20) / gap) + 1));
      const open = currentTuning()[string - 1] + currentCapo();
      const fret = note.midi - open;
      if (fret < 0 || fret > 24) {
        $("edit-instructions").textContent =
          "That string cannot play the selected pitch; choose another string.";
      } else {
        tabPositions[dragState.index] = {
          noteIndex: dragState.index, string, fret, manual: true,
        };
      }
    }
    renderSelected();
    drawAll();
  }

  function endDrag() {
    if (!dragState) return;
    const changed = dragState.changed;
    dragState = null;
    if (changed) {
      markEdited();
      updateHistoryButtons();
    }
  }

  function insertNote(event) {
    const canvas = $("staff-view");
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    saveSnapshot();
    redoHistory = [];
    const pitch = Math.max(36, Math.min(96, staffMidiAtY(y, rect.height)));
    const note = {
      id: `user-${scoreId || "score"}-${nextUserNoteId++}`,
      midi: pitch,
      name: noteName(pitch),
      start: scoreX(event, canvas) * Math.max(1, totalDuration()),
      duration: .5,
      confidence: null,
      amplitude: null,
      user_authored: true,
    };
    notes.push(note);
    tabPositions.push(computeTabPosition(note, notes.length - 1));
    chooseNote(notes.length - 1);
    markEdited();
    updateHistoryButtons();
  }

  function removeSelected() {
    if (selectedIndex < 0 || selectedIndex >= notes.length) return;
    saveSnapshot();
    redoHistory = [];
    if (liveCaptureActive && notes[selectedIndex].id) {
      pendingLiveDeletes.add(notes[selectedIndex].id);
    }
    notes.splice(selectedIndex, 1);
    tabPositions.splice(selectedIndex, 1);
    selectedIndex = -1;
    renderSelected();
    drawAll();
    markEdited();
    updateHistoryButtons();
  }

  function applySelectedEdit() {
    if (selectedIndex < 0) return;
    const midi = Number($("edit-pitch").value);
    const start = Number($("edit-start").value);
    const noteDuration = Number($("edit-duration").value);
    const string = Number($("edit-string").value);
    const fret = Number($("edit-fret").value);
    if (!Number.isInteger(midi) || midi < 0 || midi > 127 ||
        !Number.isFinite(start) || start < 0 ||
        !Number.isFinite(noteDuration) || noteDuration < .05 ||
        !Number.isInteger(string) || string < 1 || string > 6 ||
        !Number.isInteger(fret) || fret < 0 || fret > 24) {
      $("edit-instructions").textContent = "Check pitch, time, duration, string, and fret ranges";
      return;
    }
    const note = notes[selectedIndex];
    const previousMidi = note.midi;
    const pitchChanged = midi !== previousMidi;
    const previousPosition = tabPositions[selectedIndex] || computeTabPosition(note, selectedIndex);
    const positionChanged =
      previousPosition.string !== string || previousPosition.fret !== fret;
    const selectedPositionPitch =
      currentTuning()[string - 1] + currentCapo() + fret;
    if (!pitchChanged && selectedPositionPitch !== note.midi) {
      $("edit-instructions").textContent =
        "String and fret must preserve the selected pitch. Change Pitch explicitly to retune.";
      return;
    }
    saveSnapshot();
    redoHistory = [];
    note.midi = pitchChanged ? midi : note.midi;
    note.name = noteName(note.midi);
    note.start = start;
    note.duration = noteDuration;
    note.user_authored = true;
    note.state = "manually_corrected";
    const expectedFret = note.midi - currentTuning()[string - 1] - currentCapo();
    if (expectedFret === fret && expectedFret >= 0) {
      tabPositions[selectedIndex] = {
        noteIndex: selectedIndex, string, fret,
        manual: positionChanged || (!pitchChanged && previousPosition.manual),
      };
    } else {
      tabPositions[selectedIndex] = computeTabPosition(note, selectedIndex);
    }
    renderSelected();
    drawAll();
    markEdited();
    updateHistoryButtons();
  }

  function restoreSnapshot(snapshot) {
    notes = snapshot.notes.map((note) => ({ ...note }));
    tabPositions = snapshot.positions.map((position) => ({ ...position }));
    reviewDecisions = (snapshot.reviewDecisions || [])
      .map((decision) => ({ ...decision }));
    dismissedReviewSuggestions = new Set(
      snapshot.dismissedReviewSuggestions || []);
    if (snapshot.settings) {
      contextControlIds.forEach((id) => {
        const control = $(id);
        if (control.type === "checkbox") control.checked = Boolean(snapshot.settings[id]);
        else control.value = snapshot.settings[id];
      });
      currentTuningOverride = Array.isArray(snapshot.settings._customTuning)
        ? [...snapshot.settings._customTuning] : null;
      contextBeforeLastChange = readScoreContext();
      refreshStringOptions();
      updateContextSummary();
    }
    if (reviewEnabled) rebuildReviewQueue();
    selectedIndex = Math.min(selectedIndex, notes.length - 1);
    renderSelected();
    drawAll();
    markEdited();
  }

  function undo() {
    if (!history.length) return;
    saveSnapshot(redoHistory);
    restoreSnapshot(history.pop());
    updateHistoryButtons();
  }

  function redo() {
    if (!redoHistory.length) return;
    saveSnapshot(history);
    restoreSnapshot(redoHistory.pop());
    updateHistoryButtons();
  }

  function acceptNotes(nextNotes) {
    if (!Array.isArray(nextNotes)) return;
    const oldCount = notes.length;
    const selectedId = selectedIndex >= 0 ? notes[selectedIndex]?.id : null;
    const previousNotes = new Map(notes.filter((note) => note.id)
      .map((note) => [note.id, note]));
    const previousPositions = new Map(notes.map((note, index) =>
      [note.id, tabPositions[index]]));
    const incomingIds = new Set(nextNotes.map((note) => note.id).filter(Boolean));
    for (const id of pendingLiveDeletes) {
      if (!incomingIds.has(id)) pendingLiveDeletes.delete(id);
    }
    notes = nextNotes.filter((note) => !pendingLiveDeletes.has(note.id)).map((incoming) => {
      const prior = previousNotes.get(incoming.id);
      const note = {
        ...incoming,
        amplitude: Number.isFinite(incoming.amplitude) ? incoming.amplitude : null,
      };
      if (prior?.user_authored && !incoming.user_authored) {
        note.midi = prior.midi;
        note.name = prior.name;
        note.start = prior.start;
        note.duration = prior.duration;
        note.user_authored = true;
        note.state = "manually_corrected";
      }
      return note;
    });
    tabPositions = notes.map((note) => {
      const prior = previousPositions.get(note.id);
      if (Number.isInteger(note.string) && Number.isInteger(note.fret)) {
        return {
          string: note.string,
          fret: note.fret,
          manual: Boolean(note.manual_fingering),
        };
      }
      return prior ? { ...prior } : {};
    });
    assignTabPositions();
    selectedIndex = selectedId
      ? notes.findIndex((note) =>
        note.id === selectedId ||
        note.source_event_ids?.includes(selectedId)) : -1;
    if (selectedIndex < 0 && !selectedId) selectedIndex = -1;
    if (notes.length !== oldCount && !edited) {
      history = [];
      redoHistory = [];
    }
    editBaselineNotes = new Map(notes.filter((note) => note.id).map((note) =>
      [note.id, { midi: note.midi, start: note.start, duration: note.duration }]));
    editBaselineFingerings = new Map(notes.flatMap((note, index) => {
      const position = tabPositions[index];
      return note.id && position?.manual && position.string > 0 &&
        position.fret >= 0
        ? [[note.id, `${position.string}:${position.fret}`]] : [];
    }));
    if (reviewEnabled) rebuildReviewQueue();
    duration = totalDuration();
    renderSelected();
    drawAll();
    $("export-edits").disabled = transcriptionBusy || !exportReady;
  }

  function acceptFingering(fingering) {
    if (!fingering || !Array.isArray(fingering.notes) ||
        fingering.notes.length !== notes.length) return;
    rejectedFingeringCorrectionCount =
      Array.isArray(fingering.rejected_corrections)
        ? fingering.rejected_corrections.length : 0;
    fingeringSearchTruncated = Boolean(fingering.search_truncated);
    tabPositions = fingering.notes.map((item, index) => ({
      noteIndex: index,
      string: item.string,
      fret: item.fret,
      manual: Boolean(item.manual),
      ambiguous: Boolean(item.ambiguous),
      correctionRejected: Boolean(item.correction_rejected),
      candidates: Array.isArray(item.candidates)
        ? item.candidates.map((candidate) => ({
          string: candidate.string,
          fret: candidate.fret,
          sequenceCost: Number(candidate.sequence_cost) || 0,
          costComponents: candidate.cost_components || {},
        }))
        : [],
    }));
    renderSelected();
    drawAll();
  }

  function acceptMetadata(metadata) {
    currentMetadata = metadata || {};
    if (currentMetadata.score_id) scoreId = currentMetadata.score_id;
    if (Number.isInteger(currentMetadata.score_revision)) {
      scoreRevision = currentMetadata.score_revision;
    }
    if (currentProjectSettings?.tempo_automatic) {
      $("tempo-auto").checked = true;
    } else if (currentMetadata.tempo_inferred) {
      $("tempo-auto").checked = false;
      $("tempo-control").value = Math.round(currentMetadata.tempo);
    }
    if (currentProjectSettings?.meter_automatic) {
      $("meter-control").value = "auto";
    } else if (currentMetadata.meter_inferred) {
      $("meter-control").value =
        `${currentMetadata.time_numerator}/${currentMetadata.time_denominator}`;
    }
    if (currentProjectSettings?.key_automatic) {
      $("key-control").value = "auto";
    } else if (currentMetadata.key_inferred) {
      $("key-control").value = String(currentMetadata.key_fifths);
      $("minor-control").checked = Boolean(currentMetadata.minor_key);
    }
    $("tempo-value").textContent = currentMetadata.tempo_inferred
      ? `${Math.round(currentMetadata.tempo)} bpm` : "—";
    $("key-value").textContent = currentMetadata.key || "—";
    $("tab-tuning-label").textContent =
      `${tuningLabels[$("guitar-tuning").value] || tuningLabels.standard} · capo ${currentCapo()}`;
    exportReady = notes.length > 0;
    $("play-midi").disabled = !exportReady;
    const fileMode = document.querySelector(".segment.selected")?.dataset.mode === "file";
    $("play-original").disabled =
      !exportReady || !fileMode || !/\.wav$/i.test($("input-path").value);
    edited = false;
    history = [];
    redoHistory = [];
    $("export-edits").disabled = !exportReady;
    $("export-edits").textContent = "Apply edits & export";
    contextBeforeLastChange = readScoreContext();
    updateContextSummary();
    refreshStringOptions();
    updateHistoryButtons();
    updateMeasureDisplay();
    drawAll();
  }

  function acceptAudioPreview(message) {
    const nextWaveform = Array.isArray(message.waveform) ? message.waveform : [];
    const nextSpectrum = Array.isArray(message.spectrogram) ? message.spectrogram : [];
    spectrumBands = Number(message.bands) || spectrumBands;
    if (message.replace) {
      waveform = nextWaveform.slice();
      spectrum = nextSpectrum.slice();
      audioStart = Number(message.offset) || 0;
      audioEnd = audioStart + (Number(message.duration) || 0);
    } else {
      const columns = Number(message.columns) || nextWaveform.length;
      const chunkDuration = Number(message.duration) || 0;
      const chunkOffset = Number(message.offset) || 0;
      const secondsPerColumn = columns > 0 ? chunkDuration / columns : 0;
      waveform.push(...nextWaveform);
      spectrum.push(...nextSpectrum);
      const maximumColumns = 512;
      if (waveform.length > maximumColumns) {
        const remove = waveform.length - maximumColumns;
        waveform.splice(0, remove);
        spectrum.splice(0, remove * spectrumBands);
        audioStart += remove * secondsPerColumn;
      }
      if (audioEnd === 0) audioStart = chunkOffset;
      audioEnd = Math.max(audioEnd, chunkOffset + chunkDuration);
      if (columns === 0) return;
    }
    $("audio-time").textContent = waveform.length
      ? `${audioStart.toFixed(1)}–${audioEnd.toFixed(1)}s` : "Waiting for audio";
    drawWaveform();
    drawSpectrogram();
    drawConfidence();
  }

  function drawPlaybackFrame() {
    if (!playbackMode) return;
    const time = playbackTime();
    window.dispatchEvent(new CustomEvent("transcribe:playback-time", {
      detail: time,
    }));
    updateMeasureDisplay(time);
    if (time >= playbackEnd) {
      if ($("loop-toggle").getAttribute("aria-pressed") === "true") {
        if (playbackMode === "midi") {
          playMidi().catch((error) => {
            $("status-text").textContent = `Note playback failed: ${error.message}`;
            stopPlayback();
          });
        } else {
          playOriginal();
        }
        return;
      }
      stopPlayback();
      return;
    }
    drawStaff();
    drawTab();
    playbackFrame = requestAnimationFrame(drawPlaybackFrame);
  }

  async function playMidi(fromTime = 0) {
    stopPlayback();
    const AudioContextType = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextType) {
      $("status-text").textContent = "Web Audio is unavailable for note playback";
      return;
    }
    if (!audioContext) audioContext = new AudioContextType();
    await audioContext.resume();
    const startAt = audioContext.currentTime + .08;
    playbackEnd = totalDuration();
    playbackMode = "midi";
    pausedMode = "";
    pausedAt = 0;
    playbackStarted = performance.now() - fromTime * 1000;
    activeOscillators = [];
    notes.forEach((note) => {
      const noteEnd = note.start + note.duration;
      if (noteEnd <= fromTime) return;
      const scheduledStart = startAt + Math.max(0, note.start - fromTime);
      const remaining = Math.max(.05, noteEnd - Math.max(fromTime, note.start));
      const oscillator = audioContext.createOscillator();
      const gain = audioContext.createGain();
      oscillator.type = "triangle";
      oscillator.frequency.value = 440 * Math.pow(2, (note.midi - 69) / 12);
      const end = scheduledStart + remaining;
      const amplitude = note.amplitude > 0
        ? Math.max(.025, Math.min(.2, note.amplitude * .2))
        : .11;
      gain.gain.setValueAtTime(.0001, scheduledStart);
      gain.gain.linearRampToValueAtTime(amplitude,
        Math.min(end, scheduledStart + .012));
      gain.gain.setTargetAtTime(.0001, end, .04);
      oscillator.connect(gain);
      gain.connect(audioContext.destination);
      oscillator.start(scheduledStart);
      oscillator.stop(end + .18);
      activeOscillators.push(oscillator);
    });
    $("stop-playback").disabled = false;
    $("play-midi").textContent = "Ⅱ";
    $("play-midi").title = "Pause score playback (Space)";
    drawPlaybackFrame();
  }

  function pauseMidi() {
    if (playbackMode !== "midi") return;
    pausedAt = playbackTime();
    pausedMode = "midi";
    activeOscillators.forEach((oscillator) => {
      try { oscillator.stop(); } catch { }
    });
    activeOscillators = [];
    playbackMode = "";
    cancelAnimationFrame(playbackFrame);
    window.dispatchEvent(new CustomEvent("transcribe:playback-time", {
      detail: pausedAt,
    }));
    $("play-midi").textContent = "▶";
    $("play-midi").title = "Resume score playback (Space)";
    $("stop-playback").disabled = false;
  }

  function toggleMidiPlayback() {
    if (playbackMode === "midi") {
      pauseMidi();
      return;
    }
    if (playbackMode) stopPlayback();
    const resumeAt = pausedMode === "midi" ? pausedAt : 0;
    pausedMode = "";
    playMidi(resumeAt).catch((error) => {
      $("status-text").textContent = `Note playback failed: ${error.message}`;
      stopPlayback();
    });
  }

  function playOriginal() {
    stopPlayback();
    playbackMode = "original";
    playbackStarted = performance.now();
    playbackEnd = Math.max(totalDuration(), audioEnd);
    $("stop-playback").disabled = false;
    $("play-midi").textContent = "▶";
    $("play-midi").title = "Play or pause the score (Space)";
    send("play-original");
    drawPlaybackFrame();
  }

  function stopPlayback() {
    if (playbackMode === "original") send("stop-playback");
    activeOscillators.forEach((oscillator) => {
      try { oscillator.stop(); } catch { }
    });
    activeOscillators = [];
    playbackMode = "";
    pausedMode = "";
    pausedAt = 0;
    $("play-midi").textContent = "▶";
    $("play-midi").title = "Play or pause the score (Space)";
    window.dispatchEvent(new CustomEvent("transcribe:playback-stop"));
    cancelAnimationFrame(playbackFrame);
    $("stop-playback").disabled = true;
    drawAll();
  }

  function exportEdits() {
    if (!exportReady) return;
    $("export-edits").disabled = true;
    window.dispatchEvent(new CustomEvent("transcribe:export-started"));
    send("export-edits", scoreWriteFields());
  }

  function scoreWriteFields(extra = {}) {
    const notesPayload = notes.map((note) => [
      note.id, note.midi, note.start, note.duration,
      Math.max(0, Math.min(1, note.confidence || 0)),
      Math.max(0, Math.min(1, note.amplitude || 0)),
    ].join(",")).join(";");
    const positionsPayload = tabPositions.map((position, index) =>
      position.manual && position.string > 0
        ? `${index},${position.string},${position.fret}` : ""
    ).filter(Boolean).join(";");
    return {
      notes: notesPayload,
      tab_positions: positionsPayload,
      tempo: $("tempo-control").value,
      tempo_auto: $("tempo-auto").checked ? "1" : "0",
      key_fifths: $("key-control").value,
      key_auto: $("key-control").value === "auto" ? "1" : "0",
      minor: $("minor-control").checked ? "1" : "0",
      meter: $("meter-control").value,
      meter_auto: $("meter-control").value === "auto" ? "1" : "0",
      pickup: $("pickup-control").value,
      written_transposition: $("written-transposition-control").value,
      base_revision: String(scoreRevision),
      review_decisions: reviewDecisions.map((decision) =>
        [decision.id, decision.note_id, decision.outcome].join(",")).join(";"),
      tuning: $("guitar-tuning").value,
      capo: String(currentCapo()),
      model: $("model-path").value,
      instrument: $("instrument-prior").value,
      quality: $("quality-mode").value,
      live_backend: $("live-backend").value,
      polyphonic: $("polyphonic").checked ? "1" : "0",
      preserve_timing: $("preserve-timing").checked ? "1" : "0",
      latency_ms: $("latency-ms").value,
      musicxml: $("musicxml-path").value,
      tab: $("tab-path").value,
      midi: $("midi-path").value,
      ...extra,
    };
  }

  $("staff-view").addEventListener("pointerdown", (event) => beginDrag(event, $("staff-view"), "staff"));
  $("staff-view").addEventListener("pointermove", (event) => moveDrag(event, $("staff-view")));
  $("staff-view").addEventListener("pointerup", endDrag);
  $("staff-view").addEventListener("pointercancel", endDrag);
  $("staff-view").addEventListener("dblclick", insertNote);
  $("tab-view").addEventListener("pointerdown", (event) => beginDrag(event, $("tab-view"), "tab"));
  $("tab-view").addEventListener("pointermove", (event) => moveDrag(event, $("tab-view")));
  $("tab-view").addEventListener("pointerup", endDrag);
  $("tab-view").addEventListener("pointercancel", endDrag);
  $("note-list").addEventListener("click", (event) => {
    const row = event.target.closest(".note-row");
    if (row) chooseNote(Number(row.dataset.index));
  });
  $("apply-note-edit").addEventListener("click", applySelectedEdit);
  $("delete-note").addEventListener("click", removeSelected);
  $("undo-button").addEventListener("click", undo);
  $("redo-button").addEventListener("click", redo);
  $("export-edits").addEventListener("click", exportEdits);
  $("play-midi").addEventListener("click", toggleMidiPlayback);
  $("play-original").addEventListener("click", playOriginal);
  $("stop-playback").addEventListener("click", stopPlayback);
  $("loop-toggle").addEventListener("click", () => {
    const enabled = $("loop-toggle").getAttribute("aria-pressed") !== "true";
    $("loop-toggle").setAttribute("aria-pressed", String(enabled));
    $("loop-toggle").classList.toggle("active", enabled);
    $("loop-toggle").title = enabled ? "Disable loop playback" : "Loop score playback";
  });
  $("measure-prev").addEventListener("click", () => navigateMeasure(-1));
  $("measure-next").addEventListener("click", () => navigateMeasure(1));
  contextControlIds.forEach((id) => $(id).addEventListener("change", () => {
    if (id === "guitar-tuning" && $("guitar-tuning").value !== "custom") {
      currentTuningOverride = null;
    }
    if (!exportReady) {
      if (projectActive) {
        saveSnapshot(history, notes, contextBeforeLastChange || readScoreContext());
        redoHistory = [];
        contextBeforeLastChange = readScoreContext();
        if (id === "guitar-tuning" || id === "capo-fret") {
          assignTabPositions();
          refreshStringOptions();
          renderSelected();
          drawAll();
        }
        updateContextSummary();
        markEdited();
        updateHistoryButtons();
        return;
      }
      contextBeforeLastChange = readScoreContext();
      updateContextSummary();
      return;
    }
    saveSnapshot(history, notes, contextBeforeLastChange || readScoreContext());
    redoHistory = [];
    contextBeforeLastChange = readScoreContext();
    if (id === "guitar-tuning" || id === "capo-fret") {
      assignTabPositions();
      refreshStringOptions();
      renderSelected();
      drawAll();
    }
    updateContextSummary();
    markEdited();
    updateHistoryButtons();
  }));
  function refreshStringOptions() {
    const selected = $("edit-string").value;
    $("edit-string").innerHTML = currentTuning().map((pitch, index) =>
      `<option value="${index + 1}">${index + 1} · ${noteName(pitch)}</option>`
    ).join("");
    if (selected) $("edit-string").value = selected;
  }

  window.addEventListener("transcribe:theme", () => {
    drawAll();
  });

  document.addEventListener("keydown", (event) => {
    const target = event.target;
    const typing = target instanceof HTMLInputElement ||
      target instanceof HTMLSelectElement || target instanceof HTMLTextAreaElement;
    if (event.code === "Space" && !typing) {
      event.preventDefault();
      toggleMidiPlayback();
    } else if (event.key === "Delete" && !typing) {
      removeSelected();
    } else if (!typing && (event.ctrlKey || event.metaKey) &&
        event.key.toLowerCase() === "z") {
      event.preventDefault();
      if (event.shiftKey) redo();
      else undo();
    } else if (!typing && (event.ctrlKey || event.metaKey) &&
        event.key.toLowerCase() === "y") {
      event.preventDefault();
      redo();
    } else if (!typing && (event.ctrlKey || event.metaKey) &&
        event.key.toLowerCase() === "s") {
      event.preventDefault();
      window.dispatchEvent(new CustomEvent("transcribe:save-request"));
    } else if (!typing && !event.ctrlKey && !event.metaKey &&
        event.key.toLowerCase() === "o") {
      event.preventDefault();
      send("browse", { field: "input", current: $("input-path").value });
    } else if (!typing && selectedIndex < 0 && event.key === "ArrowLeft") {
      event.preventDefault();
      navigateMeasure(-1);
    } else if (!typing && selectedIndex < 0 && event.key === "ArrowRight") {
      event.preventDefault();
      navigateMeasure(1);
    } else if (!typing && selectedIndex >= 0 && event.key === "ArrowUp") {
      event.preventDefault();
      saveSnapshot();
      notes[selectedIndex].midi = Math.min(127, notes[selectedIndex].midi + 1);
      notes[selectedIndex].name = noteName(notes[selectedIndex].midi);
      tabPositions[selectedIndex] = computeTabPosition(notes[selectedIndex], selectedIndex);
      renderSelected();
      drawAll();
      markEdited();
    } else if (!typing && selectedIndex >= 0 && event.key === "ArrowDown") {
      event.preventDefault();
      saveSnapshot();
      notes[selectedIndex].midi = Math.max(0, notes[selectedIndex].midi - 1);
      notes[selectedIndex].name = noteName(notes[selectedIndex].midi);
      tabPositions[selectedIndex] = computeTabPosition(notes[selectedIndex], selectedIndex);
      renderSelected();
      drawAll();
      markEdited();
    }
    updateHistoryButtons();
  });

  window.addEventListener("transcribe:notes", (event) => acceptNotes(event.detail));
  window.addEventListener("transcribe:project-active", (event) => {
    projectActive = Boolean(event.detail?.active);
  });
  window.addEventListener("transcribe:project-settings", (event) => {
    currentProjectSettings = event.detail || null;
    const tuning = currentProjectSettings?.tuning;
    currentTuningOverride = Array.isArray(tuning) && tuning.length === 6 &&
      tuning.every((pitch) => Number.isInteger(pitch))
      ? tuning.map(Number) : null;
    refreshStringOptions();
    updateContextSummary();
    drawAll();
  });
  window.addEventListener("transcribe:project-save", (event) => {
    const detail = event.detail || {};
    window.dispatchEvent(new CustomEvent("transcribe:export-started"));
    send("save-project", scoreWriteFields({ project_path: detail.path || "" }));
  });
  window.addEventListener("transcribe:project-copy", (event) => {
    const detail = event.detail || {};
    window.dispatchEvent(new CustomEvent("transcribe:export-started"));
    send("save-score-copy", scoreWriteFields({ project_path: detail.path || "" }));
  });
  window.addEventListener("transcribe:project-autosave", (event) => {
    const detail = event.detail || {};
    send("autosave-project", scoreWriteFields({
      generation: String(detail.generation || 0),
    }));
  });
  window.addEventListener("transcribe:project-autosaving", (event) => {
    const revision = Number(event.detail?.score_revision);
    if (Number.isInteger(revision) && revision >= scoreRevision) {
      scoreRevision = revision;
      if (currentMetadata) currentMetadata.score_revision = revision;
    }
    currentProjectSettings = {
      ...(currentProjectSettings || {}),
      tempo_automatic: $("tempo-auto").checked,
      meter_automatic: $("meter-control").value === "auto",
      key_automatic: $("key-control").value === "auto",
    };
  });
  window.addEventListener("transcribe:review-enabled", (event) => {
    reviewEnabled = Boolean(event.detail?.enabled);
    rebuildReviewQueue();
    renderReview(notes[selectedIndex] || null);
  });
  window.addEventListener("transcribe:review", (event) => {
    const detail = event.detail || {};
    reviewEnabled = Boolean(detail.enabled);
    const report = detail.report;
    reviewByNote = new Map(Array.isArray(report?.notes)
      ? report.notes.map((item) => [item.note_id, item]) : []);
    dismissedReviewSuggestions = new Set(
      (Array.isArray(report?.decisions) ? report.decisions : [])
        .map((decision) => decision.suggestion_id));
    rebuildReviewQueue();
    $("review-limitation").textContent = report?.limitation ||
      "No held-out labelled calibration set or distribution-shift detector is configured.";
    if (detail.evidenceTruncated) {
      $("review-limitation").textContent +=
        " Evidence retention reached its bounded memory limit; some frames were sampled or omitted.";
    }
    renderReview(notes[selectedIndex] || null);
  });
  $("audition-event").addEventListener("click", () => {
    const note = notes[selectedIndex];
    if (!note) return;
    const start = Math.max(0, Number.isFinite(note.measured_start)
      ? note.measured_start - 0.18 : note.start - 0.18);
    const end = Math.max(start + 0.05,
      Number.isFinite(note.measured_end)
        ? note.measured_end + 0.22 : note.start + note.duration + 0.22);
    send("audition-event", { start: String(start), end: String(end) });
  });
  window.addEventListener("transcribe:engraving-select", (event) => {
    const index = notes.findIndex((note) => note.id === event.detail?.id);
    chooseNote(index);
  });
  window.addEventListener("transcribe:fingering", (event) => acceptFingering(event.detail));
  window.addEventListener("transcribe:metadata", (event) => acceptMetadata(event.detail));
  window.addEventListener("transcribe:audio-preview", (event) => acceptAudioPreview(event.detail));
  window.addEventListener("transcribe:busy", (event) => {
    const state = event.detail;
    const disabled = typeof state === "object"
      ? Boolean(state.busy) : Boolean(state);
    const preserveScore = typeof state === "object" && Boolean(state.preserveScore);
    liveCaptureActive = typeof state === "object" &&
      Boolean(state.liveCapture);
    transcriptionBusy = disabled;
    if (disabled && !preserveScore) {
      exportReady = false;
      edited = false;
      waveform = [];
      spectrum = [];
      audioStart = 0;
      audioEnd = 0;
      drawWaveform();
      drawSpectrogram();
    }
    $("play-midi").disabled = disabled || !exportReady;
    const fileMode = document.querySelector(".segment.selected")?.dataset.mode === "file";
    $("play-original").disabled =
      disabled || !exportReady || !fileMode || !/\.wav$/i.test($("input-path").value);
    $("apply-note-edit").disabled = (disabled && !liveCaptureActive) ||
      selectedIndex < 0;
    $("delete-note").disabled = (disabled && !liveCaptureActive) ||
      selectedIndex < 0;
    $("export-edits").disabled = disabled || !exportReady;
    renderSelected();
  });
  window.addEventListener("transcribe:edited-export", (event) => {
    const message = event.detail;
    if (message.success && Number.isInteger(message.score_revision)) {
      scoreRevision = message.score_revision;
      reviewDecisions = [];
      updateHistoryButtons();
    }
    contextBeforeLastChange = readScoreContext();
    $("export-edits").disabled = !exportReady;
    $("export-edits").textContent = message.success
      ? "Exported ✓" : "Retry export";
    $("status-text").textContent = message.success
      ? "Edited notation, tablature, and MIDI exported"
      : `Export failed: ${message.error || "Unknown error"}`;
  });
  window.addEventListener("resize", drawAll);
  window.addEventListener("pagehide", () => {
    stopPlayback();
    if (audioContext && audioContext.state !== "closed") {
      audioContext.close().catch((error) => {
        console.warn("Could not close note playback audio context.", error);
      });
    }
  });
  window.addEventListener("DOMContentLoaded", () => {
    refreshStringOptions();
    contextBeforeLastChange = readScoreContext();
    drawAll();
    updateMeasureDisplay(0);
  });
})();
