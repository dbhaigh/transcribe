(() => {
  const $ = (id) => document.getElementById(id);
  const host = $("engraving-host");
  let worker = null;
  let enabled = false;
  let requestId = 0;
  let acceptedNotes = [];
  let noteByEncodedId = new Map();
  let acceptedRevision = 0;
  let selectedNoteId = "";
  let zoom = 1;
  let followedNoteId = "";
  let activeCursor = null;
  let cursorNoteId = "";
  let rendering = false;
  let pendingScore = null;
  let latestScore = null;
  const pageByNoteId = new Map();
  const pageSources = new WeakMap();
  const observer = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      const page = entry.target;
      const svg = pageSources.get(page);
      if (entry.isIntersecting && svg && !page.firstChild) {
        page.innerHTML = svg;
        markSelectedNote();
      } else if (!entry.isIntersecting && page.firstChild) {
        page.style.minHeight = `${page.getBoundingClientRect().height}px`;
        page.replaceChildren();
        if (activeCursor && !activeCursor.isConnected) {
          activeCursor = null;
          cursorNoteId = "";
        }
      }
    }
  }, { rootMargin: "600px" });

  function setStatus(message) {
    $("engraving-status").textContent = message;
  }

  function setZoom(nextZoom) {
    zoom = Math.max(.6, Math.min(2.2, nextZoom));
    host.querySelectorAll(".engraving-page").forEach((page) => {
      page.style.width = `${100 * zoom}%`;
    });
    ["staff-view", "tab-view"].forEach((id) => {
      $(id).style.zoom = String(zoom);
    });
    $("engraving-fit").textContent = zoom === 1 ? "Fit" : `${Math.round(zoom * 100)}%`;
  }

  function disableRenderer(message) {
    enabled = false;
    $("engraving-layout").disabled = true;
    document.body.classList.remove("verovio-enabled");
    host.classList.add("hidden");
    $("score-hint").textContent = message;
  }

  function mountNotePage(id) {
    const page = pageByNoteId.get(encodeNoteId(id));
    if (page && !page.firstChild) page.innerHTML = pageSources.get(page) || "";
    return page;
  }

  function initializeRenderer(config) {
    if (!config || !config.verovio || enabled) return;
    try {
      worker = new Worker("notation-worker.js", { type: "module" });
    } catch (error) {
      disableRenderer(`Engraving unavailable: ${error.message}`);
      return;
    }
    enabled = true;
    document.body.classList.add("verovio-enabled");
    document.body.classList.add("engraving-draft");
    host.classList.remove("hidden");
    setStatus("Offline renderer ready · waiting for a completed score");
    worker.addEventListener("message", (event) => {
      const message = event.data || {};
      rendering = false;
      if (pendingScore) {
        const next = pendingScore;
        pendingScore = null;
        renderAcceptedScore(next);
        return;
      }
      if (message.requestId !== requestId) return;
      if (message.type === "error") {
        setStatus(`Engraving failed: ${message.message}`);
        $("score-hint").textContent =
          "Engraving failed; the editable canvas remains available.";
        host.innerHTML = "";
        host.classList.add("hidden");
        document.body.classList.add("engraving-draft");
        return;
      }
      if (message.type !== "rendered" || !Array.isArray(message.pages)) return;
      activeCursor = null;
      cursorNoteId = "";
      pageByNoteId.clear();
      const existing = Array.from(host.children);
      message.pages.forEach((svg, index) => {
        const page = existing[index] || document.createElement("div");
        page.className = "engraving-page";
        if (!existing[index]) page.style.minHeight = "600px";
        page.dataset.page = String(index + 1);
        if (pageSources.get(page) !== svg) {
          pageSources.set(page, svg);
          if (index === 0 || page.firstChild) page.innerHTML = svg;
        }
        if (!page.isConnected) host.appendChild(page);
        for (const match of svg.matchAll(/\bid="(?:tab-)?score-note-([0-9a-f]+)(?:-continuation-\d+)?"/g)) {
          if (!pageByNoteId.has(match[1])) pageByNoteId.set(match[1], page);
        }
        observer.observe(page);
      });
      existing.slice(message.pages.length).forEach(page => { observer.unobserve(page); page.remove(); });
      setZoom(zoom);
      const xml = new DOMParser().parseFromString(latestScore.musicxml, "application/xml");
      const untyped = [...xml.querySelectorAll("note")].filter(note =>
        note.querySelector("duration") && !note.querySelector("type")).length;
      const unresolvedTab = [...xml.querySelectorAll("part")].filter(part =>
        part.querySelector("clef > sign")?.textContent === "TAB")
        .flatMap(part => [...part.querySelectorAll("note")])
        .filter(note => note.querySelector("pitch") && !note.querySelector("technical > string")).length;
      const warnings = [
        untyped ? `${untyped} unquantized duration(s): written rhythm is approximate` : "",
        unresolvedTab ? `${unresolvedTab} unresolved tab position(s) cannot be engraved: assign string/fret` : "",
        message.diagnostics ? `Renderer: ${message.diagnostics}` : "",
      ].filter(Boolean);
      setStatus(`Accepted score revision ${acceptedRevision} · ${message.pages.length} page(s)` +
        (warnings.length ? ` · ${warnings.join(" · ")}` : ""));
      $("engraving-layout").disabled = false;
      $("engraving-status").title = warnings.join("\n");
      $("score-hint").textContent = "Select an engraved note to edit";
      document.body.classList.remove("engraving-draft");
      markSelectedNote();
    });
    worker.addEventListener("error", (event) => {
      disableRenderer(`Engraving worker failed: ${event.message || "unknown error"}`);
    });
    host.addEventListener("click", (event) => {
      const element = event.target.closest('[id^="score-"], [id^="tab-score-"]');
      if (!element) return;
      const note = findCanonicalNote(element.id);
      if (!note) return;
      selectedNoteId = note.id;
      markSelectedNote();
      window.dispatchEvent(new CustomEvent("transcribe:engraving-select", {
        detail: { id: note.id },
      }));
    });
  }

  function findCanonicalNote(svgId) {
    const prefix = svgId.startsWith("tab-score-note-")
      ? "tab-score-note-"
      : svgId.startsWith("score-note-") ? "score-note-" : "";
    if (!prefix) return null;
    let encodedId = svgId.slice(prefix.length);
    const continuation = encodedId.indexOf("-continuation-");
    if (continuation >= 0) encodedId = encodedId.slice(0, continuation);
    return noteByEncodedId.get(encodedId) || null;
  }

  function encodeNoteId(id) {
    return Array.from(new TextEncoder().encode(id), (byte) =>
      byte.toString(16).padStart(2, "0")).join("");
  }

  function markSelectedNote() {
    host.querySelectorAll(".engraving-selected").forEach((element) => {
      element.classList.remove("engraving-selected");
    });
    if (!selectedNoteId) return;
    const note = acceptedNotes.find((item) => item.id === selectedNoteId);
    if (!note) return;
    host.querySelectorAll("[id^='score-'], [id^='tab-score-']").forEach((element) => {
      const mapped = findCanonicalNote(element.id);
      if (mapped && mapped.id === selectedNoteId) {
        element.classList.add("engraving-selected");
      }
    });
  }

  function renderAcceptedScore(message) {
    if (!enabled || !worker || !message ||
        typeof message.musicxml !== "string" || !message.musicxml) return;
    if (rendering) { pendingScore = message; return; }
    acceptedNotes = Array.isArray(message.notes) ? message.notes : [];
    latestScore = message;
    noteByEncodedId = new Map(acceptedNotes.map((note) =>
      [encodeNoteId(note.id), note]));
    acceptedRevision = Number(
      message.score_revision ??
      message.metadata?.score_revision ??
      message.accepted_revision
    ) || 0;
    if (!acceptedNotes.some((note) => note.id === selectedNoteId)) {
      selectedNoteId = "";
    }
    document.body.classList.remove("engraving-draft");
    setStatus(`Rendering accepted score revision ${acceptedRevision}…`);
    requestId += 1;
    rendering = true;
    worker.postMessage({ type: "render", requestId, musicxml: message.musicxml,
      layout: $("engraving-layout").value });
  }

  function updatePlayback(time) {
    if (!enabled || !Number.isFinite(time) ||
        document.body.classList.contains("engraving-draft")) {
      activeCursor?.remove(); activeCursor = null; cursorNoteId = ""; return;
    }
    const note = acceptedNotes.find((item) =>
      time >= item.start && time < item.start + item.duration);
    if (!note) { activeCursor?.remove(); activeCursor = null; cursorNoteId = ""; return; }
    if (cursorNoteId === note.id && activeCursor?.isConnected) return;
    if ($("follow-playhead").checked) mountNotePage(note.id);
    const group = host.querySelector(
      `#${CSS.escape(`score-note-${encodeNoteId(note.id)}`)}`);
    if (!group || typeof group.getBBox !== "function") return;
    if ($("follow-playhead").checked && followedNoteId !== note.id) {
      group.scrollIntoView({ block: "nearest", inline: "center", behavior: "smooth" });
      followedNoteId = note.id;
    }
    const svg = group.ownerSVGElement;
    if (!svg) return;
    const bounds = group.getBBox();
    const view = svg.viewBox.baseVal;
    activeCursor?.remove();
    const cursor = document.createElementNS("http://www.w3.org/2000/svg", "line");
    cursor.setAttribute("class", "engraving-playhead");
    cursor.setAttribute("x1", String(bounds.x + bounds.width / 2));
    cursor.setAttribute("x2", String(bounds.x + bounds.width / 2));
    cursor.setAttribute("y1", String(view.y));
    cursor.setAttribute("y2", String(view.y + view.height));
    svg.appendChild(cursor);
    activeCursor = cursor;
    cursorNoteId = note.id;
  }

  $("engraving-zoom-out").addEventListener("click", () => setZoom(zoom - .15));
  $("engraving-zoom-in").addEventListener("click", () => setZoom(zoom + .15));
  $("engraving-fit").addEventListener("click", () => setZoom(1));
  $("engraving-layout").addEventListener("change", () => {
    if (latestScore && !document.body.classList.contains("engraving-draft")) {
      renderAcceptedScore(latestScore);
    }
  });
  window.addEventListener("transcribe:config", (event) => initializeRenderer(event.detail));
  window.addEventListener("transcribe:accepted-score", (event) =>
    renderAcceptedScore(event.detail));
  window.addEventListener("transcribe:edited", () => {
    if (!enabled) return;
    requestId += 1;
    pendingScore = null;
    $("engraving-layout").disabled = true;
    updatePlayback(NaN);
    document.body.classList.add("engraving-draft");
    $("score-hint").textContent =
      `Draft edits · engraving shows accepted revision ${acceptedRevision}`;
  });
  window.addEventListener("transcribe:score-selection", (event) => {
    selectedNoteId = event.detail && event.detail.id ? event.detail.id : "";
    if (selectedNoteId) {
      mountNotePage(selectedNoteId)?.scrollIntoView({ block: "nearest" });
    }
    markSelectedNote();
  });
  window.addEventListener("transcribe:edited-export", (event) => {
    if (event.detail && event.detail.success) renderAcceptedScore(event.detail);
  });
  window.addEventListener("transcribe:playback-time", (event) =>
    updatePlayback(Number(event.detail)));
  window.addEventListener("transcribe:playback-stop", () => updatePlayback(NaN));
})();
