(() => {
  const $ = (id) => document.getElementById(id);
  const inputs = {
    input: $("input-path"),
    model: $("model-path"),
    musicxml: $("musicxml-path"),
    tab: $("tab-path"),
    midi: $("midi-path"),
  };
  const colors = ["#74e4bd", "#a994ff", "#e9c578", "#6ccfe0", "#ff9b8e", "#96b5ff"];
  let mode = "file";
  let notes = [];
  let duration = 1;

  function send(action, values = {}) {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(`${action}|${new URLSearchParams(values).toString()}`);
    }
  }

  function setStatus(text, percent) {
    $("status-text").textContent = text;
    if (typeof percent === "number") {
      $("progress-value").textContent = `${percent}%`;
      $("progress-bar").style.width = `${percent}%`;
    }
  }

  function setBusy(busy) {
    $("transcribe-button").disabled = busy;
    $("cancel-button").classList.toggle("hidden", !busy || mode !== "asio");
    $("transcribe-button").querySelector("span:last-child").textContent =
      busy ? "Transcribing..." : "Transcribe audio";
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
    $("polyphonic").disabled = busy || !window.transcribeConfig?.onnx;
    $("open-folder-button").disabled = busy;
    $("cancel-button").textContent = "Stop capture";
    $("cancel-button").disabled = mode !== "asio";
  }

  function loadSavedPaths() {
    try {
      const saved = JSON.parse(localStorage.getItem("transcribe.paths") || "{}");
      Object.entries(inputs).forEach(([key, input]) => {
        if (saved[key]) input.value = saved[key];
      });
      if (saved.mode) selectMode(saved.mode);
      $("polyphonic").checked = Boolean(saved.polyphonic);
    } catch (error) {
      console.warn("Could not restore saved Transcribe paths.", error);
      $("status-text").textContent = "Saved paths could not be restored";
    }
  }

  function savePaths() {
    localStorage.setItem("transcribe.paths", JSON.stringify({
      input: inputs.input.value,
      model: inputs.model.value,
      musicxml: inputs.musicxml.value,
      tab: inputs.tab.value,
      midi: inputs.midi.value,
      mode,
      polyphonic: $("polyphonic").checked,
    }));
  }

  function selectMode(nextMode) {
    mode = nextMode;
    document.querySelectorAll(".segment").forEach((button) => {
      button.classList.toggle("selected", button.dataset.mode === mode);
    });
    $("file-source").classList.toggle("hidden", mode !== "file");
    $("asio-source").classList.toggle("hidden", mode !== "asio");
    savePaths();
  }

  function renderNotes(nextNotes, final = false) {
    notes = Array.isArray(nextNotes) ? nextNotes : [];
    duration = Math.max(1, ...notes.map((note) => note.start + note.duration));
    $("note-count").textContent = notes.length ? String(notes.length) : "—";
    $("empty-roll").classList.toggle("hidden", notes.length > 0);
    $("note-list").innerHTML = "";
    if (!notes.length) {
      $("note-list").innerHTML = '<div class="empty-notes">No notes detected yet.</div>';
    } else {
      const shown = notes.slice(-80).reverse();
      shown.forEach((note, index) => {
        const row = document.createElement("div");
        row.className = "note-row";
        const confidence = Math.round(Math.max(0, Math.min(1, note.confidence)) * 100);
        row.innerHTML = `<div class="note-main"><i class="note-color"></i><span class="note-name"></span><span class="note-time"></span></div><span class="note-time"></span><span class="note-confidence"></span>`;
        row.querySelector(".note-color").style.background = colors[(note.midi % colors.length + colors.length) % colors.length];
        row.querySelector(".note-name").textContent = note.name;
        row.querySelector(".note-time").textContent = `${note.start.toFixed(2)}s`;
        row.children[1].textContent = `${note.duration.toFixed(2)}s`;
        row.children[2].textContent = `${confidence}%`;
        $("note-list").appendChild(row);
      });
    }
    if (final) $("preview-badge").textContent = "COMPLETE";
    drawRoll();
  }

  function drawRoll() {
    const canvas = $("piano-roll");
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const scale = window.devicePixelRatio || 1;
    canvas.width = Math.round(rect.width * scale);
    canvas.height = Math.round(rect.height * scale);
    const context = canvas.getContext("2d");
    context.scale(scale, scale);
    const width = rect.width;
    const height = rect.height;
    context.fillStyle = "#0c141a";
    context.fillRect(0, 0, width, height);
    if (!notes.length) return;
    const low = Math.min(...notes.map((note) => note.midi));
    const high = Math.max(...notes.map((note) => note.midi));
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
    notes.forEach((note, index) => {
      const x = Math.max(0, note.start / duration * width);
      const w = Math.max(2, note.duration / duration * width);
      const y = height - ((note.midi + .5 - minPitch) / range) * height;
      const alpha = .52 + Math.max(0, Math.min(1, note.confidence)) * .44;
      const color = colors[(note.midi % colors.length + colors.length) % colors.length];
      context.globalAlpha = alpha;
      context.fillStyle = color;
      context.beginPath();
      const radius = Math.min(3, w / 2, pitchHeight / 2);
      context.roundRect(x, y - Math.max(2, pitchHeight * .33), w, Math.max(4, pitchHeight * .66), radius);
      context.fill();
      context.globalAlpha = 1;
    });
    $("pitch-high").textContent = notes.reduce((a, b) => a.midi > b.midi ? a : b).name;
    $("pitch-low").textContent = notes.reduce((a, b) => a.midi < b.midi ? a : b).name;
    $("roll-end").textContent = `${duration.toFixed(1)}s`;
    $("roll-duration").textContent = "Time";
  }

  function showError(message) {
    $("preview-badge").textContent = "ERROR";
    $("preview-badge").className = "preview-badge error";
    $("result-message").textContent = message;
    $("result-message").className = "result-message error";
    setStatus(message, 0);
  }

  function handleMessage(event) {
    const message = event.data || {};
    if (message.type === "ready") {
      window.transcribeConfig = message;
      $("asio-mode").disabled = !message.asio;
      document.querySelectorAll('[data-field="model"]').forEach((button) => button.disabled = !message.onnx);
      $("model-path").disabled = !message.onnx;
      $("polyphonic").disabled = !message.onnx;
      if (message.ffmpeg) $("drop-zone").classList.add("can-drop");
      $("engine-label").textContent = message.asio ? "C++ · YIN · ASIO ready" : "C++ analysis engine";
      return;
    }
    if (message.type === "field") {
      const input = inputs[message.field];
      if (input) {
        input.value = message.value;
        savePaths();
      }
      return;
    }
    if (message.type === "progress") {
      $("preview-badge").textContent = "ANALYZING";
      $("preview-badge").className = "preview-badge active";
      setStatus(message.status || "Analyzing audio", message.progress);
      return;
    }
    if (message.type === "live") {
      renderNotes(message.notes);
      $("preview-badge").textContent = "LISTENING";
      $("preview-badge").className = "preview-badge active";
      return;
    }
    if (message.type === "complete") {
      setBusy(false);
      if (message.success) {
        $("preview-badge").textContent = "COMPLETE";
        $("preview-badge").className = "preview-badge done";
        $("note-count").textContent = String(message.notes.length);
        $("tempo-value").textContent = message.metadata.tempo_inferred
          ? `${message.metadata.tempo.toFixed(0)} bpm` : "—";
        $("key-value").textContent = message.metadata.key || "—";
        $("result-message").textContent = message.summary;
        $("result-message").className = "result-message";
        renderNotes(message.notes, true);
        setStatus("Transcription complete", 100);
      } else {
        showError(message.error || "Transcription failed.");
      }
      return;
    }
    if (message.type === "error") {
      setBusy(false);
      showError(message.message || "The application encountered an error.");
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
  $("drop-browse").addEventListener("click", () => send("browse", { field: "input", current: inputs.input.value }));
  $("transcribe-button").addEventListener("click", () => {
    if (mode === "file" && !inputs.input.value.trim()) {
      showError("Choose an audio file first.");
      return;
    }
    if (!inputs.musicxml.value.trim() || !inputs.tab.value.trim()) {
      showError("Choose both MusicXML and guitar tab output paths.");
      return;
    }
    if (mode === "asio" && (!(Number($("capture-seconds").value) > 0) || Number($("capture-seconds").value) > 300)) {
      showError("Enter a capture duration greater than 0 and no more than 300 seconds.");
      return;
    }
    savePaths();
    $("result-message").className = "result-message hidden";
    $("preview-badge").textContent = "STARTING";
    $("preview-badge").className = "preview-badge active";
    setStatus(mode === "asio" ? "Connecting to ASIO input..." : "Preparing audio...", 0);
    setBusy(true);
    send("transcribe", {
      mode,
      input: inputs.input.value,
      duration: $("capture-seconds").value,
      model: inputs.model.value,
      polyphonic: $("polyphonic").checked ? "1" : "0",
      musicxml: inputs.musicxml.value,
      tab: inputs.tab.value,
      midi: inputs.midi.value,
    });
  });
  $("cancel-button").addEventListener("click", () => send("cancel"));
  $("open-folder-button").addEventListener("click", () => send("open-output", { path: inputs.musicxml.value }));
  Object.values(inputs).forEach((input) => input.addEventListener("change", savePaths));
  window.addEventListener("resize", drawRoll);
  window.addEventListener("DOMContentLoaded", () => {
    loadSavedPaths();
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.addEventListener("message", handleMessage);
      send("ready");
    }
  });
})();
