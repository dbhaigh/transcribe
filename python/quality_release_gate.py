#!/usr/bin/env python3
"""Per-fixture synthetic regression gate against reviewed historical sentinels."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path

from evaluate import write_json


METRICS = ("frame", "onset_pitch", "note_event")
RATES = ("precision", "recall", "f1")
ERRORS = ("onset_error_seconds", "offset_error_seconds")
EXPECTED_CORPUS = "183c7f68643d6ac54a1b15a26af195e16590f115c5194f25c139366844f6fca9"


def compare(candidate: dict, baseline: dict, max_f1_drop: float,
            max_rate_drop: float, max_error_increase_seconds: float) -> dict:
    if baseline.get("schema_version") != 1:
        raise ValueError("Reviewed regression sentinel must use report schema version 1")
    if candidate.get("schema_version") != 2:
        raise ValueError("Candidate must use evaluation report schema version 2")
    for report, name in ((baseline, "baseline"), (candidate, "candidate")):
        corpus = report.get("corpus", {})
        if corpus.get("name") != "transcriber-synthetic" or corpus.get("version") != 1:
            raise ValueError(f"{name} report is not synthetic corpus v1")
        if corpus.get("sha256") != EXPECTED_CORPUS:
            raise ValueError(f"{name} corpus digest differs from the reviewed v1 corpus")
    if candidate.get("model", {}).get("identifier") != baseline.get("model", {}).get("identifier"):
        raise ValueError("Candidate and baseline model identifiers differ")
    if candidate.get("model", {}).get("sha256") != baseline.get("model", {}).get("sha256"):
        raise ValueError("Candidate and baseline model hashes differ")
    baseline_configuration = baseline.get("configuration", {})
    candidate_configuration = candidate.get("configuration", {})
    mismatches = {
        key: {"baseline": value, "candidate": candidate_configuration.get(key)}
        for key, value in baseline_configuration.items()
        if candidate_configuration.get(key) != value
    }
    expected_new_defaults = {
        "polyphonic_decoder": "legacy",
        "model_objective": None,
        "neural_decoder": "legacy",
    }
    for key, expected in expected_new_defaults.items():
        if key in candidate_configuration and candidate_configuration[key] != expected:
            mismatches[key] = {"baseline_default": expected,
                               "candidate": candidate_configuration[key]}
    if mismatches:
        raise ValueError(f"Candidate evaluation configuration differs: {mismatches}")
    if candidate.get("tolerances") != baseline.get("tolerances"):
        raise ValueError("Candidate scoring tolerances differ from the reviewed baseline")

    baseline_clips = {clip["id"]: clip for clip in baseline.get("clips", [])}
    candidate_clips = {clip["id"]: clip for clip in candidate.get("clips", [])}
    if set(baseline_clips) != set(candidate_clips):
        raise ValueError("Candidate fixture IDs differ from the reviewed baseline")

    fixtures = []
    failures = []
    for clip_id in sorted(baseline_clips):
        old = baseline_clips[clip_id]
        new = candidate_clips[clip_id]
        if old.get("audio_sha256") != new.get("audio_sha256"):
            raise ValueError(f"Fixture audio changed for {clip_id}")
        metric_results = {}
        for metric in METRICS:
            old_metric = old["metrics"][metric]
            new_metric = new["metrics"][metric]
            metric_results[metric] = {}
            for rate in RATES:
                before = old_metric[rate]
                after = new_metric[rate]
                if before is None or after is None:
                    if before != after:
                        failures.append(f"{clip_id}: {metric}.{rate} availability changed")
                    metric_results[metric][rate] = {
                        "baseline": before, "candidate": after, "delta": None,
                        "status": "unavailable" if before is None and after is None else "fail",
                    }
                    continue
                delta = after - before
                limit = max_f1_drop if rate == "f1" else max_rate_drop
                status = "pass" if delta >= -limit else "fail"
                metric_results[metric][rate] = {
                    "baseline": before, "candidate": after, "delta": delta,
                    "maximum_drop": limit, "status": status,
                }
                if status == "fail":
                    failures.append(
                        f"{clip_id}: {metric}.{rate} dropped {abs(delta):.6f}, "
                        f"limit {limit:.6f}")
        error_results = {}
        for error_name in ERRORS:
            before = old["metrics"][error_name]["mean"]
            after = new["metrics"][error_name]["mean"]
            if before is None or after is None:
                status = "unavailable" if before is None and after is None else "fail"
                error_results[error_name] = {
                    "baseline": before, "candidate": after, "increase": None,
                    "status": status,
                }
                if status == "fail":
                    failures.append(f"{clip_id}: {error_name} availability changed")
                continue
            increase = after - before
            status = "pass" if increase <= max_error_increase_seconds else "fail"
            error_results[error_name] = {
                "baseline": before, "candidate": after, "increase": increase,
                "maximum_increase_seconds": max_error_increase_seconds,
                "status": status,
            }
            if status == "fail":
                failures.append(
                    f"{clip_id}: {error_name} increased {increase:.6f}s, "
                    f"limit {max_error_increase_seconds:.6f}s")
        fixtures.append(dict(id=clip_id, metrics=metric_results, boundary_errors=error_results))

    return {
        "schema_version": 1,
        "gate": "synthetic-v1-per-fixture",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "baseline": {
            "report_schema_version": baseline["schema_version"],
            "configuration": baseline_configuration,
            "model": baseline.get("model"),
            "corpus_sha256": EXPECTED_CORPUS,
            "source_revision": baseline.get("evaluator", {}).get("git_revision"),
        },
        "candidate": {
            "report_schema_version": candidate["schema_version"],
            "configuration": candidate_configuration,
            "model": candidate.get("model"),
            "corpus_sha256": EXPECTED_CORPUS,
            "source_revision": candidate.get("evaluator", {}).get("git_revision"),
        },
        "thresholds": {
            "maximum_f1_drop_per_fixture": max_f1_drop,
            "maximum_precision_recall_drop_per_fixture": max_rate_drop,
            "maximum_mean_onset_or_offset_error_increase_seconds": max_error_increase_seconds,
        },
        "scope": "Synthetic regression sentinel only; not evidence of real-recording generalization.",
        "fixtures": fixtures,
        "status": "fail" if failures else "pass",
        "failures": failures,
    }


def markdown(report: dict) -> str:
    def milliseconds(value):
        return "n/a" if value is None else f"{value * 1000:+.2f}"

    lines = [
        "# Synthetic release regression gate",
        "",
        f"**Status:** {report['status'].upper()}",
        "",
        report["scope"],
        "",
        f"Baseline corpus SHA-256: `{report['baseline']['corpus_sha256']}`",
        "",
        "| Fixture | Frame F1 delta | Onset+pitch F1 delta | Note-event F1 delta | Onset error increase (ms) | Offset error increase (ms) |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for fixture in report["fixtures"]:
        def value(item):
            return "n/a" if item["delta"] is None else f"{item['delta']:+.4f}"

        frame = fixture["metrics"]["frame"]["f1"]
        onset = fixture["metrics"]["onset_pitch"]["f1"]
        event = fixture["metrics"]["note_event"]["f1"]
        onset_error = fixture["boundary_errors"]["onset_error_seconds"]["increase"]
        offset_error = fixture["boundary_errors"]["offset_error_seconds"]["increase"]
        lines.append(
            f"| {fixture['id']} | {value(frame)} | {value(onset)} | {value(event)} | "
            f"{milliseconds(onset_error)} | {milliseconds(offset_error)} |")
    lines.extend(["", "## Failures", ""])
    lines.extend([f"- {failure}" for failure in report["failures"]] or ["- None"])
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--output-json", required=True, type=Path)
    parser.add_argument("--output-markdown", required=True, type=Path)
    parser.add_argument("--max-f1-drop", type=float, default=.01)
    parser.add_argument("--max-rate-drop", type=float, default=.01)
    parser.add_argument("--max-error-increase-ms", type=float, default=10)
    args = parser.parse_args()
    if args.max_f1_drop < 0 or args.max_rate_drop < 0 or args.max_error_increase_ms < 0:
        parser.error("Regression thresholds must be nonnegative")
    try:
        report = compare(
            json.loads(args.candidate.read_text(encoding="utf-8")),
            json.loads(args.baseline.read_text(encoding="utf-8")),
            args.max_f1_drop, args.max_rate_drop,
            args.max_error_increase_ms / 1000,
        )
        write_json(args.output_json, report)
        args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
        args.output_markdown.write_text(markdown(report), encoding="utf-8")
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        parser.exit(2, f"Release quality gate could not compare reports: {error}\n")
    if report["status"] != "pass":
        print("\n".join(report["failures"]))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
