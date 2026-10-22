(() => {
  const $ = (id) => document.getElementById(id);
  const workspace = $("workspace");
  const limits = {
    source: { min: 220, max: 480, fallback: 286 },
    inspector: { min: 250, max: 520, fallback: 316 },
  };
  let widths = { source: limits.source.fallback, inspector: limits.inspector.fallback };

  function saveLayout() {
    localStorage.setItem("transcribe.workspace.layout", JSON.stringify(widths));
  }

  function setWidth(panel, width, persist = true) {
    const limit = limits[panel];
    widths[panel] = Math.max(limit.min, Math.min(limit.max, width));
    workspace.style.setProperty(`--${panel}-width`, `${widths[panel]}px`);
    const separator = $(`${panel}-resizer`);
    separator.setAttribute("aria-valuemin", String(limit.min));
    separator.setAttribute("aria-valuemax", String(limit.max));
    separator.setAttribute("aria-valuenow", String(widths[panel]));
    if (persist) saveLayout();
  }

  function setCollapsed(panel, collapsed) {
    const className = panel === "source" ? "source-collapsed" : "inspector-collapsed";
    const expanded = !collapsed;
    workspace.classList.toggle(className, collapsed);
    const topButton = $(`toggle-${panel}`);
    const panelButton = $(`${panel}-collapse`);
    [topButton, panelButton].forEach((button) => {
      button.setAttribute("aria-expanded", String(expanded));
      button.setAttribute("aria-label",
        `${expanded ? "Collapse" : "Expand"} ${panel === "source" ? "source panel" : "inspector panel"}`);
      button.title = `${expanded ? "Collapse" : "Expand"} ${panel === "source" ? "source panel" : "inspector"}`;
    });
    localStorage.setItem(`transcribe.workspace.${panel}-collapsed`, String(collapsed));
  }

  function togglePanel(panel) {
    const className = panel === "source" ? "source-collapsed" : "inspector-collapsed";
    setCollapsed(panel, !workspace.classList.contains(className));
  }

  function resizePanel(panel, startX, startWidth, currentX) {
    const direction = panel === "source" ? 1 : -1;
    setWidth(panel, startWidth + (currentX - startX) * direction, false);
  }

  function bindResizer(panel) {
    const separator = $(`${panel}-resizer`);
    separator.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.preventDefault();
      separator.setPointerCapture(event.pointerId);
      const startX = event.clientX;
      const startWidth = widths[panel];
      const move = (moveEvent) => resizePanel(panel, startX, startWidth, moveEvent.clientX);
      const finish = () => {
        separator.removeEventListener("pointermove", move);
        separator.removeEventListener("pointerup", finish);
        separator.removeEventListener("pointercancel", finish);
        saveLayout();
      };
      separator.addEventListener("pointermove", move);
      separator.addEventListener("pointerup", finish);
      separator.addEventListener("pointercancel", finish);
    });
    separator.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") return;
      event.preventDefault();
      const direction = (event.key === "ArrowRight" ? 1 : -1) *
        (panel === "source" ? 1 : -1);
      setWidth(panel, widths[panel] + direction * (event.shiftKey ? 24 : 8));
    });
  }

  function setTheme(theme, persist = true) {
    const next = theme === "light" ? "light" : "dark";
    document.body.dataset.theme = next;
    $("theme-toggle").textContent = next === "light" ? "Dark theme" : "Light theme";
    $("theme-toggle").setAttribute("aria-label",
      `Switch to ${next === "light" ? "dark" : "light"} theme`);
    if (persist) localStorage.setItem("transcribe.theme", next);
    window.dispatchEvent(new CustomEvent("transcribe:theme", { detail: next }));
  }

  try {
    const saved = JSON.parse(localStorage.getItem("transcribe.workspace.layout") || "{}");
    Object.keys(widths).forEach((panel) => {
      const candidate = Number(saved[panel]);
      if (Number.isFinite(candidate)) widths[panel] = candidate;
      setWidth(panel, widths[panel], false);
    });
    ["source", "inspector"].forEach((panel) => {
      setCollapsed(panel,
        localStorage.getItem(`transcribe.workspace.${panel}-collapsed`) === "true");
      bindResizer(panel);
      $(`toggle-${panel}`).addEventListener("click", () => togglePanel(panel));
      $(`${panel}-collapse`).addEventListener("click", () => togglePanel(panel));
    });
    setTheme(localStorage.getItem("transcribe.theme") || "dark", false);
  } catch (error) {
    console.warn("Workspace preferences could not be restored.", error);
    setWidth("source", limits.source.fallback, false);
    setWidth("inspector", limits.inspector.fallback, false);
    setTheme("dark", false);
  }

  $("theme-toggle").addEventListener("click", () => {
    setTheme(document.body.dataset.theme === "light" ? "dark" : "light");
  });

  const help = {
    "instrument-prior": "Limits or biases candidate notes where supported. It does not separate instruments in a mix or prove which instrument played a note.",
    "quality-mode": "Full analysis and Fast preview use existing backend configurations. Fast preview trades analysis work for speed and may miss notes. A selected ONNX model keeps its own analysis contract.",
    "model-path": "Optional local ONNX model. No weights are downloaded automatically. The model must match a supported adapter; this build reports unavailable integrations explicitly.",
    "guitar-tuning": "Open-string sounding pitches, ordered from string 1 (highest) down. Tuning constrains suggested fingerings; audio cannot usually establish the performed string.",
    "capo-fret": "The capo raises every open string. Exported fret positions are relative to the capo; sounding pitch remains authoritative.",
    "quantization-mode": "Legacy uses the existing grid. Evidence-aware uses timing support. Preserve measured timing avoids forcing ambiguous attacks onto a grid; notation may be less conventional.",
    "quantization-strength": "0 retains measured timing; 1 applies the supported grid fully where the selected rhythm mode permits. This does not change the source audio.",
    "tempo-control": "Beats per minute. Auto infers tempo when evidence permits; otherwise a documented default is used. Editing accepted tempo preserves musical positions, so projected seconds can change.",
    "meter-control": "Beats grouped per measure (for example 6/8). Auto is inferred, not guaranteed. An explicit value overrides inference and affects bar boundaries.",
    "detected-confidence": "Only a validated calibration may be called a correctness probability. Raw activity, onset and stability scores are heuristic evidence, not percentages of certainty.",
  };
  Object.entries(help).forEach(([id, text]) => {
    const control = $(id);
    const label = document.querySelector(`label[for="${id}"]`) ||
      (id === "detected-confidence" ? control.previousElementSibling : null);
    if (!label) return;
    const description = document.createElement("p");
    description.id = `${id}-help`;
    description.className = "context-help";
    description.hidden = true;
    description.textContent = text;
    const button = document.createElement("button");
    button.type = "button";
    button.className = "help-tip";
    button.textContent = "?";
    button.title = text;
    button.setAttribute("aria-label", `Explain ${label.textContent.trim()}`);
    button.setAttribute("aria-controls", description.id);
    button.setAttribute("aria-expanded", "false");
    button.addEventListener("click", (event) => {
      event.preventDefault();
      description.hidden = !description.hidden;
      button.setAttribute("aria-expanded", String(!description.hidden));
    });
    label.append(button);
    // Keep disclosure text outside form labels and compact transport rows.
    const container = id === "tempo-control" || id === "meter-control"
      ? control.closest(".transport-input") : label.parentElement;
    container.append(description);
    control.setAttribute("aria-describedby", description.id);
    control.title = text;
  });

  $("help-button").addEventListener("click", () => $("help-dialog").showModal());
  $("close-help").addEventListener("click", () => $("help-dialog").close());
  $("hide-guide").addEventListener("click", () => {
    $("journey-guide").hidden = true;
    localStorage.setItem("transcribe.guide.hidden", "true");
  });
  $("show-guide").addEventListener("click", () => {
    $("journey-guide").hidden = false;
    localStorage.setItem("transcribe.guide.hidden", "false");
    $("help-dialog").close();
    $("journey-guide").scrollIntoView({ block: "nearest" });
    $("hide-guide").focus();
  });
  $("journey-guide").hidden = localStorage.getItem("transcribe.guide.hidden") === "true";
  document.addEventListener("keydown", (event) => {
    if (event.key === "F1") {
      event.preventDefault();
      if (!document.querySelector("dialog[open]")) $("help-dialog").showModal();
    }
  });
})();
