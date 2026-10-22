#!/usr/bin/env python3
"""Combine quality, performance, provenance, test, and manual release evidence."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import xml.etree.ElementTree as ElementTree

from evaluate import write_json


MANUAL_CHECKS = (
    "live_input_and_recording",
    "real_time_notation",
    "tablature_synchronization",
    "editing_and_undo_redo",
    "playback_cursor_synchronization",
    "export_round_trip",
)
METRIC_NAMES = ("frame", "onset_pitch", "note_event")


def _read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {path}")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _summary_metrics(summary: dict) -> dict:
    return {
        name: {
            key: summary.get(name, {}).get(key)
            for key in ("precision", "recall", "f1")
        }
        for name in METRIC_NAMES
    }


def _quality_systems(report: dict) -> list[tuple[dict, dict]]:
    if report.get("type") == "comparison_suite":
        return [(system["report"], system) for system in report.get("systems", [])]
    return [(report, {})]


def quality_evidence(paths: list[Path]) -> tuple[list[dict], set[str], list[str]]:
    evidence = []
    providers = set()
    limitations = []
    for path in paths:
        top = _read_json(path)
        if top.get("schema_version") != 2:
            raise ValueError(f"Unsupported quality report schema in {path}")
        for report, suite_system in _quality_systems(top):
            build = report.get("build", top.get("build", {}))
            environment = report.get("environment", top.get("environment", {}))
            model = report.get("model", {})
            provider = suite_system.get(
                "provider", report.get("configuration", {}).get("provider", "unknown"))
            if provider:
                providers.add(provider)
            summary = report.get("summary", {})
            compact_clips = [
                {
                    "id": clip["id"],
                    "metrics": _summary_metrics(clip.get("metrics", {})),
                    "onset_error_seconds": clip.get("metrics", {}).get(
                        "onset_error_seconds", {}).get("mean"),
                    "offset_error_seconds": clip.get("metrics", {}).get(
                        "offset_error_seconds", {}).get("mean"),
                    "fragmentation": clip.get("metrics", {}).get("note_fragmentation"),
                    "merging": clip.get("metrics", {}).get("note_merging"),
                }
                for clip in report.get("clips", [])
            ]
            evidence.append({
                "source_report": str(path),
                "report_type": top.get("type", "single_engine"),
                "generated_at_utc": report.get("generated_at_utc", top.get("generated_at_utc")),
                "corpus": report.get("corpus", top.get("corpus")),
                "provenance": report.get("corpus", top.get("corpus", {})).get("provenance"),
                "license": report.get("corpus", top.get("corpus", {})).get("license"),
                "dataset_split": report.get("configuration", top.get("configuration", {})).get(
                    "dataset_split"),
                "build": {
                    "compiler": build.get("compiler"),
                    "build_type": build.get("build_type"),
                    "features": build.get("features"),
                    "project_version": build.get("project_version"),
                },
                "environment": environment,
                "system": {
                    "id": suite_system.get("id", model.get("identifier")),
                    "backend": suite_system.get("backend",
                                                report.get("configuration", {}).get("backend")),
                    "provider": provider,
                    "model_sha256": suite_system.get("model_sha256", model.get("sha256")),
                },
                "configuration": report.get("configuration", top.get("configuration")),
                "summary_metrics": _summary_metrics(summary),
                "timing": {
                    "analysis_seconds": summary.get("analysis_seconds"),
                    "total_seconds": summary.get("total_seconds"),
                    "realtime_factor": summary.get("real_time_factor"),
                    "peak_resident_bytes": summary.get("peak_resident_bytes"),
                },
                "per_clip": compact_clips,
                "comparison": report.get("comparison", top.get("comparison")),
            })
            identifier = str(model.get("identifier", suite_system.get("id", ""))).lower()
            is_onnx = suite_system.get("backend",
                                       report.get("configuration", {}).get("backend")) == "onnx"
            if is_onnx or "basic-pitch" in identifier or "basic_pitch" in identifier:
                limitations.append(
                    "Potential neural-model training-data overlap with external evaluation recordings "
                    "has not been ruled out.")
    if not paths:
        limitations.append("No quality-evaluation reports were supplied.")
    if not any(item.get("corpus", {}).get("name") in ("GuitarSet", "guitarset")
               for item in evidence):
        limitations.append(
            "No licensed real-recording evaluation was supplied; synthetic results do not establish "
            "real-instrument generalization.")
    return evidence, providers, list(dict.fromkeys(limitations))


def performance_evidence(paths: list[Path]) -> tuple[list[dict], list[str]]:
    evidence = []
    limitations = []
    for path in paths:
        report = _read_json(path)
        if report.get("schema_version") != 2 or report.get("measurement_protocol_version") != 2:
            raise ValueError(f"Unsupported or incomparable performance report protocol in {path}")
        clips = []
        for row in report.get("clips", []):
            timings = row.get("timing_percentiles_seconds", {})
            model = row.get("model_interface", {})
            clips.append({
                "id": row.get("id"),
                "backend": row.get("backend"),
                "mode": row.get("mode"),
                "audio_sha256": row.get("audio_sha256"),
                "cold_inference_seconds": row.get("cold_run", {}).get("wall_seconds"),
                "warm_wall_seconds": timings,
                "process_wall_seconds": row.get("process_wall_seconds"),
                "startup_seconds": row.get("startup_seconds"),
                "model_load_seconds": model.get("load_seconds"),
                "real_time_factor": row.get("realtime_factor"),
                "throughput_audio_seconds_per_second": row.get("audio_seconds_per_second"),
                "peak_resident_bytes": max(
                    (sample.get("peak_resident_bytes", 0)
                     for sample in [row.get("cold_run", {}), *row.get("runs", [])]),
                    default=None,
                ),
                "quality_metrics": row.get("metrics"),
                "comparison": row.get("comparison"),
            })
        evidence.append({
            "source_report": str(path),
            "generated_at_utc": report.get("generated_at_utc"),
            "measurement_protocol_version": report.get("measurement_protocol_version"),
            "build": report.get("build"),
            "environment": report.get("environment"),
            "comparison_context": report.get("comparison_context"),
            "configuration": report.get("configuration"),
            "protocol": report.get("protocol"),
            "clips": clips,
        })
    if not paths:
        limitations.append(
            "No controlled Release performance report was supplied; startup, cold/warm latency, "
            "throughput, real-time factor, and memory remain unqualified.")
    return evidence, limitations


def test_evidence(path: Path | None) -> dict:
    if path is None:
        return {"status": "pending", "source": None, "tests": None,
                "passed": None, "failed": None, "skipped": None}
    root = ElementTree.parse(path).getroot()
    cases = root.findall(".//testcase")
    failed_cases = [case for case in cases
                    if case.find("failure") is not None or case.find("error") is not None]
    skipped_cases = [case for case in cases if case.find("skipped") is not None]
    failed = len(failed_cases)
    skipped = len(skipped_cases)
    def describe(case, tag):
        item = case.find(tag)
        text = item.get("message") or (item.text or "").strip()
        return {
            "name": case.get("name"),
            "classname": case.get("classname"),
            "details": text.splitlines()[0] if text else None,
        }
    status = "fail" if failed else "pass"
    return {
        "status": status, "source": str(path), "tests": len(cases),
        "passed": len(cases) - failed - skipped, "failed": failed, "skipped": skipped,
        "failed_tests": [
            describe(case, "failure" if case.find("failure") is not None else "error")
            for case in failed_cases
        ],
        "skipped_tests": [describe(case, "skipped") for case in skipped_cases],
    }


def manual_evidence(path: Path | None) -> tuple[dict, list[str]]:
    if path is None:
        return {"status": "pending", "checks": {}}, [
            "Manual acceptance of live notation, tablature synchronization, editing, playback, and export is pending."
        ]
    data = _read_json(path)
    if data.get("schema_version") != 1 or not isinstance(data.get("checks"), dict):
        raise ValueError("Manual acceptance file must use schema_version 1 and a checks object")
    missing = sorted(set(MANUAL_CHECKS) - set(data["checks"]))
    extra = sorted(set(data["checks"]) - set(MANUAL_CHECKS))
    if missing or extra:
        raise ValueError(f"Manual acceptance checks mismatch; missing={missing}, extra={extra}")
    allowed = {"pass", "fail", "not_run"}
    for key, value in data["checks"].items():
        if not isinstance(value, dict) or value.get("status") not in allowed:
            raise ValueError(f"Manual check {key} must be an object with pass/fail/not_run status")
    failures = [key for key, value in data["checks"].items()
                if value["status"] == "fail"]
    pending = [key for key, value in data["checks"].items()
               if value["status"] == "not_run"]
    return {
        "status": "fail" if failures else "pending" if pending else "pass",
        "operator": data.get("operator"),
        "date": data.get("date"),
        "checks": data["checks"],
        "failures": failures,
        "pending": pending,
    }, (["Manual acceptance has failed checks: " + ", ".join(failures)] if failures else
        ["Manual acceptance is incomplete: " + ", ".join(pending)] if pending else [])


def change_summary(explicit: str | None) -> dict:
    root = Path(__file__).resolve().parents[1]
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True, check=False)
    subject = subprocess.run(
        ["git", "log", "-1", "--format=%s"], cwd=root,
        capture_output=True, text=True, check=False)
    changed = subprocess.run(
        ["git", "show", "--format=", "--name-only", "HEAD"], cwd=root,
        capture_output=True, text=True, check=False)
    status = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=all"], cwd=root,
        capture_output=True, text=True, check=False)
    worktree_paths = []
    if status.returncode == 0:
        for line in status.stdout.splitlines():
            if len(line) > 3:
                path = line[3:].strip()
                if " -> " in path:
                    path = path.split(" -> ", 1)[1]
                worktree_paths.append(path)
    dirty = bool(worktree_paths)
    subject_text = subject.stdout.strip() if subject.returncode == 0 else "Not available"
    return {
        "summary": explicit or (
            "Uncommitted working-tree changes" if dirty else subject_text),
        "commit": commit.stdout.strip() if commit.returncode == 0 else None,
        "subject": subject_text if subject.returncode == 0 else None,
        "dirty_worktree": dirty,
        "working_tree_paths": sorted(set(worktree_paths)),
        "commit_paths": sorted({line.strip() for line in changed.stdout.splitlines()
                                if line.strip()}) if changed.returncode == 0 else [],
        "git_metadata_error": next(
            (run.stderr.strip() for run in (commit, subject, changed, status) if run.returncode),
            None),
    }


def create_report(args) -> dict:
    quality, providers, quality_limits = quality_evidence(args.quality)
    performance, performance_limits = performance_evidence(args.performance)
    performance_review = []
    for report in performance:
        context = report.get("comparison_context") or {}
        for clip in report["clips"]:
            comparison = clip.get("comparison")
            if not comparison:
                continue
            if not context.get("timing_comparable"):
                performance_review.append({
                    "input": clip["id"], "backend": clip["backend"], "mode": clip["mode"],
                    "status": "not_comparable",
                    "reason": "Hardware or build configuration differs",
                })
            elif comparison.get("warm_speedup") is not None and comparison["warm_speedup"] < 1:
                performance_review.append({
                    "input": clip["id"], "backend": clip["backend"], "mode": clip["mode"],
                    "status": "observed_slowdown",
                    "warm_speedup": comparison["warm_speedup"],
                    "reason": "Matched-hardware warm timing is slower; disposition required",
                })
    if any(item["status"] == "observed_slowdown" for item in performance_review):
        performance_limits.append(
            "Matched-hardware performance slowdown(s) were observed; release status remains pending "
            "until each is reviewed and dispositioned.")
    if any(item["status"] == "not_comparable" for item in performance_review):
        performance_limits.append(
            "Some performance deltas are not comparable because hardware/build identity differs; "
            "do not use them as speedup claims.")
    gates = []
    for path in args.quality_gate:
        gate = _read_json(path)
        if gate.get("schema_version") != 1:
            raise ValueError(f"Unsupported quality gate schema in {path}")
        gates.append({
            "source_report": str(path),
            "gate": gate.get("gate"),
            "status": gate.get("status"),
            "baseline": gate.get("baseline"),
            "candidate": gate.get("candidate"),
            "thresholds": gate.get("thresholds"),
            "failures": gate.get("failures", []),
        })
    if not gates:
        quality_limits.append("No per-fixture quality regression gate was supplied.")
    tests = test_evidence(args.ctest_junit)
    manual, manual_limits = manual_evidence(args.manual_acceptance)
    limitations = list(dict.fromkeys(
        quality_limits + performance_limits + manual_limits + args.limitation))
    gate_failures = any(gate["status"] != "pass" for gate in gates)
    failed = tests["status"] == "fail" or gate_failures or manual["status"] == "fail"
    required_pending = (
        tests["status"] == "pending" or not gates or manual["status"] == "pending" or
        bool(performance_review) or
        not performance or not any(
            item.get("corpus", {}).get("name") in ("GuitarSet", "guitarset")
            for item in quality)
    )
    status = "blocked" if failed else "pending" if required_pending else "ready"
    qualified_providers = sorted(providers)
    if not any(provider in ("cuda", "directml", "rocm") for provider in qualified_providers):
        limitations.append(
            "No accelerator provider has a matching quality report; GPU provider performance is unqualified.")
    return {
        "schema_version": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "generator": {
            "file": "python/release_report.py",
            "sha256": _sha256(Path(__file__).resolve()),
        },
        "overall_status": status,
        "changes": change_summary(args.change_summary),
        "automated_tests": tests,
        "quality_regression_gates": gates,
        "quality_evidence": quality,
        "performance_evidence": performance,
        "performance_review": performance_review,
        "provider_qualification": {
            "quality_report_providers": qualified_providers,
            "gpu_inventory": [
                report.get("environment", {}).get("gpu_inventory")
                for report in quality + performance
                if report.get("environment", {}).get("gpu_inventory") is not None
            ],
            "hardware_timing_gate": "not configured; shared CI timing is descriptive only",
        },
        "manual_acceptance": manual,
        "unresolved_limitations": list(dict.fromkeys(limitations)),
        "claims_policy": (
            "Synthetic metrics are regression sentinels only. Real-recording claims require a "
            "documented licensed split, leakage analysis, and per-instrument evidence. "
            "Performance comparisons require matching hardware, build, provider, model, and protocol."),
    }


def markdown(report: dict) -> str:
    lines = [
        "# Transcriber release evidence report",
        "",
        f"**Overall status:** {report['overall_status'].upper()}",
        "",
        f"Generated: {report['generated_at_utc']}",
        "",
        "## Changes",
        "",
        report["changes"]["summary"],
        "",
        f"Commit: `{report['changes'].get('commit') or 'not available'}`",
        "",
        "## Automated tests and deterministic regression gates",
        "",
        f"- CTest: **{report['automated_tests']['status']}** "
        f"({report['automated_tests']['passed']} passed, "
        f"{report['automated_tests']['failed']} failed, "
        f"{report['automated_tests']['skipped']} skipped).",
    ]
    lines.extend([
        f"  - Failed: {item['name']}: {item['details'] or 'no failure detail'}"
        for item in report["automated_tests"].get("failed_tests", [])
    ])
    lines.extend([
        f"  - Skipped: {item['name']}: {item['details'] or 'no skip reason'}"
        for item in report["automated_tests"].get("skipped_tests", [])
    ])
    if report["quality_regression_gates"]:
        for gate in report["quality_regression_gates"]:
            lines.append(f"- {gate['gate']}: **{gate['status']}** "
                         f"(baseline corpus `{gate['baseline']['corpus_sha256']}`).")
            lines.extend([f"  - Regression: {failure}" for failure in gate["failures"]])
    else:
        lines.append("- Per-fixture quality gate: **pending**.")
    lines.extend(["", "## Quality evidence", ""])
    if not report["quality_evidence"]:
        lines.append("No quality reports supplied.")
    else:
        lines.extend([
            "| System | Corpus / split | Provider | Frame F1 | Onset+pitch F1 | Note-event F1 |",
            "|---|---|---|---:|---:|---:|",
        ])
    for item in report["quality_evidence"]:
        metrics = item["summary_metrics"]
        corpus = item["corpus"] or {}
        split = item.get("dataset_split") or "not applicable"
        def format_f1(metric):
            value = metrics[metric].get("f1")
            return "n/a" if value is None else f"{value:.4f}"

        lines.append(
            f"| {item['system']['id']} | {corpus.get('name')} v{corpus.get('version')} / "
            f"{split} | {item['system']['provider']} | {format_f1('frame')} | "
            f"{format_f1('onset_pitch')} | {format_f1('note_event')} |")
        comparison = item.get("comparison") or {}
        regressions = comparison.get("event_f1_regressed_clips", [])
        if regressions:
            lines.append(f"  - Observed note-event F1 regressions: {', '.join(regressions)}.")
    lines.extend(["", "## Performance evidence", ""])
    if not report["performance_evidence"]:
        lines.append("No performance report supplied; latency, throughput, and memory are unqualified.")
    for item in report["performance_evidence"]:
        context = item.get("comparison_context")
        comparisons = [clip.get("comparison") for clip in item["clips"]
                       if clip.get("comparison")]
        if comparisons and context and context.get("timing_comparable"):
            warm_speedups = [entry["warm_speedup"] for entry in comparisons
                             if entry.get("warm_speedup") is not None]
            speedup = statistics.median(warm_speedups) if warm_speedups else None
            if speedup is None:
                timing_text = "matched-hardware comparison has no timings"
            elif speedup >= 1:
                timing_text = f"observed matched-hardware warm speedup {speedup:.3f}x"
            else:
                timing_text = f"observed matched-hardware warm slowdown {1 / speedup:.3f}x"
        elif comparisons:
            timing_text = "timing deltas recorded but hardware/build mismatch prevents comparison"
        else:
            timing_text = "no matched performance baseline"
        lines.append(
            f"- **{item['configuration'].get('model_identifier')}** — "
            f"protocol v{item['measurement_protocol_version']}; "
            f"build {item['build'].get('build_type')}; {len(item['clips'])} measurements; "
            f"{timing_text}.")
        lines.extend([
            "",
            "Per-input results; p95/p99 with fewer than 100 warm samples are descriptive, not statistically stable.",
            "",
            "| Backend | Mode | Input | Warm n | Startup ms | Process ms | Model load ms | Cold ms | Warm p50 ms | p95 ms | p99 ms | RTF | Audio s/s | Peak MiB | Event F1 | Comparison |",
            "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
        ])
        for clip in item["clips"]:
            timings = clip.get("warm_wall_seconds") or {}
            metrics = clip.get("quality_metrics") or {}
            event = metrics.get("note_event") or {}
            comparison = clip.get("comparison")
            if not comparison:
                comparison_text = "baseline n/a"
            elif not comparison.get("timing_comparable"):
                comparison_text = "not comparable"
            elif comparison.get("warm_speedup") is None:
                comparison_text = "n/a"
            elif comparison["warm_speedup"] >= 1:
                comparison_text = f"{comparison['warm_speedup']:.3f}x faster"
            else:
                comparison_text = f"{1 / comparison['warm_speedup']:.3f}x slower"

            def seconds_ms(value):
                return "n/a" if value is None else f"{value * 1000:.2f}"

            def number(value):
                return "n/a" if value is None else f"{value:.4f}"

            def mib(value):
                return "n/a" if value is None else f"{value / 1048576:.2f}"

            memory = clip.get("peak_resident_bytes")
            lines.append(
                f"| {clip['backend']} | {clip['mode']} | {clip['id']} | "
                f"{timings.get('sample_count', 0)} | "
                f"{seconds_ms(clip.get('startup_seconds'))} | "
                f"{seconds_ms(clip.get('process_wall_seconds'))} | "
                f"{seconds_ms(clip.get('model_load_seconds'))} | "
                f"{seconds_ms(clip.get('cold_inference_seconds'))} | "
                f"{seconds_ms(timings.get('p50'))} | {seconds_ms(timings.get('p95'))} | "
                f"{seconds_ms(timings.get('p99'))} | {number(clip.get('real_time_factor'))} | "
                f"{number(clip.get('throughput_audio_seconds_per_second'))} | "
                f"{mib(memory)} | "
                f"{number(event.get('f1'))} | {comparison_text} |")
    if report["performance_review"]:
        lines.extend(["", "### Performance disposition required", ""])
        for item in report["performance_review"]:
            lines.append(
                f"- {item['input']} / {item['backend']} / {item['mode']}: "
                f"{item['status']} — {item['reason']}.")
    lines.extend(["", "## Manual acceptance", "",
                  f"Status: **{report['manual_acceptance']['status']}**"])
    for key, value in report["manual_acceptance"].get("checks", {}).items():
        lines.append(f"- {key}: **{value['status']}**")
    lines.extend(["", "## Unresolved limitations", ""])
    lines.extend([f"- {item}" for item in report["unresolved_limitations"]] or ["- None"])
    changed_paths = (report["changes"].get("working_tree_paths") or
                     report["changes"].get("commit_paths", []))
    if changed_paths:
        lines.extend(["", "## Changed paths", ""])
        lines.extend([f"- `{path}`" for path in changed_paths])
    lines.extend(["", "## Claims policy", "", report["claims_policy"], ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quality", action="append", type=Path, default=[])
    parser.add_argument("--quality-gate", action="append", type=Path, default=[])
    parser.add_argument("--performance", action="append", type=Path, default=[])
    parser.add_argument("--ctest-junit", type=Path)
    parser.add_argument("--manual-acceptance", type=Path)
    parser.add_argument("--change-summary")
    parser.add_argument("--limitation", action="append", default=[])
    parser.add_argument("--output-json", required=True, type=Path)
    parser.add_argument("--output-markdown", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = create_report(args)
        write_json(args.output_json, report)
        args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
        args.output_markdown.write_text(markdown(report), encoding="utf-8")
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError,
            ElementTree.ParseError) as error:
        parser.exit(2, f"Release report could not be generated: {error}\n")
    return 1 if report["overall_status"] == "blocked" else 0


if __name__ == "__main__":
    raise SystemExit(main())
