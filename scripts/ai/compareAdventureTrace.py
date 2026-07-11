#!/usr/bin/env python3
"""Compare two ScriptedAdventureAI trace sets."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from summarizeAdventureTrace import iter_trace_files, summarize  # noqa: E402


def nested_int(summary: dict[str, Any], section: str, key: str) -> int:
    value = summary.get(section, {}).get(key, 0)
    return int(value) if isinstance(value, int) else 0


def deep_int(summary: dict[str, Any], *keys: str) -> int:
    value: Any = summary
    for key in keys:
        if not isinstance(value, dict):
            return 0
        value = value.get(key, 0)
    return int(value) if isinstance(value, int) else 0


def print_delta(label: str, baseline: int, candidate: int, lower_is_better: bool = False) -> None:
    delta = candidate - baseline
    marker = ""
    if delta:
        improved = delta < 0 if lower_is_better else delta > 0
        marker = " improved" if improved else " regressed"
    print(f"{label}: baseline={baseline} candidate={candidate} delta={delta}{marker}")


def compare(baseline_paths: list[str], candidate_paths: list[str]) -> dict[str, Any]:
    baseline = summarize(iter_trace_files(baseline_paths))
    candidate = summarize(iter_trace_files(candidate_paths))
    return {
        "baseline": baseline,
        "candidate": candidate,
        "delta": {
            "parsed": candidate["parsed"] - baseline["parsed"],
            "quality_score": deep_int(candidate, "quality", "score") - deep_int(baseline, "quality", "score"),
            "map_progress_score": deep_int(candidate, "mapProgress", "score")
            - deep_int(baseline, "mapProgress", "score"),
            "mistakes": deep_int(candidate, "mistakes", "total") - deep_int(baseline, "mistakes", "total"),
            "important_mistakes": deep_int(candidate, "mistakes", "important") - deep_int(baseline, "mistakes", "important"),
            "fallback_outputs": nested_int(candidate, "output_statuses", "fallback")
            - nested_int(baseline, "output_statuses", "fallback"),
            "failed_actions": nested_int(candidate, "progress", "failed")
            - nested_int(baseline, "progress", "failed"),
            "unsafe_candidates": nested_int(candidate, "analysis", "unsafe_candidates")
            - nested_int(baseline, "analysis", "unsafe_candidates"),
            "hero_threat_alerts": nested_int(candidate, "analysis", "hero_threat_alerts")
            - nested_int(baseline, "analysis", "hero_threat_alerts"),
            "defense_alerts": nested_int(candidate, "analysis", "defense_alerts")
            - nested_int(baseline, "analysis", "defense_alerts"),
            "executed_actions": nested_int(candidate, "progress", "executed")
            - nested_int(baseline, "progress", "executed"),
        },
    }


def print_text(result: dict[str, Any]) -> None:
    baseline = result["baseline"]
    candidate = result["candidate"]
    print_delta("parsed files", baseline["parsed"], candidate["parsed"])
    print_delta("quality score", deep_int(baseline, "quality", "score"), deep_int(candidate, "quality", "score"))
    print_delta("map progress score", deep_int(baseline, "mapProgress", "score"), deep_int(candidate, "mapProgress", "score"))
    print_delta("mistakes", deep_int(baseline, "mistakes", "total"), deep_int(candidate, "mistakes", "total"), lower_is_better=True)
    print_delta(
        "important mistakes",
        deep_int(baseline, "mistakes", "important"),
        deep_int(candidate, "mistakes", "important"),
        lower_is_better=True,
    )
    print_delta(
        "fallback outputs",
        nested_int(baseline, "output_statuses", "fallback"),
        nested_int(candidate, "output_statuses", "fallback"),
        lower_is_better=True,
    )
    print_delta(
        "failed actions",
        nested_int(baseline, "progress", "failed"),
        nested_int(candidate, "progress", "failed"),
        lower_is_better=True,
    )
    print_delta(
        "unsafe candidates",
        nested_int(baseline, "analysis", "unsafe_candidates"),
        nested_int(candidate, "analysis", "unsafe_candidates"),
        lower_is_better=True,
    )
    print_delta(
        "hero threat alerts",
        nested_int(baseline, "analysis", "hero_threat_alerts"),
        nested_int(candidate, "analysis", "hero_threat_alerts"),
        lower_is_better=True,
    )
    print_delta(
        "defense alerts",
        nested_int(baseline, "analysis", "defense_alerts"),
        nested_int(candidate, "analysis", "defense_alerts"),
        lower_is_better=True,
    )
    print_delta(
        "executed actions",
        nested_int(baseline, "progress", "executed"),
        nested_int(candidate, "progress", "executed"),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two ScriptedAdventureAI trace sets.")
    parser.add_argument("baseline", nargs="+", help="Baseline trace files or directories.")
    parser.add_argument("--candidate", nargs="+", required=True, help="Candidate trace files or directories.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON comparison.")
    args = parser.parse_args()

    result = compare(args.baseline, args.candidate)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_text(result)
    return 1 if result["baseline"]["parse_errors"] or result["candidate"]["parse_errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
