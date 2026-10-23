Project: C++20 audio-to-score tool. Live path is: ASIO callback -> SPSC input queue -> ingest worker -> analysis worker (IncrementalYin or OverlappingWindow) -> bounded event queue -> poll_events() -> TranscriptionDocument -> WebView2 UI (ui/ folder).

Hard rules:
1. The ASIO audio callback may only copy samples, read timestamps and use lock-free atomics. No allocation, mutex, logging, inference, file I/O, or UI/notification calls inside it.
2. Default build stays dependency-light (std only). ASIO, ONNX, FFmpeg, WebView2 and Verovio stay behind their existing CMake options.
3. WebView2 is the only GUI. No native Win32 GUI.
4. Preserve existing public overloads and default CLI/GUI outputs. New behaviour is additive.
5. Live note events keep stable IDs and revisions: NoteOn (provisional), NoteStabilized, NoteOff, NoteCorrected, NoteRetracted, NoteFinalized, AnalysisGap, Status. Never show raw_strength as a probability.
6. Report dropped/gapped/overloaded states visibly; never hide loss behind successful-looking output.
7. Manual edits are protected from live updates and from refinement unless explicitly approved.
8. Every change needs focused Catch2 tests (use the existing [asio]/[live] tags) and UI asset/bridge wiring checks. Do not claim hardware validation you have not done.
9. Follow docs/live.md and docs/workstation-ui.md; update them when behaviour changes.