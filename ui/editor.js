(() => {
  const $ = (id) => document.getElementById(id);
  const timeline = window.TranscribeTimeline;
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
  let waveformMinimum = [];
  let waveformMaximum = [];
  let spectrum = [];
  let spectrumBands = 40;
  let spectrumMinimum = 0;
  let spectrumMaximum = 0;
  let audioColumns = [];
  let history = [];
  let redoHistory = [];
  let dragState = null;
  let exportReady = false;
  let edited = false;
  let transcriptionBusy = false;
  let liveCaptureActive = false;
  let liveCaptureSavedPositions = null;
  let liveGapRegions = [];
  let editBaselineNotes = new Map();
  let editBaselineFingerings = new Map();
  let pendingLiveDeletes = new Set();
  let playbackMode = "";
  let pausedMode = "";
  let pausedAt = 0;
  let playbackStarted = 0;
  let playbackGeneration = 0;
  let originalPosition = 0;
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
  let phraseFingerings = [];
  let phraseFingeringSignature = "";
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
  let paintFrame = 0;
  let visibleEntries = [];
  let noteIndex = timeline.index([]);
  let paintedDuration = -1;
  let audioDirty = true;
  let scoreDirty = true;
  let noteIndexDirty = true;

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
    return Math.max(duration, audioEnd, 1);
  }

  function noteColor(note) {
    if (document.body.dataset.theme === "light") {
      if (note.user_authored || note.state === "manually_corrected") return "#805a00";
      if (note.uncertain) return "#ab3527";
      if (note.state === "provisional") return "#345d9a";
      return note.finalized ? "#16705c" : "#6653a4";
    }
    if (note.user_authored || note.state === "manually_corrected") return "#c39b35";
    if (note.uncertain) return "#e27467";
    if (note.state === "provisional") return "#819ee5";
    return note.finalized ? "#32ae8c" : "#8b83d0";
  }

  function tempoMap() {
    const overridden = !$("tempo-auto").checked &&
      Math.abs(Number($("tempo-control").value) - currentMetadata.tempo) > .5;
    return overridden ? [] : currentMetadata.tempo_map || [];
  }

  function currentTempo() {
    return $("tempo-auto").checked && Number.isFinite(currentMetadata.tempo)
      ? currentMetadata.tempo : Number($("tempo-control").value) || 120;
  }

  function quartersAt(seconds) {
    return timeline.quarters(seconds, tempoMap(), currentTempo());
  }

  function remapTempo(previous) {
    const before = previous["tempo-auto"] && Number.isFinite(currentMetadata.tempo)
      ? currentMetadata.tempo : Number(previous["tempo-control"]) || 120;
    const after = currentTempo();
    const oldMap = !previous["tempo-auto"] && Math.abs(before - currentMetadata.tempo) > .5
      ? [] : currentMetadata.tempo_map || [];
    notes.forEach((note) => {
      const start = timeline.quarters(note.start, oldMap, before);
      const end = timeline.quarters(note.start + note.duration, oldMap, before);
      note.start = timeline.seconds(start, tempoMap(), after);
      note.duration = timeline.seconds(end, tempoMap(), after) - note.start;
    });
  }

  function fitCanvas(canvas) {
    canvas.dataset.paintCount = String(Number(canvas.dataset.paintCount || 0) + 1);
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

    function writtenPosition(note, baseY, step) {
      const degree = ["C", "D", "E", "F", "G", "A", "B"].indexOf(note.spelling_step);
      if (degree < 0 || !Number.isInteger(note.spelling_octave) || note.user_authored)
        return staffPosition(note.midi, baseY, step);
      return { y: baseY - (note.spelling_octave * 7 + degree - 30) * step,
        accidental: note.accidental_required, alter: note.spelling_alter };
    }

    function drawAccidental(context, x, y, alter = 1) {
      context.save();
      context.translate(x, y);
      context.lineWidth = 1.3;
      context.beginPath();
      if (alter < 0) {
        context.moveTo(0, -11); context.lineTo(0, 6);
        context.bezierCurveTo(12, -1, 4, -7, 0, -1);
      } else {
        context.moveTo(-2, -8); context.lineTo(-2, alter ? 8 : 3);
        context.moveTo(2, alter ? -8 : -3); context.lineTo(2, 8);
        context.moveTo(-4, -2); context.lineTo(4, -4);
        context.moveTo(-4, 4); context.lineTo(4, 2);
      }
      context.stroke();
      context.restore();
    }

    function drawBarLines(context, width, top, bottom) {
      const view = timeline.bounds(totalDuration());
      const meter = ($("meter-control").value === "auto"
        ? `${currentMetadata.time_numerator || 4}/${currentMetadata.time_denominator || 4}`
        : $("meter-control").value).split("/").map(Number);
      const bar = meter[0] * 4 / meter[1];
      const first = Math.max(0, Math.floor(quartersAt(view.start) / bar));
      const last = Math.ceil(quartersAt(view.end) / bar);
      context.save();
      context.strokeStyle = canvasColor("--muted", "#81919b");
      context.fillStyle = context.strokeStyle;
      context.font = "10px Segoe UI";
      // At full-take zoom retain at most one barline per eight screen pixels.
      const stride = Math.max(1, Math.ceil((last - first) * 8 / width));
      for (let number = first; number <= last; number += stride) {
        const seconds = timeline.seconds(number * bar, tempoMap(), currentTempo());
        const x = timeline.x(seconds, totalDuration(), width);
        if (x < 36 || x > width) continue;
        context.beginPath(); context.moveTo(x, top); context.lineTo(x, bottom); context.stroke();
        context.fillText(String(number + 1), x + 3, top - 5);
      }
      context.restore();
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
    return timeline.x(note.start, totalDuration(), width);
  }

  function drawUnanalysedRegions(context, width, height) {
    const view = timeline.bounds(totalDuration());
    const visible = liveGapRegions.filter((gap) =>
      gap.end_seconds > view.start && gap.start_seconds < view.end);
    const canvas = context.canvas;
    if (canvas) canvas.dataset.gapRegions = String(visible.length);
    for (const gap of visible) {
      const left = Math.max(36, timeline.x(Math.max(view.start, gap.start_seconds),
        totalDuration(), width));
      const right = Math.min(width - 10, timeline.x(Math.min(view.end, gap.end_seconds),
        totalDuration(), width));
      if (right <= left) continue;
      context.save();
      context.beginPath();
      context.rect(left, 0, right - left, height);
      context.clip();
      context.fillStyle = "rgba(233, 197, 120, .12)";
      context.fillRect(left, 0, right - left, height);
      context.strokeStyle = "rgba(233, 197, 120, .5)";
      context.lineWidth = 1;
      for (let x = left - height; x < right; x += 9) {
        context.beginPath();
        context.moveTo(x, height);
        context.lineTo(x + height, 0);
        context.stroke();
      }
      context.fillStyle = canvasColor("--text", "#ecf2f4");
      context.font = 'bold 9px "DM Mono", Consolas, monospace';
      context.fillText("UNANALYSED", left + 4, 13);
      context.restore();
    }
  }

  function playbackTime() {
    if (playbackMode === "midi") return Math.max(0, audioContext.currentTime - playbackStarted);
    if (playbackMode === "original") return originalPosition;
    return pausedMode ? pausedAt : 0;
  }

  function drawStaff() {
    const canvas = $("staff-view");
    const low = visibleEntries.reduce((value, { note }) => Math.min(value, note.midi), 127);
    const high = visibleEntries.reduce((value, { note }) => Math.max(value, note.midi), 0);
    const widestRange = Math.max(0, high - low);
    canvas.style.height = `${Math.max(190, Math.min(380, 150 + widestRange * 2.2))}px`;
    const frame = fitCanvas(canvas);
    if (!frame) return;
    const { context, width, height } = frame;
    const background = canvasColor("--field", "#0c1319");
    const muted = canvasColor("--muted", "#81919b");
    context.clearRect(0, 0, width, height);
    context.fillStyle = background;
    context.fillRect(0, 0, width, height);
    drawUnanalysedRegions(context, width, height);
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
    // Vector treble clef: no font-dependent musical text substitutes.
    context.save();
    context.translate(21, baseY - lineGap * 2);
    context.strokeStyle = muted; context.lineWidth = 1.7;
    context.beginPath();
    context.moveTo(0, 29);
    context.bezierCurveTo(16, 32, -10, -37, 2, -30);
    context.bezierCurveTo(17, -16, -19, -5, -8, 11);
    context.bezierCurveTo(6, 26, 18, 4, 3, 0);
    context.bezierCurveTo(-6, -2, -6, 9, 0, 10);
    context.stroke(); context.restore();
    drawBarLines(context, width, baseY - 44, baseY);
    if (visibleEntries.length > 1500) {
      context.fillStyle = muted;
      context.fillText(`${visibleEntries.length} notes in overview · zoom the timeline for readable notation`, 40, 35);
      return;
    }
    if (!notes.length) {
      context.fillStyle = "#6e8089";
      context.font = "11px Segoe UI, sans-serif";
      context.fillText("Detected notes will be placed on the staff as they stabilize.", margin, height / 2);
      return;
    }
    const beams = new Map();
    const beamCounts = new Map();
    const tieEnds = new Map();
    visibleEntries.forEach(({ note }) => {
      const beats = note.rhythm_denominator > 0 && !note.user_authored
        ? 4 / note.rhythm_denominator
        : quartersAt(note.start + note.duration) - quartersAt(note.start);
      const key = `${note.voice || 1}:${Math.floor(quartersAt(note.start) + 1e-6)}`;
      if (beats <= .5) beamCounts.set(key, (beamCounts.get(key) || 0) + 1);
      if (note.tie_stop) tieEnds.set(`${note.voice || 1}:${note.midi}:${note.start.toFixed(3)}`, note);
    });
    visibleEntries.forEach(({ note, index }) => {
      const x = noteX(note, width);
      const { y, accidental, alter } = writtenPosition(note, baseY, step);
      const radius = index === selectedIndex ? 6 : 5;
      const selected = index === selectedIndex;
      const stateColor = noteColor(note);
      const visualState = note.live_visual_state ||
        (note.finalized ? "finalized" : note.state);
      const opacity = Number.isFinite(note.live_visual_opacity)
        ? note.live_visual_opacity : 1;
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
      context.fillStyle = selected ? canvasColor("--text", "#ecf2f4") : stateColor;
      context.globalAlpha = opacity * (visualState === "provisional" ? .68 : .88);
      if (visualState === "provisional") context.setLineDash([2, 2]);
      context.beginPath();
      context.ellipse(x, y, radius, radius * .72, -.28, 0, Math.PI * 2);
      context.strokeStyle = selected ? "#fff2bd" : stateColor;
      const beats = note.rhythm_denominator > 0 && !note.user_authored
        ? 4 / note.rhythm_denominator
        : quartersAt(note.start + note.duration) - quartersAt(note.start);
      if (visualState === "provisional") context.stroke();
      else if (visualState === "stabilized" || beats < 1.8) context.fill();
      else context.stroke();
      if (visualState === "stabilized") {
        context.fillStyle = stateColor;
        context.font = 'bold 8px "DM Mono", Consolas, monospace';
        context.fillText("S", x + radius + 2, y - radius);
      } else if (visualState === "corrected") {
        context.lineWidth = 2;
        context.beginPath();
        context.ellipse(x, y, radius + 3, radius * .72 + 3, -.28, 0, Math.PI * 2);
        context.stroke();
      } else if (visualState === "retracted") {
        context.beginPath();
        context.moveTo(x - radius, y - radius);
        context.lineTo(x + radius, y + radius);
        context.moveTo(x + radius, y - radius);
        context.lineTo(x - radius, y + radius);
        context.stroke();
      }
      if (beats < 3.8) {
        const stemUp = note.voice > 1 ? note.voice % 2 === 1 : y >= baseY - lineGap * 2;
        const stemX = stemUp ? x + radius - 1 : x - radius + 1;
        const stemEnd = y + (stemUp ? -25 : 25);
        context.beginPath();
        context.moveTo(stemX, y);
        context.lineTo(stemX, stemEnd);
        context.stroke();
        const beamKey = `${note.voice || 1}:${Math.floor(quartersAt(note.start) + 1e-6)}`;
        const previous = beams.get(beamKey);
        const beamed = beats <= .5 && previous &&
          Math.abs(previous.end - note.start) < .035 && previous.up === stemUp;
        if (beamed) {
          context.lineWidth = 3;
          context.beginPath();
          context.moveTo(previous.x, previous.y);
          context.lineTo(stemX, stemEnd);
          context.stroke();
        } else if (beats < .55 && (beamCounts.get(beamKey) || 0) < 2) {
          context.beginPath();
          context.moveTo(stemX, stemEnd);
          context.quadraticCurveTo(stemX + 12, stemEnd + 4, stemX + 6, stemEnd + 11);
          context.stroke();
        }
        if (beats <= .5) beams.set(beamKey, { x: stemX, y: stemEnd,
          up: stemUp, end: note.start + note.duration });
      }
      if (accidental) {
        drawAccidental(context, x - 12, y, alter);
      }
      const dotted = !note.user_authored && note.rhythm_denominator
        ? note.rhythm_dots > 0 : [.75, 1.5, 3, 6].some(value => Math.abs(beats - value) < .03);
      if (dotted) {
        context.beginPath(); context.arc(x + 10, y - 2, 1.7, 0, Math.PI * 2); context.fill();
      }
      if (note.tuplet_actual > 0 && !note.user_authored) {
        context.fillStyle = muted; context.font = "10px Segoe UI";
        context.fillText(String(note.tuplet_actual), x, y - 35);
      }
      if (note.tie_start) {
        const next = tieEnds.get(`${note.voice || 1}:${note.midi}:${(note.start + note.duration).toFixed(3)}`);
        if (next) {
          const endX = noteX(next, width);
          context.beginPath(); context.moveTo(x + 4, y + 8);
          context.quadraticCurveTo((x + endX) / 2, y + 23, endX - 4, y + 8);
          context.stroke();
        }
      }
      if (selected) {
        context.fillStyle = canvasColor("--text", "#ecf2f4");
        context.font = '8px "DM Mono", Consolas, monospace';
        context.fillText(noteName(note.midi), x - 12, y - 10);
      }
      context.setLineDash([]);
      context.restore();
    });
    const view = timeline.bounds(totalDuration());
    // Rests come only from the canonical score, never guessed from a display gap.
    for (const rest of edited ? [] : currentMetadata.rests || []) {
      if (rest.start < view.start || rest.start > view.end) continue;
      const x = timeline.x(rest.start, totalDuration(), width);
      const y = baseY - 22 + (rest.voice > 1 ? 10 : 0);
      context.strokeStyle = muted; context.fillStyle = muted; context.lineWidth = 2;
      if (rest.denominator === 1 || rest.denominator === 2) {
        context.fillRect(x - 5, y + (rest.denominator === 1 ? -11 : -4), 10, 4);
      } else if (rest.denominator >= 8) {
        context.beginPath(); context.arc(x - 3, y - 4, 2.5, 0, Math.PI * 2); context.fill();
        context.beginPath(); context.moveTo(x - 2, y - 3);
        context.lineTo(x + 4, y - 6); context.lineTo(x, y + 8); context.stroke();
      } else {
        context.beginPath(); context.moveTo(x - 2, baseY - 32);
        context.lineTo(x + 3, baseY - 25); context.lineTo(x - 3, baseY - 18);
        context.quadraticCurveTo(x - 6, baseY - 15, x, baseY - 12); context.stroke();
      }
      if (rest.dots) { context.beginPath(); context.arc(x + 8, y, 1.5, 0, Math.PI * 2); context.fill(); }
    }
    context.fillStyle = muted;
    context.font = '10px "Segoe UI", sans-serif';
    context.fillText(`${view.start.toFixed(1)}s`, margin, height - 8);
    context.textAlign = "right";
    context.fillText(`${view.end.toFixed(1)}s`, width - 12, height - 8);
    context.textAlign = "left";
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
      const prior = previous[noteIndex];
      const priorIsValid = prior &&
        prior.string > 0 && prior.fret >= 0 && prior.fret <= 36 &&
        tuning[prior.string - 1] + capo + prior.fret === note.midi &&
        !active.some((item) => item.string === prior.string);
      const position = priorIsValid
        ? { ...prior }
        : { string: 0, fret: -1, manual: false };
      if (position.string) {
        active.push({ string: position.string, end: note.start + note.duration });
      }
      tabPositions[noteIndex] = {
        ...position,
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
    drawUnanalysedRegions(context, width, height);
    const margin = 36;
    const top = 20;
    const stringCount = currentTuning().length;
    const gap = Math.min(15, (height - 38) / Math.max(1, stringCount - 1));
    context.strokeStyle = "rgba(123, 148, 157, .42)";
    context.lineWidth = 1;
    for (let string = 0; string < stringCount; string += 1) {
      const y = top + string * gap;
      context.beginPath();
      context.moveTo(margin, y + .5);
      context.lineTo(width - 10, y + .5);
      context.stroke();
      context.fillStyle = "#80919a";
      context.font = '8px "DM Mono", Consolas, monospace';
      context.fillText(String(string + 1), 14, y + 3);
    }
    drawBarLines(context, width, top, top + (stringCount - 1) * gap);
    if (visibleEntries.length > 1500) {
      context.fillStyle = canvasColor("--muted", "#81919b");
      context.fillText("Zoom the timeline for individual frets and durations", 40, height - 12);
      return;
    }
    const tuning = currentTuning();
    $("tab-tuning-label").textContent =
      `${tuningLabels[$("guitar-tuning").value] || tuningLabels.standard} · capo ${currentCapo()}`;
    let unresolvedTabNotes = 0;
    visibleEntries.forEach(({ note, index }) => {
      const position = tabPositions[index] || { string: 0, fret: -1 };
      const visualState = note.live_visual_state ||
        (note.finalized ? "finalized" : note.state);
      const opacity = Number.isFinite(note.live_visual_opacity)
        ? note.live_visual_opacity : 1;
      if (!position.string || !Number.isInteger(position.fret) || position.fret < 0) {
        unresolvedTabNotes += 1;
        const x = noteX(note, width);
        context.save();
        context.globalAlpha = opacity;
        context.strokeStyle = noteColor(note);
        context.setLineDash([2, 3]);
        context.beginPath();
        context.moveTo(x, top);
        context.lineTo(x, top + (stringCount - 1) * gap);
        context.stroke();
        context.setLineDash([]);
        context.font = 'bold 11px "DM Mono", Consolas, monospace';
        context.textAlign = "center";
        context.fillText("?", x, top - 5);
        context.restore();
        return;
      }
      const y = top + (position.string - 1) * gap;
      const x = noteX(note, width);
      context.save();
      context.globalAlpha = opacity * (visualState === "provisional" ? .68 : 1);
      context.fillStyle = index === selectedIndex
        ? canvasColor("--text", "#ecf2f4") : noteColor(note);
      context.strokeStyle = noteColor(note);
      context.font = index === selectedIndex
        ? 'bold 10px "DM Mono", Consolas, monospace'
        : '9px "DM Mono", Consolas, monospace';
      context.textAlign = "center";
      if (visualState === "provisional") {
        context.beginPath();
        context.arc(x, y, 7, 0, Math.PI * 2);
        context.stroke();
      }
      context.fillText(String(position.fret), x, y + 4);
      context.strokeStyle = noteColor(note);
      if (visualState === "stabilized") {
        context.beginPath();
        context.arc(x, y - 6, 2, 0, Math.PI * 2);
        context.fill();
      } else if (visualState === "corrected") {
        context.lineWidth = 1.5;
        context.beginPath();
        context.arc(x, y, 8, 0, Math.PI * 2);
        context.stroke();
      } else if (visualState === "retracted") {
        context.beginPath();
        context.moveTo(x - 5, y - 5);
        context.lineTo(x + 5, y + 5);
        context.moveTo(x + 5, y - 5);
        context.lineTo(x - 5, y + 5);
        context.stroke();
      }
      const endX = timeline.x(note.start + note.duration, totalDuration(), width);
      context.strokeStyle = noteColor(note);
      context.setLineDash(visualState === "provisional" ? [3, 3] : []);
      context.beginPath();
      context.moveTo(x + 6, y + 6);
      context.lineTo(Math.max(x + 7, endX), y + 6);
      context.stroke();
      context.setLineDash([]);
      context.textAlign = "left";
      context.restore();
    });
    canvas.dataset.unresolvedTabNotes = String(unresolvedTabNotes);
    context.fillStyle = "#768992";
    const view = timeline.bounds(totalDuration());
    context.font = '10px "Segoe UI", sans-serif';
    context.fillText(`${view.start.toFixed(1)}s`, margin, height - 8);
    context.textAlign = "right";
    context.fillText(`${view.end.toFixed(1)}s`, width - 12, height - 8);
    context.textAlign = "left";
  }

  function drawWaveform() {
    const frame = fitCanvas($("waveform"));
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    context.fillStyle = "rgba(116, 228, 189, .76)";
    const plotHeight = height - 20;
    const middle = plotHeight / 2;
    if (waveform.length) {
      waveform.forEach((peak, index) => {
        const column = audioColumns[index];
        if (!column) return;
        const view = timeline.bounds(totalDuration());
        if (column.end < view.start || column.start > view.end) return;
        const x = timeline.x(column.start, totalDuration(), width);
        const endX = timeline.x(column.end, totalDuration(), width);
        const minimum = Number.isFinite(waveformMinimum[index])
          ? waveformMinimum[index] : -Math.abs(peak);
        const maximum = Number.isFinite(waveformMaximum[index])
          ? waveformMaximum[index] : Math.abs(peak);
        const firstY = middle - maximum * plotHeight * .44;
        const lastY = middle - minimum * plotHeight * .44;
        context.fillRect(x, Math.min(firstY, lastY),
          Math.max(1, endX - x), Math.max(1, Math.abs(lastY - firstY)));
      });
    } else {
      context.strokeStyle = "rgba(116, 228, 189, .76)";
      context.beginPath();
      context.moveTo(0, middle);
      context.lineTo(width, middle);
      context.stroke();
    }
    drawAudioAxes(frame, false);
  }

  function drawAudioAxes({ context, width, height }, frequency) {
    const plotHeight = height - 20;
    const view = timeline.bounds(totalDuration());
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, 36, height);
    context.fillRect(0, plotHeight, width, 20);
    context.fillStyle = canvasColor("--muted", "#a4b4be");
    context.font = '10px "Segoe UI", sans-serif';
    context.textBaseline = "middle";
    context.textAlign = "left";
    if (frequency && spectrumMaximum > spectrumMinimum && spectrumMinimum > 0) {
      for (const hz of [55, 100, 1000, 10000]) {
        if (hz < spectrumMinimum || hz > spectrumMaximum) continue;
        const y = plotHeight * (1 - Math.log(hz / spectrumMinimum) /
          Math.log(spectrumMaximum / spectrumMinimum));
        context.fillText(hz >= 1000 ? `${hz / 1000}k` : String(hz), 1, Math.max(7, Math.min(plotHeight - 6, y)));
      }
    } else if (!frequency) {
      context.fillText("1", 4, 8);
      context.fillText("0", 4, plotHeight / 2);
      context.fillText("−1", 1, plotHeight - 8);
    } else {
      context.fillText("Hz ?", 1, 10);
    }
    for (let tick = 0; tick <= 4; tick += 1) {
      const seconds = view.start + tick * (view.end - view.start) / 4;
      const x = timeline.x(seconds, totalDuration(), width);
      context.textAlign = tick === 4 ? "right" : tick === 0 ? "left" : "center";
      context.fillText(`${seconds.toFixed(view.end - view.start < 5 ? 2 : 1)}s`, x, height - 8);
    }
    context.textAlign = "left";
    context.textBaseline = "alphabetic";
    $(frequency ? "spectrogram" : "waveform").setAttribute("aria-label",
      `${frequency ? "Log-frequency relative-energy spectrogram" : "Normalized signed min/max waveform envelope"}, ${view.start.toFixed(2)} to ${view.end.toFixed(2)} seconds` +
      (frequency && spectrumMaximum > 0 ? `, ${spectrumMinimum} to ${spectrumMaximum} Hz` : ""));
  }

  function drawSpectrogram() {
    const frame = fitCanvas($("spectrogram"));
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    if (!spectrum.length || !spectrumBands) { drawAudioAxes(frame, true); return; }
    const columns = spectrum.length / spectrumBands;
    const plotHeight = height - 20;
    const cellHeight = plotHeight / spectrumBands;
    for (let column = 0; column < columns; column += 1) {
      const span = audioColumns[column];
      if (!span) continue;
      const view = timeline.bounds(totalDuration());
      if (span.end < view.start || span.start > view.end) continue;
      const x = timeline.x(span.start, totalDuration(), width);
      const cellWidth = timeline.x(span.end, totalDuration(), width) - x;
      for (let band = 0; band < spectrumBands; band += 1) {
        const strength = spectrum[column * spectrumBands + band];
        if (strength < .05) continue;
        context.fillStyle = `rgba(${Math.round(36 + strength * 80)},${Math.round(93 + strength * 162)},${Math.round(115 + strength * 97)},${Math.min(.95, .18 + strength * .82)})`;
        context.fillRect(x, plotHeight - (band + 1) * cellHeight,
          Math.ceil(cellWidth), Math.ceil(cellHeight));
      }
    }
    drawAudioAxes(frame, true);
  }

  function drawConfidence() {
    const frame = fitCanvas($("confidence-heatmap"));
    if (!frame) return;
    const { context, width, height } = frame;
    context.clearRect(0, 0, width, height);
    context.fillStyle = canvasColor("--field", "#0c1319");
    context.fillRect(0, 0, width, height);
    if (!notes.length) return;
    visibleEntries.forEach(({ note }) => {
      const x = timeline.x(note.start, totalDuration(), width);
      const w = Math.max(2, timeline.x(note.start + note.duration, totalDuration(), width) - x);
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

  function drawAll(score = true) {
    scoreDirty ||= Boolean(score);
    if (paintFrame) return;
    paintFrame = requestAnimationFrame(() => {
      paintFrame = 0;
      if (noteIndexDirty) duration = notes.reduce((end, note) => Math.max(end, note.start + note.duration), 1);
      if (paintedDuration !== totalDuration()) scoreDirty = true;
      if (scoreDirty) {
        if (noteIndexDirty) {
          noteIndex = timeline.index(notes);
          noteIndexDirty = false;
        }
        visibleEntries = noteIndex(totalDuration());
        drawStaff();
        drawTab();
        drawConfidence();
        scoreDirty = false;
      }
      if (audioDirty || paintedDuration !== totalDuration()) {
        drawWaveform();
        drawSpectrogram();
        audioDirty = false;
        paintedDuration = totalDuration();
      }
    });
  }

  function computeTabPosition(note, noteIndex) {
    return { noteIndex, string: 0, fret: -1, manual: false, candidates: [],
      explanation: "Unresolved after editing; select a pitch-compatible fingering." };
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
    if (target === history) stopPlayback();
    target.push(structuredClone({
      notes: source,
      positions: tabPositions,
      selectedId: notes[selectedIndex]?.id || null,
      pendingLiveDeletes: [...pendingLiveDeletes],
      settings,
      reviewDecisions,
      dismissedReviewSuggestions: [...dismissedReviewSuggestions],
    }));
    if (target.length > 100) target.shift();
  }

  function updateHistoryButtons() {
    const locked = transcriptionBusy && !liveCaptureActive;
    $("undo-button").disabled = locked || history.length === 0;
    $("redo-button").disabled = locked || redoHistory.length === 0;
  }

  function markEdited(restoring = false) {
    noteIndexDirty = true;
    const assigned = new Map();
    let error = "";
    for (let index = 0; index < notes.length; index += 1) {
      const note = notes[index];
      const position = tabPositions[index];
      if (!Number.isInteger(note.midi) || note.midi < 0 || note.midi > 127 ||
          !Number.isFinite(note.start) || note.start < 0 ||
          !Number.isFinite(note.duration) || note.duration <= 0) {
        error = "Edit rejected: invalid pitch or timing."; break;
      }
      if (!position?.string) continue;
      if (currentTuning()[position.string - 1] + currentCapo() + position.fret !== note.midi) {
        error = "Edit rejected: string/fret must preserve pitch."; break;
      }
      const previous = assigned.get(position.string) || [];
      if (previous.some((other) => note.start < other.start + other.duration - 1e-9 &&
          other.start < note.start + note.duration - 1e-9)) {
        error = "Edit rejected: overlapping notes cannot occupy the same string."; break;
      }
      previous.push(note);
      assigned.set(position.string, previous);
    }
    if (error) {
      const before = history.pop();
      if (before) restoreSnapshot(before, false);
      $("edit-instructions").textContent = error;
      $("status-text").textContent = error;
      updateHistoryButtons();
      return;
    }
    redoHistory = [];
    stopPlayback();
    exportReady = Boolean(scoreId);
    $("play-midi").disabled = !exportReady || notes.length === 0;
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
        if (!restoring) {
          note.user_authored = true;
          note.state = "manually_corrected";
        }
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
    editBaselineFingerings.forEach((_, id) => {
      if (current.has(id) && !currentFingerings.has(id)) {
        changes.push({ kind: "fingering", note_id: id, string: 0, fret: -1 });
      }
    });
    editBaselineNotes = new Map([...current].map(([id, note]) =>
      [id, { midi: note.midi, start: note.start, duration: note.duration }]));
    editBaselineFingerings = currentFingerings;
    window.dispatchEvent(new CustomEvent("transcribe:edited", {
      detail: { notes, changes, dirty: true },
    }));
  }

  function updateMeasureDisplay(time = notes[selectedIndex]?.start ?? playbackTime()) {
    const [numerator, denominator] = ($("meter-control").value === "auto"
      ? `${currentMetadata.time_numerator || 4}/${currentMetadata.time_denominator || 4}`
      : $("meter-control").value).split("/").map(Number);
    const barQuarters = Math.max(.25, numerator * 4 / denominator);
    const pickup = Math.max(0, Number($("pickup-control").value) || 0);
    const quarters = Math.max(0, quartersAt(time));
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
      const quarters = quartersAt(note.start);
      return quarters >= startQuarters && quarters < endQuarters;
    });
    const fallback = notes.reduce((best, note, index) =>
      Math.abs(quartersAt(note.start) - startQuarters) <
        Math.abs(quartersAt(notes[best].start) - startQuarters)
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
    $("split-note").disabled = !note || editDisabled || note.duration < .1;
    $("merge-note").disabled = !note || editDisabled;
    if (!note) {
      $("selection-label").textContent = "No selection";
      $("edit-instructions").textContent = "Click a note to inspect it";
      $("detected-pitch").textContent = "No note selected";
      $("detected-source").textContent =
        "Select a note in notation, tablature, or the timeline.";
      $("detected-onset").textContent = "—";
      $("detected-offset").textContent = "—";
      $("detected-duration").textContent = "—";
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
    $("detected-duration").textContent = `${note.duration.toFixed(3)} s`;
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
    if (note.finalized) $("detected-source").textContent += " · finalized (no further live revisions)";
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
    if (note.guitar_technique) {
      $("edit-instructions").textContent +=
        ` · Guitar contour: ${note.guitar_technique.gesture}. ${note.guitar_technique.explanation}` +
        " Technique confidence is uncalibrated; original fingering is unknown.";
    }
    $("edit-pitch").value = note.midi;
    $("edit-start").value = note.start.toFixed(2);
    $("edit-duration").value = note.duration.toFixed(2);
    $("edit-string").value = position.string || "0";
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
        if (transcriptionBusy && !liveCaptureActive) return;
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
    const uncertainty = document.createElement("span");
    uncertainty.textContent =
      "Suggested positions, not detected strings. Fingering costs are not calibrated confidence.";
    fingeringContainer.appendChild(uncertainty);
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
        position.explanation || "Unresolved string/fret assignment under the current tuning and capo.";
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
        ? `Sequence cost: fret ${costs.fret_height.toFixed(2)}, preferred ${costs.preferred_position.toFixed(2)}, hand movement ${costs.hand_movement.toFixed(2)}, string preference ${costs.string_preference.toFixed(2)}, chord stretch ${costs.chord_stretch.toFixed(2)}, repeated strings ${(costs.repeated_strings || 0).toFixed(2)}, crossings ${(costs.string_crossings || 0).toFixed(2)}, awkward transitions ${(costs.awkward_transitions || 0).toFixed(2)}, unassigned ${costs.unassigned_notes.toFixed(2)}. Choose without changing pitch.`
        : "Choose this fingering without changing the sounding pitch.";
      suggestion.addEventListener("click", () => {
        if (transcriptionBusy && !liveCaptureActive) return;
        const open = currentTuning()[candidate.string - 1] + currentCapo();
        if (open + candidate.fret !== notes[selectedIndex].midi) return;
        const selected = notes[selectedIndex];
        if (notes.some((other, index) => index !== selectedIndex &&
            tabPositions[index]?.string === candidate.string &&
            other.start < selected.start + selected.duration &&
            selected.start < other.start + other.duration)) {
          $("edit-instructions").textContent =
            "That string is occupied by another note. Choose a complete phrase alternative instead.";
          return;
        }
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
    if (phraseFingeringSignature === fingeringSignature()) {
      phraseFingerings.forEach((alternative, rank) => {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "alternative-pitch phrase-fingering";
        button.textContent = `Phrase option ${rank + 1} · cost ${Number(alternative.cost?.total || 0).toFixed(1)}`;
        button.title = "Apply this joint string/fret assignment without changing any pitch or timing.";
        button.addEventListener("click", () => {
          if ((transcriptionBusy && !liveCaptureActive) ||
              phraseFingeringSignature !== fingeringSignature()) return;
          const positions = alternative.positions;
          if (!Array.isArray(positions) || positions.length !== notes.length ||
              positions.some((p, index) => p.note_index !== index ||
                !Number.isInteger(p.string) || !Number.isInteger(p.fret) ||
                p.string < 1 || p.string > currentTuning().length || p.fret < 0 || p.fret > 36 ||
                currentTuning()[p.string - 1] + currentCapo() + p.fret !== notes[index].midi ||
                positions.some((other, otherIndex) => otherIndex !== index && other.string === p.string &&
                  notes[index].start < notes[otherIndex].start + notes[otherIndex].duration &&
                  notes[otherIndex].start < notes[index].start + notes[index].duration))) {
            $("edit-instructions").textContent = "Phrase alternative is stale or unresolved; regenerate guitar suggestions.";
            return;
          }
          saveSnapshot();
          redoHistory = [];
          tabPositions = positions.map((p, index) => ({ ...tabPositions[index],
            noteIndex: index, string: p.string, fret: p.fret, manual: true }));
          drawAll();
          renderSelected();
          markEdited();
          updateHistoryButtons();
        });
        fingeringContainer.appendChild(button);
      });
    }
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
    if (transcriptionBusy && !liveCaptureActive) return;
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
      position: { ...tabPositions[index] }, timelineDuration: totalDuration(),
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
    const deltaTime = deltaX / Math.max(1, rect.width - 48) * dragState.timelineDuration;
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
      if (fret < 0 || fret > (liveCaptureActive ? 24 : 36)) {
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

  function endDrag(event) {
    if (!dragState) return;
    const changed = dragState.changed;
    dragState = null;
    if (changed) {
      if (event?.type === "pointercancel") {
        restoreSnapshot(history.pop(), false);
        updateHistoryButtons();
        return;
      }
      markEdited();
      updateHistoryButtons();
    }
  }

  function insertNote(event) {
    if (transcriptionBusy && !liveCaptureActive) return;
    if (!scoreId && !liveCaptureActive) {
      $("status-text").textContent = "Create a project or transcribe audio before inserting notes.";
      return;
    }
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
    if (transcriptionBusy && !liveCaptureActive) return;
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
    if (transcriptionBusy && !liveCaptureActive) return;
    if (selectedIndex < 0) return;
    const midi = Number($("edit-pitch").value);
    const start = Number($("edit-start").value);
    const noteDuration = Number($("edit-duration").value);
    const string = Number($("edit-string").value);
    const fret = Number($("edit-fret").value);
    if (!Number.isInteger(midi) || midi < 0 || midi > 127 ||
        !Number.isFinite(start) || start < 0 ||
        !Number.isFinite(noteDuration) || noteDuration < .05 ||
        !Number.isInteger(string) || string < 0 || string > currentTuning().length ||
        !Number.isInteger(fret) || fret < 0 || fret > (liveCaptureActive ? 24 : 36)) {
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
    if (string > 0 && !pitchChanged && selectedPositionPitch !== note.midi) {
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
    if (string > 0 && expectedFret === fret && expectedFret >= 0) {
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

  function restoreSnapshot(snapshot, notify = true) {
    stopPlayback();
    snapshot = structuredClone(snapshot);
    notes = snapshot.notes;
    noteIndexDirty = true;
    tabPositions = snapshot.positions;
    selectedIndex = notes.findIndex((note) => note.id === snapshot.selectedId);
    pendingLiveDeletes = new Set(snapshot.pendingLiveDeletes || []);
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
    chooseNote(selectedIndex);
    if (notify) markEdited(true);
  }

  function undo() {
    if (transcriptionBusy && !liveCaptureActive) return;
    if (!history.length) return;
    saveSnapshot(redoHistory);
    const redo = redoHistory;
    restoreSnapshot(history.pop());
    redoHistory = redo;
    updateHistoryButtons();
  }

  function redo() {
    if (transcriptionBusy && !liveCaptureActive) return;
    if (!redoHistory.length) return;
    const remaining = redoHistory;
    const snapshot = remaining.pop();
    saveSnapshot(history);
    restoreSnapshot(snapshot);
    redoHistory = remaining;
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
      if (liveCaptureActive && prior?.user_authored && !incoming.user_authored) {
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
      const prior = previousPositions.get(note.id) ||
        liveCaptureSavedPositions?.get(note.id);
      if (Number.isInteger(note.string) && Number.isInteger(note.fret)) {
        return {
          string: note.string,
          fret: note.fret,
          manual: Boolean(note.manual_fingering),
        };
      }
      return prior ? { ...prior } : {};
    });
    if (liveCaptureSavedPositions &&
        [...liveCaptureSavedPositions.keys()].some(id => incomingIds.has(id))) {
      liveCaptureSavedPositions = null;
    }
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
    duration = notes.reduce((end, note) => Math.max(end, note.start + note.duration), 1);
    renderSelected();
    drawAll();
    $("export-edits").disabled = transcriptionBusy || !exportReady;
  }

  function fingeringSignature() {
    return JSON.stringify([currentTuning(), currentCapo(),
      notes.map(note => [note.id, note.midi, note.start, note.duration])]);
  }

  function acceptFingering(fingering) {
    if (!fingering || !Array.isArray(fingering.notes) ||
        fingering.notes.length !== notes.length) return;
    phraseFingerings = Array.isArray(fingering.alternatives) ? fingering.alternatives : [];
    phraseFingeringSignature = fingeringSignature();
    rejectedFingeringCorrectionCount =
      Array.isArray(fingering.rejected_corrections)
        ? fingering.rejected_corrections.length : 0;
    fingeringSearchTruncated = Boolean(fingering.search_truncated);
    tabPositions = fingering.notes.map((item, index) => ({
      noteIndex: index,
      string: item.string,
      fret: item.fret,
      manual: Boolean(item.manual),
      explanation: item.explanation || "",
      suggestedFinger: item.suggested_finger,
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
    stopPlayback();
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
    exportReady = Boolean(scoreId);
    $("play-midi").disabled = !exportReady || notes.length === 0;
    const fileMode = document.querySelector(".segment.selected")?.dataset.mode === "file";
    $("play-original").disabled =
      !exportReady || !fileMode || !/\.wav$/i.test($("input-path").value);
    edited = false;
    history = [];
    redoHistory = [];
    pendingLiveDeletes.clear();
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
    const nextWaveformMinimum = Array.isArray(message.waveform_minimum)
      ? message.waveform_minimum : nextWaveform.map(value => -Math.abs(value));
    const nextWaveformMaximum = Array.isArray(message.waveform_maximum)
      ? message.waveform_maximum : nextWaveform.map(value => Math.abs(value));
    const nextSpectrum = Array.isArray(message.spectrogram) ? message.spectrogram : [];
    const bands = Number(message.bands) || spectrumBands;
    const offset = Number(message.offset);
    const length = Number(message.duration);
    if (!Number.isFinite(offset) || offset < 0 || !Number.isFinite(length) || length < 0 ||
        nextWaveformMinimum.length !== nextWaveform.length ||
        nextWaveformMaximum.length !== nextWaveform.length ||
        nextWaveformMinimum.some((value, index) =>
          !Number.isFinite(value) || !Number.isFinite(nextWaveformMaximum[index]) ||
          value > nextWaveformMaximum[index]) ||
        nextSpectrum.length !== nextWaveform.length * bands) {
      $("status-text").textContent = "Audio preview rejected: invalid timing or dimensions.";
      return;
    }
    if (message.replace || bands !== spectrumBands) {
      waveform = [];
      waveformMinimum = [];
      waveformMaximum = [];
      spectrum = [];
      audioColumns = [];
    }
    spectrumBands = bands;
    spectrumMinimum = Number(message.minimum_frequency_hz) || 0;
    spectrumMaximum = Number(message.maximum_frequency_hz) || 0;
    const spans = nextWaveform.map((_, index) => ({
      start: offset + index * length / nextWaveform.length,
      end: offset + (index + 1) * length / nextWaveform.length,
    }));
    if (message.replace) {
      waveform = nextWaveform.slice();
      waveformMinimum = nextWaveformMinimum.slice();
      waveformMaximum = nextWaveformMaximum.slice();
      spectrum = nextSpectrum.slice();
      audioColumns = spans;
      audioStart = offset;
      audioEnd = audioStart + length;
    } else {
      const columns = Number(message.columns) || nextWaveform.length;
      const chunkDuration = Number(message.duration) || 0;
      const chunkOffset = Number(message.offset) || 0;
      waveform.push(...nextWaveform);
      waveformMinimum.push(...nextWaveformMinimum);
      waveformMaximum.push(...nextWaveformMaximum);
      spectrum.push(...nextSpectrum);
      audioColumns.push(...spans);
      const maximumColumns = 512;
      if (waveform.length > maximumColumns) {
        const remove = waveform.length - maximumColumns;
        waveform.splice(0, remove);
        waveformMinimum.splice(0, remove);
        waveformMaximum.splice(0, remove);
        spectrum.splice(0, remove * spectrumBands);
        audioColumns.splice(0, remove);
      }
      if (audioEnd === 0) audioStart = chunkOffset;
      audioEnd = Math.max(audioEnd, chunkOffset + chunkDuration);
      if (columns === 0) return;
    }
    audioStart = audioColumns[0]?.start ?? offset;
    $("audio-time").textContent = waveform.length
      ? `${audioStart.toFixed(1)}–${audioEnd.toFixed(1)}s` : "Waiting for audio";
    audioDirty = true;
    drawAll(false);
    window.dispatchEvent(new CustomEvent("transcribe:timeline", {
      detail: { duration: totalDuration() },
    }));
  }

  function drawPlaybackFrame() {
    if (!playbackMode) return;
    const time = playbackTime();
    window.dispatchEvent(new CustomEvent("transcribe:playback-time", {
      detail: time,
    }));
    updateMeasureDisplay(time);
    if (playbackMode === "midi" && time >= playbackEnd) {
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
    playbackFrame = requestAnimationFrame(drawPlaybackFrame);
  }

  async function playMidi(fromTime = 0) {
    stopPlayback();
    const generation = playbackGeneration;
    const AudioContextType = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextType) {
      $("status-text").textContent = "Web Audio is unavailable for note playback";
      return;
    }
    if (!audioContext) audioContext = new AudioContextType();
    $("stop-playback").disabled = false;
    try {
      await audioContext.resume();
    } catch (error) {
      if (generation !== playbackGeneration) return;
      throw error;
    }
    if (generation !== playbackGeneration) return;
    const startAt = audioContext.currentTime + .08;
    playbackEnd = totalDuration();
    playbackMode = "midi";
    pausedMode = "";
    pausedAt = 0;
    playbackStarted = startAt - fromTime;
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
      oscillator.onended = () => {
        oscillator.disconnect();
        gain.disconnect();
        activeOscillators = activeOscillators.filter((item) => item !== oscillator);
      };
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
      oscillator.stop();
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
    drawAll();
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
    originalPosition = 0;
    playbackEnd = Math.max(totalDuration(), audioEnd);
    $("stop-playback").disabled = false;
    $("play-midi").textContent = "▶";
    $("play-midi").title = "Play or pause the score (Space)";
    send("play-original", { token: String(playbackGeneration) });
    drawPlaybackFrame();
  }

  function stopPlayback() {
    playbackGeneration += 1;
    if (playbackMode === "original") send("stop-playback");
    activeOscillators.forEach((oscillator) => {
      oscillator.stop();
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
      tab_assignments: tabPositions.map((position, index) =>
        position.string > 0 ? `${index},${position.string},${position.fret}` : ""
      ).filter(Boolean).join(";"),
      tempo: String(currentTempo()),
      tempo_auto: $("tempo-auto").checked ? "1" : "0",
      key_fifths: $("key-control").value,
      key_auto: $("key-control").value === "auto" ? "1" : "0",
      minor: $("minor-control").checked ? "1" : "0",
      meter: $("meter-control").value,
      meter_auto: $("meter-control").value === "auto" ? "1" : "0",
      pickup: $("pickup-control").value,
      written_transposition: $("written-transposition-control").value,
      base_revision: String(scoreRevision),
      score_id: scoreId,
      review_decisions: reviewDecisions.map((decision) =>
        [decision.id, decision.note_id, decision.outcome].join(",")).join(";"),
      tuning: $("guitar-tuning").value,
      tuning_pitches: currentTuning().join(","),
      capo: String(currentCapo()),
      model: $("model-path").value,
      instrument: $("instrument-prior").value,
      quality: $("quality-mode").value,
      live_backend: $("live-backend").value,
      polyphonic: $("polyphonic").checked ? "1" : "0",
      preserve_timing: $("preserve-timing").checked ? "1" : "0",
      latency_ms: $("latency-ms").value,
      quantization: $("quantization-mode").value,
      quantization_strength: $("quantization-strength").value,
      minimum_rhythm_evidence: $("rhythm-evidence").value,
      retain_review_evidence: $("review-mode").checked ? "1" : "0",
      bounded_inference: $("bounded-inference").checked ? "1" : "0",
      annotation_text: $("project-annotation").value,
      musicxml: $("musicxml-path").value,
      tab: $("tab-path").value,
      midi: $("midi-path").value,
      json: $("json-path").value,
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
  $("split-note").addEventListener("click", () => {
    const note = notes[selectedIndex];
    if (!note || note.duration < .1 || (transcriptionBusy && !liveCaptureActive)) return;
    saveSnapshot();
    const half = note.duration / 2;
    const continuation = { ...note, id: `user-${scoreId || "score"}-${nextUserNoteId++}`,
      start: note.start + half, duration: half, user_authored: true };
    note.duration = half;
    notes.splice(selectedIndex + 1, 0, continuation);
    tabPositions.splice(selectedIndex + 1, 0, { ...tabPositions[selectedIndex] });
    markEdited();
    renderSelected();
    drawAll();
    updateHistoryButtons();
  });
  $("merge-note").addEventListener("click", () => {
    const note = notes[selectedIndex];
    if (!note || (transcriptionBusy && !liveCaptureActive)) return;
    const next = notes.reduce((candidate, item, index) =>
      item.midi === note.midi && item.start >= note.start + note.duration - 1e-6 &&
      (candidate < 0 || item.start < notes[candidate].start) ? index : candidate, -1);
    if (next < 0) {
      $("edit-instructions").textContent = "No following same-pitch note is available to merge.";
      return;
    }
    saveSnapshot();
    note.duration = notes[next].start + notes[next].duration - note.start;
    if (liveCaptureActive) pendingLiveDeletes.add(notes[next].id);
    notes.splice(next, 1);
    tabPositions.splice(next, 1);
    selectedIndex = notes.indexOf(note);
    markEdited();
    renderSelected();
    drawAll();
    updateHistoryButtons();
  });
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
        if (id === "tempo-control" || id === "tempo-auto") {
          remapTempo(contextBeforeLastChange || readScoreContext());
        }
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
    if (id === "tempo-control" || id === "tempo-auto") {
      remapTempo(contextBeforeLastChange || readScoreContext());
    }
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
    $("edit-string").innerHTML = '<option value="0">Unassigned</option>' + currentTuning().map((pitch, index) =>
      `<option value="${index + 1}">${index + 1} · ${noteName(pitch)}</option>`
    ).join("");
    if (selected) $("edit-string").value = selected;
  }

  window.addEventListener("transcribe:theme", () => {
    drawAll();
  });

  document.addEventListener("keydown", (event) => {
    if (event.defaultPrevented || document.querySelector("dialog[open]")) return;
    if (event.target instanceof Element &&
        event.target.closest("button, a, summary, [role=separator], [contenteditable=true]")) return;
    if (transcriptionBusy && !liveCaptureActive) return;
    const target = event.target;
    const typing = target instanceof HTMLInputElement ||
      target instanceof HTMLSelectElement || target instanceof HTMLTextAreaElement;
    if (!typing && !event.ctrlKey && !event.metaKey && ["+", "=", "-", "0", "PageUp", "PageDown"].includes(event.key)) {
      event.preventDefault();
      const control = { "+": "timeline-zoom-in", "=": "timeline-zoom-in",
        "-": "timeline-zoom-out", "0": "timeline-fit",
        PageUp: "timeline-prev", PageDown: "timeline-next" }[event.key];
      $(control).click();
    } else if (event.code === "Space" && !typing) {
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
    } else if (!typing && !event.ctrlKey && !event.metaKey && selectedIndex >= 0 &&
        ["ArrowLeft", "ArrowRight"].includes(event.key)) {
      event.preventDefault();
      chooseNote(Math.max(0, Math.min(notes.length - 1,
        selectedIndex + (event.key === "ArrowRight" ? 1 : -1))));
      const view = timeline.bounds(totalDuration());
      const note = notes[selectedIndex];
      if (note.start < view.start || note.start > view.end) {
        timeline.setView(note.start, view.end - view.start, totalDuration());
      }
    }
    updateHistoryButtons();
  });

  window.addEventListener("transcribe:notes", (event) => {
    noteIndexDirty = true;
    acceptNotes(event.detail);
  });
  window.addEventListener("transcribe:live-overlays", (event) => {
    liveGapRegions = Array.isArray(event.detail?.gaps) ? event.detail.gaps : [];
    drawAll();
  });
  window.addEventListener("transcribe:project-active", (event) => {
    projectActive = Boolean(event.detail?.active);
  });
  window.addEventListener("transcribe:project-settings", (event) => {
    currentProjectSettings = event.detail || null;
    const tuning = currentProjectSettings?.tuning;
    currentTuningOverride = Array.isArray(tuning) && tuning.length >= 1 && tuning.length <= 12 &&
      tuning.every((pitch) => Number.isInteger(pitch) && pitch >= 0 && pitch <= 127)
      ? tuning.map(Number) : null;
    refreshStringOptions();
    updateContextSummary();
    drawAll();
  });
  window.addEventListener("transcribe:project-save", (event) => {
    const detail = event.detail || {};
    window.dispatchEvent(new CustomEvent("transcribe:export-started"));
    send("save-project", scoreWriteFields({
      project_path: detail.path || "", copy_audio: detail.copyAudio ? "1" : "0",
    }));
  });
  window.addEventListener("transcribe:project-relink", (event) => {
    const detail = event.detail || {};
    send("autosave-project", scoreWriteFields({
      relink_audio: detail.path || "", generation: String(detail.generation || 0),
    }));
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
    stopPlayback();
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
  window.addEventListener("transcribe:playback-state", (event) => {
    const state = event.detail || {};
    if (Number(state.token) !== playbackGeneration || playbackMode !== "original") return;
    originalPosition = Math.max(0, Number(state.seconds) || 0);
    if (state.state === "ended" && $("loop-toggle").getAttribute("aria-pressed") === "true") {
      playOriginal();
    } else if (state.state !== "playing") {
      stopPlayback();
    }
  });
  window.addEventListener("transcribe:source-changed", () => {
    stopPlayback();
    waveform = [];
    waveformMinimum = [];
    waveformMaximum = [];
    spectrum = [];
    audioColumns = [];
    audioStart = 0;
    audioEnd = 0;
    audioDirty = true;
    $("audio-time").textContent = "Waiting for audio";
    $("play-original").disabled = true;
    drawAll();
  });
  window.addEventListener("transcribe:playback-error", stopPlayback);
  window.addEventListener("transcribe:busy", (event) => {
    const state = event.detail;
    const disabled = typeof state === "object"
      ? Boolean(state.busy) : Boolean(state);
    const preserveScore = typeof state === "object" && Boolean(state.preserveScore);
    const nextLiveCapture = typeof state === "object" &&
      Boolean(state.liveCapture);
    if (nextLiveCapture && !liveCaptureActive) {
      liveCaptureSavedPositions = new Map(notes.map((note, index) =>
        [note.id, tabPositions[index]]).filter(([id]) => id));
    }
    liveCaptureActive = nextLiveCapture;
    transcriptionBusy = disabled;
    if (disabled) stopPlayback();
    if (disabled && !preserveScore) {
      exportReady = false;
      edited = false;
      waveform = [];
      waveformMinimum = [];
      waveformMaximum = [];
      spectrum = [];
      audioColumns = [];
      audioStart = 0;
      audioEnd = 0;
      drawWaveform();
      drawSpectrogram();
    }
    $("play-midi").disabled = disabled || !exportReady || notes.length === 0;
    const fileMode = document.querySelector(".segment.selected")?.dataset.mode === "file";
    $("play-original").disabled =
      disabled || !exportReady || !fileMode || !/\.wav$/i.test($("input-path").value);
    $("apply-note-edit").disabled = (disabled && !liveCaptureActive) ||
      selectedIndex < 0;
    $("delete-note").disabled = (disabled && !liveCaptureActive) ||
      selectedIndex < 0;
    $("export-edits").disabled = disabled || !exportReady;
    updateHistoryButtons();
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
      ? "Accepted score exports completed"
      : `Export failed: ${message.error || "Unknown error"}`;
  });
  function invalidateLayout() { audioDirty = true; drawAll(); }
  window.addEventListener("resize", invalidateLayout);
  window.addEventListener("transcribe:viewport", invalidateLayout);
  window.addEventListener("transcribe:theme", invalidateLayout);
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
