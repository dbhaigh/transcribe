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
  let playbackMode = "";
  let playbackStarted = 0;
  let playbackEnd = 0;
  let playbackFrame = 0;
  let audioContext = null;
  let activeOscillators = [];
  let currentMetadata = {};

  function send(action, values = {}) {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(`${action}|${new URLSearchParams(values).toString()}`);
    }
  }

  function noteName(midi) {
    return `${semitoneNames[(midi % 12 + 12) % 12]}${Math.floor(midi / 12) - 1}`;
  }

  function currentTuning() {
    return tuningPitches[$("guitar-tuning").value] || tuningPitches.standard;
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
    return playbackMode ? Math.max(0, (performance.now() - playbackStarted) / 1000) : 0;
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
      context.fillStyle = selected ? "#fff2bd" : colors[index % colors.length];
      context.globalAlpha = .58 + Math.max(0, Math.min(1, note.confidence)) * .42;
      context.beginPath();
      context.ellipse(x, y, radius, radius * .72, -.28, 0, Math.PI * 2);
      context.globalAlpha = 1;
      context.strokeStyle = selected ? "#fff2bd" : colors[index % colors.length];
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
      const position = candidates[0] || { string: 0, fret: -1 };
      if (position.string) {
        active.push({ string: position.string, end: note.start + note.duration });
      }
      tabPositions[noteIndex] = {
        noteIndex, string: position.string, fret: position.fret,
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
      const value = Math.max(0, Math.min(1, note.confidence));
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
    return best || { noteIndex, string: 0, fret: -1 };
  }

  function saveSnapshot(target = history, source = notes) {
    target.push({
      notes: source.map((note) => ({ ...note })),
      positions: tabPositions.map((position) => ({ ...position })),
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
    window.dispatchEvent(new CustomEvent("transcribe:edited", { detail: { notes } }));
  }

  function renderSelected() {
    const note = notes[selectedIndex];
    document.querySelectorAll(".note-row").forEach((row) => {
      row.classList.toggle("selected", Number(row.dataset.index) === selectedIndex);
    });
    const fields = ["edit-pitch", "edit-start", "edit-duration", "edit-string", "edit-fret"];
    fields.forEach((id) => { $(id).disabled = !note || transcriptionBusy; });
    $("apply-note-edit").disabled = !note || transcriptionBusy;
    $("delete-note").disabled = !note || transcriptionBusy;
    if (!note) {
      $("selection-label").textContent = "No selection";
      $("edit-instructions").textContent = "Click a note to inspect it";
      $("alternatives-list").textContent = "Pitch suggestions appear for the selected note.";
      $("alternative-note").textContent = "Select a note";
      return;
    }
    const position = tabPositions[selectedIndex] || computeTabPosition(note, selectedIndex);
    $("selection-label").textContent = `${noteName(note.midi)} · ${note.start.toFixed(2)}s`;
    $("edit-instructions").textContent = `Confidence ${Math.round(note.confidence * 100)}%`;
    $("edit-pitch").value = note.midi;
    $("edit-start").value = note.start.toFixed(2);
    $("edit-duration").value = note.duration.toFixed(2);
    $("edit-string").value = position.string || "1";
    $("edit-fret").value = Math.max(0, position.fret);
    $("alternative-note").textContent = noteName(note.midi);
    const container = $("alternatives-list");
    container.replaceChildren();
    [-2, -1, 1, 2].forEach((offset) => {
      const pitch = Math.max(0, Math.min(127, note.midi + offset));
      const suggestion = document.createElement("button");
      suggestion.type = "button";
      suggestion.className = "alternative-pitch";
      suggestion.textContent = `${noteName(pitch)} · ${Math.max(10, Math.round(note.confidence * (1 - Math.abs(offset) * .2) * 100))}%`;
      suggestion.title = "Nearby pitch suggestion; confidence is an uncalibrated estimate.";
      suggestion.addEventListener("click", () => {
        saveSnapshot();
        notes[selectedIndex].midi = pitch;
        notes[selectedIndex].name = noteName(pitch);
        tabPositions[selectedIndex] = computeTabPosition(notes[selectedIndex], selectedIndex);
        renderSelected();
        drawAll();
        markEdited();
        updateHistoryButtons();
      });
      container.appendChild(suggestion);
    });
  }

  function chooseNote(index) {
    selectedIndex = Number.isInteger(index) && index >= 0 && index < notes.length
      ? index : -1;
    renderSelected();
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
      const fret = Math.max(0, Math.min(24, note.midi - open));
      note.midi = open + fret;
      note.name = noteName(note.midi);
      tabPositions[dragState.index] = { noteIndex: dragState.index, string, fret };
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
      midi: pitch,
      name: noteName(pitch),
      start: scoreX(event, canvas) * Math.max(1, totalDuration()),
      duration: .5,
      confidence: .65,
      amplitude: .5,
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
    const nextPitch = pitchChanged
      ? midi
      : positionChanged
        ? currentTuning()[string - 1] + currentCapo() + fret
        : note.midi;
    if (nextPitch < 0 || nextPitch > 127) {
      $("edit-instructions").textContent = "Selected string and fret exceed the MIDI pitch range";
      return;
    }
    saveSnapshot();
    redoHistory = [];
    note.midi = nextPitch;
    note.name = noteName(nextPitch);
    note.start = start;
    note.duration = noteDuration;
    const expectedFret = nextPitch - currentTuning()[string - 1] - currentCapo();
    if (expectedFret === fret && expectedFret >= 0) {
      tabPositions[selectedIndex] = { noteIndex: selectedIndex, string, fret };
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
    notes = nextNotes.map((note) => ({
      ...note,
      amplitude: Number.isFinite(note.amplitude) ? note.amplitude : 0,
    }));
    if (notes.length !== oldCount || tabPositions.length !== notes.length) {
      assignTabPositions();
      selectedIndex = -1;
      if (!edited) {
        history = [];
        redoHistory = [];
      }
    }
    duration = totalDuration();
    renderSelected();
    drawAll();
    $("export-edits").disabled = !exportReady;
  }

  function acceptMetadata(metadata) {
    currentMetadata = metadata || {};
    if (currentMetadata.tempo_inferred) {
      $("tempo-control").value = Math.round(currentMetadata.tempo);
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
    refreshStringOptions();
    updateHistoryButtons();
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
    if (playbackTime() >= playbackEnd) {
      stopPlayback();
      return;
    }
    drawStaff();
    drawTab();
    playbackFrame = requestAnimationFrame(drawPlaybackFrame);
  }

  async function playMidi() {
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
    playbackStarted = performance.now();
    activeOscillators = [];
    notes.forEach((note) => {
      const oscillator = audioContext.createOscillator();
      const gain = audioContext.createGain();
      oscillator.type = "triangle";
      oscillator.frequency.value = 440 * Math.pow(2, (note.midi - 69) / 12);
      const start = startAt + note.start;
      const end = start + Math.max(.05, note.duration);
      const amplitude = note.amplitude > 0
        ? Math.max(.025, Math.min(.2, note.amplitude * .2))
        : .11;
      gain.gain.setValueAtTime(.0001, start);
      gain.gain.linearRampToValueAtTime(amplitude, start + .012);
      gain.gain.setTargetAtTime(.0001, end, .04);
      oscillator.connect(gain);
      gain.connect(audioContext.destination);
      oscillator.start(start);
      oscillator.stop(end + .18);
      activeOscillators.push(oscillator);
    });
    $("stop-playback").disabled = false;
    drawPlaybackFrame();
  }

  function playOriginal() {
    stopPlayback();
    playbackMode = "original";
    playbackStarted = performance.now();
    playbackEnd = Math.max(totalDuration(), audioEnd);
    $("stop-playback").disabled = false;
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
    cancelAnimationFrame(playbackFrame);
    $("stop-playback").disabled = true;
    drawAll();
  }

  function exportEdits() {
    if (!exportReady) return;
    const notesPayload = notes.map((note) => [
      note.midi, note.start, note.duration,
      Math.max(0, Math.min(1, note.confidence || 0)),
      Math.max(0, Math.min(1, note.amplitude || 0)),
    ].join(",")).join(";");
    const positionsPayload = tabPositions.map((position, index) =>
      position.string > 0 ? `${index},${position.string},${position.fret}` : ""
    ).filter(Boolean).join(";");
    $("export-edits").disabled = true;
    window.dispatchEvent(new CustomEvent("transcribe:export-started"));
    send("export-edits", {
      notes: notesPayload,
      tab_positions: positionsPayload,
      tempo: $("tempo-control").value,
      key_fifths: $("key-control").value,
      minor: $("minor-control").checked ? "1" : "0",
      meter: $("meter-control").value,
      musicxml: $("musicxml-path").value,
      tab: $("tab-path").value,
      midi: $("midi-path").value,
    });
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
  $("play-midi").addEventListener("click", () => { playMidi().catch((error) => {
    $("status-text").textContent = `Note playback failed: ${error.message}`;
  }); });
  $("play-original").addEventListener("click", playOriginal);
  $("stop-playback").addEventListener("click", stopPlayback);
  ["guitar-tuning", "capo-fret"].forEach((id) => $(id).addEventListener("change", () => {
    assignTabPositions();
    refreshStringOptions();
    renderSelected();
    drawAll();
  }));

  function refreshStringOptions() {
    const selected = $("edit-string").value;
    $("edit-string").innerHTML = currentTuning().map((pitch, index) =>
      `<option value="${index + 1}">${index + 1} · ${noteName(pitch)}</option>`
    ).join("");
    if (selected) $("edit-string").value = selected;
  }

  $("theme-toggle").addEventListener("click", () => {
    const next = document.body.dataset.theme === "light" ? "dark" : "light";
    document.body.dataset.theme = next;
    $("theme-toggle").textContent = next === "light" ? "Dark mode" : "Light mode";
    localStorage.setItem("transcribe.theme", next);
    drawAll();
  });

  document.addEventListener("keydown", (event) => {
    const target = event.target;
    const typing = target instanceof HTMLInputElement ||
      target instanceof HTMLSelectElement || target instanceof HTMLTextAreaElement;
    if (event.code === "Space" && !typing) {
      event.preventDefault();
      if (playbackMode) stopPlayback();
      else playMidi().catch(() => {});
    } else if (event.key === "Delete" && !typing) {
      removeSelected();
    } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "z") {
      event.preventDefault();
      if (event.shiftKey) redo();
      else undo();
    } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "y") {
      event.preventDefault();
      redo();
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
  window.addEventListener("transcribe:metadata", (event) => acceptMetadata(event.detail));
  window.addEventListener("transcribe:audio-preview", (event) => acceptAudioPreview(event.detail));
  window.addEventListener("transcribe:busy", (event) => {
    const disabled = Boolean(event.detail);
    transcriptionBusy = disabled;
    if (disabled) {
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
    $("apply-note-edit").disabled = disabled || selectedIndex < 0;
    $("delete-note").disabled = disabled || selectedIndex < 0;
    $("export-edits").disabled = disabled || !exportReady;
    renderSelected();
  });
  window.addEventListener("transcribe:edited-export", (event) => {
    const message = event.detail;
    $("export-edits").disabled = !exportReady;
    $("export-edits").textContent = message.success
      ? "Exported ✓" : "Retry export";
    $("status-text").textContent = message.success
      ? "Edited notation, tablature, and MIDI exported"
      : `Export failed: ${message.error || "Unknown error"}`;
  });
  window.addEventListener("resize", drawAll);
  window.addEventListener("DOMContentLoaded", () => {
    const savedTheme = localStorage.getItem("transcribe.theme") || "dark";
    document.body.dataset.theme = savedTheme;
    $("theme-toggle").textContent = savedTheme === "light" ? "Dark mode" : "Light mode";
    refreshStringOptions();
    drawAll();
  });
})();
