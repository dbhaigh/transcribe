# Engraved notation in the WebView2 editor

## Existing path and constraints

The current WebView2 staff is drawn in [`ui/editor.js`](../ui/editor.js) on a
fixed-height canvas. It maps recording seconds linearly across one treble staff,
derives a rough natural-note position from MIDI pitch, and draws a sharp sign
for chromatic pitches. It does not consume the score's written spelling,
rhythmic values, measures, meter, key, voices, ties, rests, or tuplets. The
tablature canvas is a separate timeline view.

The C++ [`ScoreDocument`](../include/transcribe/score.hpp) is the accepted
musical model; [`ScoreEditor`](../src/score.cpp) applies revision-checked
transactions. The existing MusicXML exporter
([`src/musicxml.cpp`](../src/musicxml.cpp)) already serializes score positions,
written pitches and spelling, time/key signatures, voices, chord groups,
rests, ties, dots, supported tuplets, dynamics/articulations when present, and
a guitar-tab part. That is the appropriate renderer input; rendering the raw
audio-note list would lose score interpretation.

The WebView2 bridge uses a local virtual host (`appassets.transcribe`) and
revisioned score messages. This permits local JavaScript/WASM assets without a
remote service. Editing remains in the C++ score transaction path; an SVG is a
view, never the score data store.

## Candidate comparison (reviewed 2026-10-20)

| Candidate | License and distribution | Offline / Windows | Engraving and editing fit |
|---|---|---|---|
| [Verovio](https://github.com/rism-digital/verovio) 6.3.0 (`verovio` npm package) | LGPL-3.0-or-later; the project publishes `COPYING` and `COPYING.LESSER`. The 6.3.0 npm metadata reports 27,014,756 unpacked bytes (8,404,846-byte package archive in this review). | Its ESM WebAssembly toolkit can be copied beside the UI and loaded from the existing local WebView2 virtual host. No network access or native DLL is needed at runtime. Redistributable builds must carry the LGPL notices/text and comply with the applicable LGPL terms. | Native C++20 engraving engine, MusicXML input and SVG output. It is a renderer, not a score editor. MusicXML IDs can be used as a selection bridge, subject to verifying the actual SVG mapping for the pinned build. Uses SMuFL notation fonts; the selected npm WASM distribution packages its rendering data with the module rather than requiring a system music-font install. |
| [OpenSheetMusicDisplay (OSMD)](https://github.com/opensheetmusicdisplay/opensheetmusicdisplay) 2.2.1 | BSD-3-Clause; npm metadata reports 2,311,326 unpacked bytes. Its README explicitly lists MusicXML rendering and guitar tablature, while also warning that not all MusicXML tags/attributes are fully supported. | Browser JavaScript; can be hosted locally and bundled without a native Windows dependency. | Uses VexFlow for browser rendering and exposes an accessible/modifiable graphical model, but still requires a separate mapping to Transcriber's canonical note IDs and transactional editor. Its smaller package is attractive, but Verovio was selected for this task's engraving-first requirement and native C++/WASM toolkit. |

**Selection:** Verovio is the optional WebView2 engraving backend. Its
MusicXML-to-SVG toolkit and local WASM build fit the existing bridge and avoid
replacing the C++ engine or native GUI. The LGPL dependency is isolated to the
optional WebView2 UI; the default native GUI and CLI do not link or load it.
OSMD remains a reasonable alternative if future compatibility tests show a
MusicXML feature that Verovio cannot render acceptably.

Sources: [Verovio README and license](https://github.com/rism-digital/verovio/blob/develop/README.md),
[Verovio JavaScript package README](https://github.com/rism-digital/verovio/blob/develop/emscripten/npm/README.md),
[Verovio LGPL texts](https://github.com/rism-digital/verovio/tree/develop),
[Verovio npm 6.3.0 metadata](https://registry.npmjs.org/verovio/6.3.0),
[OSMD README](https://github.com/opensheetmusicdisplay/opensheetmusicdisplay/blob/develop/README.md),
[OSMD license](https://github.com/opensheetmusicdisplay/opensheetmusicdisplay/blob/develop/LICENSE),
and [OSMD npm 2.2.1 metadata](https://registry.npmjs.org/opensheetmusicdisplay/2.2.1).

## Implemented integration and synchronization

Verovio runs only in the optional WebView2 front end. It receives MusicXML
generated from the same accepted `ScoreDocument` revision used by MusicXML
export. The score exporter can return that exact MusicXML string while writing
the requested file. Canonical note IDs are attached to notation and tablature
MusicXML elements; bar-crossing continuation fragments map back to the same
score note. Selecting an engraved note selects the canonical note in the
existing editor; edits, undo/redo, validation, and conflict handling remain
`ScoreTransaction` operations. Accepted edits regenerate the engraving from
the accepted revision, not from browser-generated SVG.

The ESM/WebAssembly toolkit runs in a module worker. Provider/build
configuration is explicit: `TRANSCRIBE_ENABLE_VEROVIO` is off by default and
requires `TRANSCRIBE_ENABLE_WEBVIEW2` on Windows plus a local `verovio@6.3.0`
package directory. There is no configure-time download and no network request
at runtime. The UI retains its editable canvas as fallback and displays it
alongside the last accepted engraving while edits are drafts. A failed or
unsupported score load reports the failure and leaves the canvas usable.
Playback draws a transient cursor over the engraved note active at the
current recording-time position; it does not modify SVG source or score data.

SVG remains vector at any display density. The renderer provides zoom controls
and a scrollable, vertically paginated viewport. SVG clicks and canvas
selection synchronize with the editor by stable note ID. Unsaved browser edits
remain a draft; they do not replace the accepted engraving or score revision.
Pagination and system breaks are renderer output, not new score events.

## Optional build and deployment

The default CMake configuration remains unchanged. To enable the renderer,
provide the `dist` directory from the pinned `verovio@6.3.0` npm package:

```powershell
cmake -S . -B build-webview `
  -DTRANSCRIBE_ENABLE_WEBVIEW2=ON `
  -DTRANSCRIBE_ENABLE_VEROVIO=ON `
  -DTRANSCRIBE_VEROVIO_DIST_DIR="C:\path\to\node_modules\verovio\dist" `
  -DTRANSCRIBE_WEBVIEW2_SDK_ROOT="C:\path\to\Microsoft.Web.WebView2"
cmake --build build-webview --config Release --target transcriber-gui
```

Configuration validates the npm metadata (exact version and license) and
required ESM toolkit files; it never downloads Verovio. The built application
copies the JS/WASM module, LGPL/GPL license
texts, package provenance notice, and visual fixtures into its local UI asset
directory. The renderer identifies Verovio and its LGPL license in the UI. A
deployed WebView2 runtime is still required as for the existing application;
no Internet connection is needed for transcription or engraving after
deployment. The dependency is not used by the native Win32 GUI build.

## Scope and known limitations

Engraving quality is bounded by the MusicXML that Transcriber currently
produces. The renderer cannot infer missing score facts. The existing exporter
does not yet model every conventional engraving feature, all instrument
catalogue conventions, arbitrary MusicXML constructs, or all tuplet-bracket
and voice-layout decisions. Its supported meters and spellings also constrain
what can be displayed. Guitar tablature is available as a separate MusicXML
part; exact fingering remains a score suggestion, not a claim about the
original performance.

The first integration keeps the existing canvas as a fallback and for
uncommitted edit previews. It does not provide engraving-tool drag gestures,
beam editing, page-layout editing, or arbitrary SVG annotation. Those require
additional canonical score commands and are intentionally not implemented by
mutating rendered SVG.

`tests/fixtures/notation/` contains common and complex MusicXML fixtures with
stable IDs, meter, rests, dots, chords, multiple voices, tuplets, ties,
dynamics, articulation, and tablature. The optional build copies these to the
UI asset directory. While the app is running, open
`https://appassets.transcribe/notation-fixtures.html` to render both fixtures
for visual review. `tests/notation_fixtures_smoke.py` checks their structure
and renderer wiring; pixel-based screenshot baselines are not automated yet
because the repository does not pin a WebView2 runtime/graphics environment.
These fixtures do not imply full MusicXML conformance.

The surrounding WebView2 workstation controls, supported operations, and
current integration limits are documented in
[`workstation-ui.md`](./workstation-ui.md).
