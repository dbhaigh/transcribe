import createVerovioModule from "./vendor/verovio/verovio-module.mjs";
import { VerovioToolkit } from "./vendor/verovio/verovio.mjs";

let toolkitPromise;

async function getToolkit() {
  if (!toolkitPromise) {
    toolkitPromise = createVerovioModule().then((module) => {
      const instance = new VerovioToolkit(module);
      instance.setOptions({
        pageWidth: 1400,
        scale: 38,
        breaks: "auto",
        adjustPageHeight: true,
        svgViewBox: true,
      });
      return instance;
    });
  }
  return toolkitPromise;
}

self.addEventListener("message", async (event) => {
  const { requestId, musicxml, fixture } = event.data || {};
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
    self.postMessage({ type: "rendered", requestId, fixture, pages });
  } catch (error) {
    self.postMessage({
      type: "error",
      requestId,
      fixture,
      message: error instanceof Error ? error.message : String(error),
    });
  }
});
