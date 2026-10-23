(() => {
  const finite = (value, name) => {
    if (!Number.isFinite(value) || value < 0) throw new Error(`Invalid ${name}`);
    return value;
  };
  function mapPosition(value, segments, tempo, inverse) {
    finite(value, "timeline position");
    const from = inverse ? "start_seconds" : "start_quarters";
    const to = inverse ? "start_quarters" : "start_seconds";
    let segment = { start_seconds: 0, start_quarters: 0, beats_per_minute: tempo };
    for (const candidate of segments || []) {
      if (candidate[from] > value) break;
      segment = candidate;
    }
    if (!Number.isFinite(segment.beats_per_minute) || segment.beats_per_minute <= 0) {
      throw new Error("Invalid timeline tempo");
    }
    const bpm = Math.max(1, Math.min(1000, segment.beats_per_minute));
    return segment[to] + (value - segment[from]) * (inverse ? bpm / 60 : 60 / bpm);
  }
  let viewport = null;
  let sourceDuration = 1;
  let cursor = null;
  function bounds(duration) {
    return viewport || { start: 0, end: Math.max(1, duration) };
  }
  function x(seconds, duration, width) {
    const view = bounds(duration);
    return 36 + (seconds - view.start) / (view.end - view.start) * Math.max(1, width - 48);
  }
  function time(point, duration, width) {
    const view = bounds(duration);
    return Math.max(view.start, view.start + (point - 36) /
      Math.max(1, width - 48) * (view.end - view.start));
  }
  function updateCursors() {
    document.querySelectorAll(".time-surface").forEach((surface) => {
      const line = surface.querySelector(".timeline-playhead");
      const canvas = surface.querySelector("canvas");
      if (!line || !canvas) return;
      const view = bounds(sourceDuration);
      line.hidden = !Number.isFinite(cursor) || cursor < view.start || cursor > view.end;
      line.style.left = `${canvas.offsetLeft + x(cursor || 0, sourceDuration, canvas.clientWidth)}px`;
      line.style.top = `${canvas.offsetTop}px`;
      line.style.height = `${canvas.clientHeight}px`;
      line.style.bottom = "auto";
    });
  }
  function setView(start, span, duration = sourceDuration, manual = false) {
    sourceDuration = Math.max(1, duration);
    if (span === null) viewport = null;
    else {
      const length = Math.max(.25, Math.min(sourceDuration, span));
      const first = Math.max(0, Math.min(sourceDuration - length, start));
      viewport = { start: first, end: first + length };
    }
    updateControls();
    const view = bounds(sourceDuration);
    window.dispatchEvent(new CustomEvent("transcribe:viewport", { detail: view }));
    if (manual) {
      window.dispatchEvent(new CustomEvent("transcribe:viewport-manual", { detail: view }));
    }
    updateCursors();
  }
  function updateControls() {
    const view = bounds(sourceDuration);
    const label = document.getElementById("timeline-window");
    if (label) label.textContent = `${view.start.toFixed(1)}–${view.end.toFixed(1)} s`;
    const slider = document.getElementById("timeline-scroll");
    if (slider) {
      slider.max = String(Math.max(0, sourceDuration - (view.end - view.start)));
      slider.value = String(view.start);
      slider.disabled = Number(slider.max) <= 0;
      slider.setAttribute("aria-valuetext", `${view.start.toFixed(2)} seconds; visible through ${view.end.toFixed(2)} seconds`);
    }
  }
  window.TranscribeTimeline = Object.freeze({
    seconds: (quarters, segments, tempo = 120) => mapPosition(quarters, segments, tempo, false),
    quarters: (seconds, segments, tempo = 120) => mapPosition(seconds, segments, tempo, true),
    sampleSeconds(frame, rate) {
      finite(frame, "sample position");
      if (!Number.isSafeInteger(frame) || !Number.isInteger(rate) || rate <= 0) {
        throw new Error("Sample conversion requires an exact frame and known sample rate");
      }
      return frame / rate;
    },
    samplePosition(seconds, rate) {
      finite(seconds, "recording seconds");
      if (!Number.isInteger(rate) || rate <= 0) throw new Error("Unknown sample rate");
      const frame = Math.floor(seconds * rate + .5);
      if (!Number.isSafeInteger(frame)) throw new Error("Sample position exceeds JavaScript precision");
      return frame;
    },
    x, time, bounds, setView,
    index(notes) {
      const ordered = notes.map((note, index) => ({ note, index }))
        .sort((a, b) => a.note.start - b.note.start);
      let maximumEnd = 0;
      const ends = ordered.map(({ note }) => (maximumEnd =
        Math.max(maximumEnd, note.start + note.duration)));
      return (duration) => {
        const view = bounds(duration);
        let low = 0, high = ends.length;
        while (low < high) {
          const middle = (low + high) >>> 1;
          if (ends[middle] < view.start) low = middle + 1; else high = middle;
        }
        const result = [];
        for (let i = low; i < ordered.length && ordered[i].note.start <= view.end; i += 1) {
          if (ordered[i].note.start + ordered[i].note.duration >= view.start) result.push(ordered[i]);
        }
        return result;
      };
    },
  });
  window.addEventListener("DOMContentLoaded", () => {
    ["waveform", "spectrogram", "staff-view", "tab-view", "piano-roll"].forEach((id) => {
      const canvas = document.getElementById(id);
      const surface = canvas.parentElement;
      surface.classList.add("time-surface");
      const line = document.createElement("span");
      line.className = "timeline-playhead";
      line.hidden = true;
      line.setAttribute("aria-hidden", "true");
      surface.appendChild(line);
      new ResizeObserver(updateCursors).observe(canvas);
    });
    document.getElementById("timeline-zoom-in").addEventListener("click", () => {
      const view = bounds(sourceDuration);
      setView(cursor ?? view.start, (view.end - view.start) / 2, sourceDuration, true);
    });
    document.getElementById("timeline-zoom-out").addEventListener("click", () => {
      const view = bounds(sourceDuration);
      setView(view.start, (view.end - view.start) * 2, sourceDuration, true);
    });
    document.getElementById("timeline-fit").addEventListener("click", () =>
      setView(0, null, sourceDuration, true));
    document.getElementById("timeline-scroll").addEventListener("input", (event) => {
      const view = bounds(sourceDuration);
      setView(Number(event.target.value), view.end - view.start, sourceDuration, true);
    });
    document.querySelectorAll(".time-surface").forEach((surface) => {
      surface.addEventListener("wheel", (event) => {
        if (!event.shiftKey && !event.deltaX) return;
        const view = bounds(sourceDuration);
        if (!viewport) return;
        event.preventDefault();
        setView(view.start + (event.deltaX || event.deltaY) /
          Math.max(1, surface.clientWidth) * (view.end - view.start),
          view.end - view.start, sourceDuration, true);
      }, { passive: false });
    });
    for (const direction of [-1, 1]) {
      document.getElementById(direction < 0 ? "timeline-prev" : "timeline-next").addEventListener("click", () => {
        const view = bounds(sourceDuration);
        setView(view.start + direction * (view.end - view.start) * .8,
          view.end - view.start, sourceDuration, true);
      });
    }
  });
  window.addEventListener("transcribe:timeline", (event) => {
    sourceDuration = Math.max(1, event.detail.duration);
    if (viewport && viewport.end > sourceDuration) {
      setView(viewport.start, viewport.end - viewport.start);
      return;
    }
    updateControls();
    updateCursors();
  });
  window.addEventListener("transcribe:playback-time", (event) => {
    cursor = Number(event.detail);
    if (viewport && document.getElementById("follow-playhead").checked &&
        (cursor < viewport.start || cursor > viewport.end)) {
      setView(cursor, viewport.end - viewport.start);
    }
    updateCursors();
  });
  window.addEventListener("transcribe:playback-stop", () => { cursor = null; updateCursors(); });
  window.addEventListener("transcribe:source-changed", () => {
    cursor = null;
    setView(0, null, 1);
  });
  window.addEventListener("resize", updateCursors);
})();
