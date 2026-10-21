# Local projects

The WebView2 workstation stores a project as a versioned
`.transcriber` JSON document. The canonical score remains the source of truth;
the project wrapper adds recording references and format metadata, analysis
configuration, export status, a stable project identity, and the last named
save location. Score history, source events, raw evidence, and manual
corrections are persisted with the score.

## Audio storage policy

Audio is never embedded in the project JSON. A project explicitly uses one of
two policies:

- **External reference** stores a path to the original recording. A relative
  reference is preferred when the audio is beneath the project folder; otherwise
  an absolute local path is retained. Moving a project may therefore require
  relinking an absolute-path recording.
- **Score only** stores no audio reference. The workstation's **Score copy**
  command creates this form without changing the open project's audio policy.

Live recordings saved into a named project are written as a sibling float-WAV
asset and referenced relatively. If creating a live recording asset fails, the
project is saved score-only and the application reports the loss of the audio
asset. Audio is local by default; the project workflow does not upload audio or
send telemetry.

## Save, autosave, and recovery

Named project saves use a temporary file in the destination directory, flush
the file contents, and replace the destination atomically. The Windows path
uses `MoveFileEx` with replace and write-through flags; POSIX builds use
`fsync` and `rename`. The file picker asks before replacing an existing named
project. A live recording audio sidecar is stored alongside the project.

Edits are debounced into a single pending autosave snapshot in the local
application data directory. Autosave does not replace the named project and
does not clear its unsaved marker. On startup, the workstation offers to reopen
the recovery snapshot or discard it. The audio reference is kept separate from
editable score data; if its source file is unavailable, the score can still be
opened and the missing-audio condition is reported.

Use **Save** to write the current project (or choose a name the first time).
Use **Score copy** to write a separate score-only project. The accepted score
revision, notation, tablature, and supported exports are generated from the
same canonical revision. Export-status records include their source revision;
later edits make older exports visibly stale in the project summary.

## Format and migration

The root object has `format: "transcriber-project"` and a
`project_schema_version`. Version 1 contains explicit audio policy and
recording metadata, analysis settings, export records, and a nested canonical
score document. Score-only legacy score documents and project schema version 0
are migrated when loaded. Unsupported future schema versions, malformed
fields, invalid paths/policies, and inconsistent score/export data are rejected
with an actionable error rather than silently defaulted.

The serializer and schema validation are implemented in
[`src/score.cpp`](../src/score.cpp); atomic file and audio-asset persistence is
implemented in [`src/project.cpp`](../src/project.cpp). Persistence,
migration, malformed-input, and interrupted-write behavior is covered by
[`tests/project_tests.cpp`](../tests/project_tests.cpp).

## Conversion limits

Project files preserve the canonical score and supported project metadata.
Export conversion limits remain format-specific: MusicXML and MIDI cannot
represent all of the application's evidence and editing history, while ASCII
tablature is intentionally a compact fingering view. Those details remain in
the project even when an export omits them. Projects reference external audio
instead of bundling it, so score-only copies are suitable for sharing score
data without copying or disclosing a recording.
