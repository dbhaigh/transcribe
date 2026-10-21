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
})();
