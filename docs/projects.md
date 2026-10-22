# Native projects and sessions

The WebView2 workstation saves `.transcriber` JSON projects. Version **2**
extends the existing project wrapper; the nested canonical score remains schema
**1** and is the single source of truth for notation, timing and tablature.
No database, archive library or neural runtime is required to open a project.

## Save, reopen and relocate

- **Save** writes the score and settings. Imported recordings remain external
  references unless the user explicitly chooses **Save with audio**.
- **Save with audio** asks for a project destination and copies the original
  recording beside it, byte for byte, under a unique name. Choose a separate
  folder, then move/share the project **and its referenced recording** together.
  This is a portable directory, not a self-contained single-file archive.
- **Score copy** writes a separate project without any recording reference.
  It does not change the open project or copy audio.
- **Summary** shows audio ownership, score revision, model selection and export
  freshness. Its **Project notes** field stores annotations and marks changes
  unsaved. Annotations are not interpreted as commands or HTML.
- Missing recordings do not prevent score editing. **Locate audio** selects a
  replacement and commits pending editor changes before updating the reference.
  It does not transcribe, replace the score or change note timestamps.
  **Save** persists that relocation in the named project.

Relinking WAV audio validates sample rate, frame count and channel count against
known project metadata. It rejects trimmed/resampled/incompatible replacements.
These checks establish timing compatibility, **not content identity**. For
non-WAV files the workflow checks only file availability: verify alignment
manually before playing or re-transcribing. Even equal-sized, equal-duration WAVs
can contain different performances. No audio content fingerprint is claimed.

### Audio storage policies

| `audio.policy` | Meaning |
|---|---|
| `score-only` | No recording path; portable score/evidence only. |
| `external-reference` | Existing recording, not owned by the project. |
| `project-copy` | Project-owned recording with a contained relative path. |

Relative references resolve against the project file's directory, not the
process working directory. Absolute external references remain supported.
Relative audio paths cannot be rooted or contain `..` components. Copied recordings
are never substituted for the original provenance: `original_reference`
retains the acquisition path when available. This path is informational and is
not used as a silent fallback.

Live recordings are saved as unique sibling float-WAV assets. Live capture also
has its existing recovery audio asset; failure to create that recovery asset is
reported and leaves a score-only recovery project. A named live-audio save
failure is an error, not a successful score-only save.

Audio and model weights are **never embedded** in project JSON. Models are not
copied or downloaded by project operations. A project with copied audio remains
editable without its original model, although re-transcription may require
locating/installing that model separately. Relative model references resolve
against the project directory; GUI model selections are saved as absolute
external references. No inference runs simply because a project is opened.
Re-audition/reaccept optional source-isolation inputs after reopening; an old
audition/acceptance flag is not portable proof that an external stem is valid.

## Version-2 schema

All fields emitted by the production serializer are required for version 2
except the three automatic-inference flags accepted as absent for historical
version-1 compatibility. Unknown fields are not preserved. Use the serializer
when changing native documents; editing JSON text by hand may invalidate its
integrity check.

| Root field | Type and contract |
|---|---|
| `format` | Literal `"transcriber-project"`. |
| `project_schema_version` | Integer `2`; independent of nested score schema. |
| `id`, `title` | Nonempty strings, at most 256 / 4,096 bytes. |
| `last_saved_path` | Last named save location; empty for unnamed sessions. Recovery hint, not the authority for resolving ordinary opens. |
| `audio` | Policy, `reference_path`, `relative_to_project`, `sample_rate`, `channels`, `frame_count`, `original_reference`. Rates in Hz; frames are original interleaved sample frames, not channel samples. |
| `analysis` | Settings and model references described below. |
| `score` | Complete canonical score schema 1; no layout coordinates. |
| `exports` | At most 32 unique format records (`musicxml`, `tablature`, `midi`), each with `format`, `path`, `score_revision`, `success`, `error`. Revision cannot be newer than the score. |
| `annotations` | At most 4,096 `{id,note_id,text}` objects. IDs unique; empty `note_id` means project-wide. Nonempty IDs must reference existing canonical notes. Each text at most 16,384 UTF-8 bytes. |
| `fingering_alternatives` | At most 16 ranked sequence snapshots, with at most 100,000 positions in total. Each has `cost`, nine `cost_components`, and `positions:[{note_id,string,fret}]`. Strings are 1-based; frets relative to capo. Stable IDs, not transient array indices. |
| `fingering_alternatives_truncated` | Boolean; reports when the search or persistence budget omitted alternatives. Shown in Summary and the restored suggestion inspector. |
| `integrity` | `{algorithm:"crc32-canonical-v2",value:"xxxxxxxx"}`. See below. |

The canonical score retains measured onset/offset seconds, source sample rate,
tempo-map/quarter mappings, editable note/rest/voice/rhythm/tie/spelling data,
meter/key hypotheses and overrides, instrument name/transposition, 1-12-string
tuning, capo/physical fret count, source events, evidence, review decisions,
manual tab corrections and revision audit records. The existing
[score implementation](../include/transcribe/score.hpp) defines these types.
MIDI, MusicXML, ASCII tablature and analysis JSON remain separate interchange
formats and are not replaced by projects.

### Analysis settings

`backend_identifier`, `model_identifier`, `model_reference`, `provider` and
`model_version` retain model selection/provenance metadata. A missing version
is explicitly `""`, not a fabricated model release or checksum. The current GUI
uses CPU inference; serializing a provider name does not make it available.
Actual event detection provenance remains in the canonical source events.

The remaining fields preserve:

- `instrument_prior`: `auto`, `none`, `guitar`, `bass`, `piano` or `voice`;
  `live_backend`: `yin`, `spectral` or `onnx`;
- `tempo_automatic`, `meter_automatic`, `key_automatic`, `polyphonic`,
  `fast_preview`, `preserve_expressive_timing`, `retain_review_evidence`,
  `bounded_inference`;
- `latency_ms` (finite, 0-10,000 in the interchange API; the current GUI live
  analyzer retains its narrower supported 20-50 ms range);
- `quantization`: `legacy`, `evidence` or `expressive`;
  `quantization_strength`, `minimum_rhythm_evidence`: finite 0-1 values;
- `fingering_alternatives`: search-output budget 0-16;
  `maximum_fret`: 0-36; `preferred_position`: 0 through that maximum.

Saved alternative costs preserve the existing component order:
fret height, preferred position, hand movement, string preference, chord stretch,
unassigned notes, repeated strings, string crossings, awkward transitions.
Their finite nonnegative sum must match `cost`. Imported positions must preserve
their referenced pitches and respect tuning, capo and fret limits. Snapshots
are suggestions, not proof of performed strings/fingers or calibrated confidence.
The accepted canonical tab assignment, including unresolved positions, is
restored independently of a fresh suggestion search.

Alternative snapshots survive ordinary saves. Pitch, duration, onset, note-set
or tuning/capo edits invalidate the old sequence context and remove its stored
alternatives rather than retaining stale rankings; reopening can generate new
suggestions. Deleting an annotated note retains its annotation as project-wide.
The GUI's free-text project note is the reserved annotation ID `project-notes`.

### Undo history

Revision audit records are persisted, **not reversible undo/redo snapshots**.
The browser/C++ undo stacks are intentionally reset on reopen. The accepted
edited score and manual assignments are retained; reopening never rolls them
back. Serializing stale asynchronous jobs, playback state, UI coordinates or
unbounded whole-score undo snapshots would be unsafe and is not supported.

## Corruption checks and validation

Version 2 requires a non-cryptographic CRC-32/IEEE integrity value. The
authoritative bytes are the production serializer's compact, ordered semantic
payload **without** the `integrity` member. CRC uses initial `0xffffffff`,
reflected polynomial `0xedb88320`, final XOR `0xffffffff`, and eight lowercase
hexadecimal digits. The loader parses/validates known fields, reserializes that
payload and compares the checksum before returning the document.
Whitespace/member-order differences are therefore harmless; changed notes,
paths, annotations or settings are detected. This is an application-specific
canonicalization, not RFC 8785. Use
`serialize_project_document` when producing interoperable native files.

CRC detects accidental corruption; it is **not authentication**, an audio
fingerprint, or protection against intentional changes with a recomputed CRC.
Historical versions had no checksum, so migration cannot establish their prior
integrity. Unknown fields are ignored and excluded from the checksum.

The shared JSON reader rejects duplicate keys, trailing garbage, invalid
numbers, nesting beyond 64, and oversized containers. Files must be nonempty
and at most **64 MiB**; the same limit is enforced on saves. Score validation
also applies. Paths are at most 32,768 bytes and reject controls/NUL, URL/data
payloads and Windows device namespaces. Audio is limited to 768 kHz and 32
channels. Copy operations accept nonempty regular recordings up to **32 GiB**
and use filesystem copying, not a recording-sized application buffer.
Opening a project is not a filesystem sandbox: external references and ordinary
filesystem links retain their normal OS access semantics.

## Atomic saves, recovery and migration

Project writes use exclusive temporary siblings, flush, then atomic replacement.
Windows uses `FlushFileBuffers` / `MoveFileEx` with write-through; POSIX uses
`fsync` / `rename` (directory fsync remains best effort). Source recordings must
not be overwritten by their project output.

Copied and live assets have unique, immutable names. Audio is published/flushed
**before** the project references it. A failed project commit removes its newly
completed asset and leaves the previous named project and previous audio intact.
Interrupted writes can leave unreferenced assets or temporary files, but cannot
make an older project reference newly overwritten audio. Old sidecars are not
garbage-collected automatically: retain them for older/recovery revisions or
remove only files known not to be referenced. Copy checks source size/mtime
before and after; do not modify a recording concurrently with copying.
Audio copying runs on the worker, is not cancelable mid-copy, and can delay
shutdown on slow storage. There is no cross-file atomic filesystem transaction
or guarantee against hardware/filesystem failure after a reported flush.

Autosave keeps one debounced pending snapshot in local application data, never
replaces a named project and never clears its unsaved marker. Recovery snapshots
rebase audio references without pretending their external assets are owned by
the recovery directory. Startup offers recovery/discard. A corrupt named file
does not silently fall back or erase the open score: open a recovery copy or
restore a known-good copy explicitly.

Migration paths are explicit:

1. Standalone legacy/score JSON is wrapped as a project, retaining the score.
2. Project version 0 migrates its score and optional audio/basic identifiers.
3. Project version 1 preserves score/audio/exports and supplies defaults for
   rhythm settings, annotation/alternative arrays and missing model metadata;
   the former audio path becomes informational original provenance.
4. Version 2 validates its checksum; unknown future versions fail clearly.

Migration is in-memory until the next explicit save; the original is not
rewritten during open. Save under another name to retain an older-reader copy.

## Example and verification

[guitar-phrase.transcriber](../tests/fixtures/projects/guitar-phrase.transcriber)
is a small version-2, score-only synthetic E4 guitar phrase with an annotation.
Open it without any audio/model dependencies. The fixture is generated using the
same production serializer and is checked by the project test suite.

From a configured dependency-light test build:

```powershell
cmake --build build-evaluation-quality --target transcriber-tests -j 4
.\build-evaluation-quality\transcriber-tests.exe "[project],[score]" --reporter compact
python tests\ui_workflow_browser.py
python tests\ui_smoke.py ui
python tests\workstation_host.py --webview .\build-usability-webview-basic\transcriber-gui.exe
```

The focused tests cover nonempty edited-score round trips, exact canonical score
serialization, tuning/capo/manual tab, annotations, alternative costs, model
metadata, v0/v1/standalone migration, checksums/truncation, validation limits,
Unicode/spaced paths, copied-directory relocation, missing/mismatched WAVs,
failed commits, immutable live assets and the example fixture. The actual
Windows-host journey additionally exercises native save/reopen, copying,
missing-audio relink without losing edited notes, and corruption errors.

### Verification results

Verified on Windows with the existing LLVM-MinGW builds:

| Check | Result |
|---|---|
| Release `[project],[score]` | 318 assertions, 34 cases passed |
| ASan/UBSan `[project]` | 125 assertions, 15 cases passed (`detect_leaks=0`, Windows runtime limitation) |
| Selected CTests, including browser workflow and asset smoke | 15/15 passed |
| Strict model-free WebView2 build and actual native-host journey | Passed |
| Strict ONNX + ASIO + Verovio build and actual native-host journey | Passed |

The host journeys use real WebView2/C++ save/open/copy/relink/export operations,
not a static UI mock. Linux runtime behavior, physical power-loss fault injection,
32 GiB copies, physical ASIO capture and hardware-provider inference were not
tested in this project-format pass. The browser workflow ran in Chromium; a
separate Node syntax check was skipped because Node is unavailable.
