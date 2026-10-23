(() => {
  const tabs = Array.from(document.querySelectorAll("[data-display-tab]"));
  const noteLegend = document.getElementById("note-legend");
  const notationZoom = document.getElementById("score-zoom-controls");

  function activate(tab, focus = false) {
    const view = tab.dataset.displayTab;
    tabs.forEach((candidate) => {
      const selected = candidate === tab;
      candidate.setAttribute("aria-selected", String(selected));
      candidate.tabIndex = selected ? 0 : -1;
      document.getElementById(candidate.getAttribute("aria-controls")).hidden = !selected;
    });
    noteLegend.hidden = view === "waveform" || view === "spectrogram";
    notationZoom.hidden = view !== "standard-notation" && view !== "guitar-tablature";
    if (focus) tab.focus();
    window.dispatchEvent(new Event("resize"));
  }

  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => activate(tab));
    tab.addEventListener("keydown", (event) => {
      let nextIndex = index;
      if (event.key === "ArrowRight") nextIndex = (index + 1) % tabs.length;
      else if (event.key === "ArrowLeft") nextIndex = (index - 1 + tabs.length) % tabs.length;
      else if (event.key === "Home") nextIndex = 0;
      else if (event.key === "End") nextIndex = tabs.length - 1;
      else return;
      event.preventDefault();
      activate(tabs[nextIndex], true);
    });
  });

  const selected = tabs.find((tab) => tab.getAttribute("aria-selected") === "true") || tabs[0];
  activate(selected);
})();
