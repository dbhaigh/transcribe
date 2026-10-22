# Batch processing and integrations

Transcriber can process a directory or an explicit, versioned CSV manifest:

```powershell
transcriber --batch .\recordings --output-dir .\scores --concurrency 2
transcriber --batch-manifest .\jobs.csv --output-dir .\scores `
  --batch-results .\scores\results.json --progress-jsonl .\scores\progress.jsonl `
  --concurrency 2 --memory-limit-mb 4096 --resume
```

Directory mode remains top-level only. It writes MusicXML and ASCII tablature by
default, and accepts WAV in dependency-light builds. Its audio-file filter
includes MP3, FLAC, OGG, M4A and AAC when the optional FFmpeg integration is
enabled. Directory mode rejects case-insensitive duplicate filename stems,
since those would overwrite the same outputs. Use a manifest to assign explicit
output names or to process separate recordings with duplicate stems.

## Manifest version 1

The manifest is UTF-8 CSV, including standard double-quote escaping. Its first
record is exactly `transcriber-batch-manifest,1`; the next record is the header.
`input` is required. Example:

```csv
transcriber-batch-manifest,1
input,musicxml,tab,events-json,project,instrument,tempo,meter,quality
"takes/guitar, verse.wav",scores/verse.musicxml,scores/verse.tab,scores/verse.events.json,scores/verse.transcriber,guitar,96,4/4,high
../bass/take.wav,,,,,bass,112,4/4,fast
```

Input, output and model paths that are not absolute are resolved relative to the
manifest's directory. The supported per-recording option columns are:

`instrument`, `model`, `backend`, `provider`, `device-id`, `tempo`,
`key-fifths`, `mode`, `meter`, `quality`, `polyphonic`,
`minimum-confidence`, `tuning`, `capo`, `string-tuning`, `fret-count`,
`written-transposition`, `quantization`, `quantization-strength`,
`minimum-rhythm-evidence`, and `tab-alternatives`.

The supported output columns are `musicxml`, `tab`, `midi`, `json`,
`events-json`, `score-project`, and `project`. MusicXML and tablature are
always enabled. A nonempty output cell selects that row's path; otherwise an
enabled output is named `<ordinal>_<input-stem><suffix>` under the output
directory (for example, `000002_take.events.json`). Ordinals are stable in
manifest order and avoid collisions between repeated stems. Optional MIDI,
JSON, note-event JSON, score-project JSON, and native project exports can be
enabled globally with the corresponding CLI option or individually by filling
the relevant manifest cell. A global path option enables that output type for
each row; per-row output paths still control where each file is written.

Manifest headers, row widths, option values and output collisions are checked
before processing. Outputs may not overwrite an input, model, config, manifest,
result, journal, progress file, or another output. The manifest is limited to
8 MiB and 100,000 recording rows.

## Result, progress and event formats

`--batch-results <path>` writes a JSON object with
`format: "transcriber-batch-results"` and `schema_version: 1`. It includes
summary counts and the requested concurrency and memory budget, plus one entry
per manifest/directory item. Each file entry has a one-based `index`, absolute
`input`, `status`, `error`, estimated memory bytes, note count when analysis
completed, and the list of expected output paths. Status is `success`, `failed`,
`skipped`, or `cancelled`.

`--progress-jsonl <path>` appends one JSON object per line. Each event has
`schema_version: 1`, `type`, one-based `index` (zero for batch-level events),
`completed`, `total`, `input`, `status`, and `error`. Typical types are
`batch_started`, `file_started`, `file_succeeded`, `file_failed`,
`file_skipped`, `file_cancelled`, `batch_finished`, and `batch_cancelled`. A new invocation
appends to an existing progress file; orchestration should record its starting
byte offset or use a new path for each run.

`--events-json <path>` writes the versioned
`transcriber-note-events` schema v1. The document declares seconds, score
quarters and zero-based source sample frames; records source and instrument
metadata; and gives every note sounding/written MIDI pitch, measured onset and
offset, score timing, voice, frame positions, confidence semantics and event
provenance. A heuristic score is not described as a calibrated probability.
The separate `--project <path>` writes the native project schema v2 and
references the original audio; audio is not embedded.

Legacy JSON output and `--score-project` remain available with their existing
contracts.

## Concurrency, memory and resumption

`--concurrency` accepts 1 through 32 and defaults to 1. Each active file gets an
independent engine/model session. Sessions are not shared across workers, so
parallel neural jobs can increase model memory use and initialization work.
Choose concurrency with the model/provider's resource requirements in mind.

`--memory-limit-mb` accepts 128 through 1,048,576 and is a scheduler admission
budget based on conservative input/model estimates. It controls which estimated
jobs may run simultaneously; it is **not** an operating-system memory limit and
cannot guarantee peak process memory. Estimates are particularly approximate
for compressed input, models and provider-side allocations. A single job whose
estimate exceeds the budget is reported as failed without starting.

`--resume` uses an append-only journal beside the result file
(`<batch-results>.journal`, or `batch-results.json.journal` by default). It
skips a job only when the latest journal status for its input/settings/output
signature is success and all expected outputs are still regular files. Failed
and cancelled work is retried. A changed input, option, or output path forms a
different signature. The journal is local operational state and should be kept
with the corresponding result file. Its input identity uses the absolute path,
file size and modification timestamp (not a content hash); do not preserve
timestamps when replacing audio if you need a guaranteed retry.

SIGINT stops new scheduling and requests cooperative cancellation of active
analysis. Decoding or inference sections without cancellation checkpoints can
finish before observing the request. Individual exports are written atomically,
but a job's group of exports is not a transaction: on failure, some output
files may already exist. The result and journal report the job outcome; resume
will rerun failed jobs rather than treating partial output as success. Model
sessions are currently per-file and are shut down when each job completes.

## Exit codes and failure semantics

* `0`: every item succeeded or was skipped after a verified successful resume.
* `1`: at least one item failed, or the batch configuration/result/progress
  operation itself failed.
* `130`: SIGINT/cancellation was observed. Completed per-file results are
  retained; work not started is marked cancelled.

A nonzero result is not a claim that no exports were produced. Inspect the
structured result file for per-item status and expected paths before consuming
outputs.

## External orchestration example

```python
import json
import subprocess

completed = subprocess.run(
    [
        r".\build\Release\transcriber.exe",
        "--batch-manifest", r".\jobs.csv",
        "--batch-results", r".\scores\results.json",
        "--progress-jsonl", r".\scores\progress.jsonl",
        "--concurrency", "2",
        "--memory-limit-mb", "4096",
        "--resume",
    ],
    check=False,
)
with open(r".\scores\results.json", encoding="utf-8") as stream:
    report = json.load(stream)
for item in report["files"]:
    print(item["status"], item["input"], item["error"])
if completed.returncode not in (0, 1, 130):
    raise RuntimeError(f"Unexpected Transcriber exit code {completed.returncode}")
```

The CLI is a local process interface; this feature does not introduce a network
service or shared model-session protocol.
