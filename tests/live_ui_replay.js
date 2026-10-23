window.runTranscriberCallbackReplayTests = async (studio, replay) => {
  const require = (condition, message) => {
    if (!condition) throw new Error(message);
  };
  const host = message => studio.__host({ data: message });
  const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
  const delivered = [];
  const overloadStates = [];
  studio.addEventListener("transcribe:live-events", event => delivered.push(...event.detail));

  host({ type: "ready", onnx: false, asio: true, verovio: false });
  host({ type: "live-status", version: 1, job: 1, state: "capturing",
    status: "Recording; live analysis active" });

  require(Array.isArray(replay.batches) && replay.batches.length > 0,
    "Callback replay did not produce bridge batches");
  let observedOverload = false;
  let recovered = false;
  for (const batch of replay.batches) {
    host(batch);
    await delay(20);
    const state = studio.document.getElementById("health-overload").dataset.state;
    overloadStates.push(state);
    if (state === "active") observedOverload = true;
    if (observedOverload && state === "none") recovered = true;
  }
  await delay(40);

  const checks = [];
  if (replay.overload_probe) {
    require(observedOverload, "OVERLOAD was never displayed during the delayed-analysis probe");
    require(recovered, "OVERLOAD did not clear after queues and analysis lag recovered");
    checks.push(`OVERLOAD appeared and recovered across ${overloadStates.length} bridge ticks`);
  } else {
    const byNote = new Map();
    for (const event of delivered) {
      if (!event.id || !Number.isInteger(Number(event.midi))) continue;
      if (!byNote.has(event.id)) byNote.set(event.id, { midi: Number(event.midi), types: new Set() });
      byNote.get(event.id).types.add(event.type);
    }
    for (const midi of replay.expected_pitches) {
      const match = [...byNote.values()].find(note => note.midi === midi &&
        ["provisional", "stabilized", "finalized"].every(type => note.types.has(type)));
      require(match, `MIDI ${midi} did not reach provisional, stabilized and finalized UI events`);
    }
    const status = studio.document.getElementById("health-overload");
    require(studio.document.getElementById("recording-timer").textContent !== "00:00.0",
      "The replay did not advance the sample-time recording timer");
    require(studio.document.getElementById("piano-roll").dataset.finalizedNotes,
      "The live canvas did not render finalized replay notes");
    require(status && studio.document.getElementById("status-text").textContent === "Capture finished",
      "The UI did not receive the final capture status");
    checks.push(`real callback transport -> bridge -> UI lifecycle for MIDI ${replay.expected_pitches.join(", ")}`);
  }
  require(studio.__errors.length === 0, `Browser errors: ${studio.__errors.join("; ")}`);
  return checks;
};
