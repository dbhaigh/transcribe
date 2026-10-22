import createVerovioModule from "./vendor/verovio/verovio-module.mjs";
import { VerovioToolkit } from "./vendor/verovio/verovio.mjs";

let toolkitPromise;

async function getToolkit() {
  if (!toolkitPromise) {
    toolkitPromise = createVerovioModule().then((module) => {
      const instance = new VerovioToolkit(module);
      return instance;
    });
  }
  return toolkitPromise;
}

self.addEventListener("message", async (event) => {
  const { requestId, musicxml, fixture, layout = "a4", inspect = false } = event.data || {};
  if (!Number.isInteger(requestId) || typeof musicxml !== "string" || !musicxml) {
    self.postMessage({
      type: "error",
      requestId,
      fixture,
      message: "Engraving request was invalid.",
    });
    return;
  }

  try {
    const toolkit = await getToolkit();
    const sizes = { a4: [2100, 2970], letter: [2159, 2794], compact: [1400, 2000] };
    if (!Object.hasOwn(sizes, layout)) throw new Error("Unsupported engraving page layout.");
    const [pageWidth, pageHeight] = sizes[layout];
    toolkit.setOptions({
      pageWidth, pageHeight, scale: 38, breaks: "auto",
      adjustPageHeight: false, svgViewBox: true,
    });
    if (!toolkit.loadData(musicxml)) {
      throw new Error(toolkit.getLog() || "Verovio could not load the score MusicXML.");
    }
    const pageCount = toolkit.getPageCount();
    if (!Number.isInteger(pageCount) || pageCount < 1) {
      throw new Error("Verovio produced no score pages.");
    }
    const pages = [];
    for (let page = 1; page <= pageCount; page += 1) {
      const svg = toolkit.renderToSVG(page, false);
      if (typeof svg !== "string" || !svg.includes("<svg")) {
        throw new Error(`Verovio failed to render page ${page}.`);
      }
      pages.push(svg);
    }
    let inspection = {};
    if (inspect) {
      const mei = toolkit.getMEI();
      toolkit.renderToMIDI();
      const midiValues = {};
      for (const match of mei.matchAll(/<note\b[^>]*\bxml:id="([^"]+)"/g)) {
        midiValues[match[1]] = toolkit.getMIDIValuesForElement(match[1]);
      }
      inspection = { mei, midiValues };
    }
    self.postMessage({
      type: "rendered", requestId, fixture, pages,
      diagnostics: toolkit.getLog() || "",
      ...inspection,
    });
  } catch (error) {
    self.postMessage({
      type: "error",
      requestId,
      fixture,
      message: error instanceof Error ? error.message : String(error),
    });
  }
});
