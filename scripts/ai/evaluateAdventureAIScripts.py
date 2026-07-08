#!/usr/bin/env python3
"""Run baseline-vs-candidate ScriptedAdventureAI evaluations."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from compareAdventureTrace import compare  # noqa: E402
from runAdventureAIBatch import run_one, script_override_value  # noqa: E402


def nested_int(summary: dict[str, Any], section: str, key: str) -> int:
    value = summary.get(section, {}).get(key, 0)
    return int(value) if isinstance(value, int) else 0


def script_snapshot_source(script: str, cwd: str | None) -> Path | None:
    override = script_override_value(script) or script
    if override.startswith("file:"):
        path = Path(override[5:])
        return path if path.is_file() else None

    base = Path(cwd) if cwd else Path.cwd()
    candidates = [
        base / override,
        base / "scripts" / override,
        Path(override),
        Path("scripts") / override,
    ]
    for path in candidates:
        if path.is_file():
            return path
    return None


def snapshot_script(script: str, label: str, output: Path, cwd: str | None) -> str | None:
    source = script_snapshot_source(script, cwd)
    if not source:
        return None

    snapshot_dir = output / "script-snapshots"
    snapshot_dir.mkdir(parents=True, exist_ok=True)
    destination = snapshot_dir / f"{label}{source.suffix or '.lua'}"
    shutil.copy2(source, destination)
    return str(destination)


def make_side_args(args: argparse.Namespace, side_output: Path, script: str) -> argparse.Namespace:
    return argparse.Namespace(
        client=args.client,
        map=args.map,
        ai=args.ai or ["ScriptedAdventureAI"],
        runs=args.runs,
        testdays=args.testdays,
        timeout=args.timeout,
        output=side_output,
        cwd=args.cwd,
        clean=args.clean,
        extra_arg=args.extra_arg,
        script=script,
        trace=True,
        json=False,
    )


def run_side(args: argparse.Namespace, label: str, script: str) -> list[dict[str, Any]]:
    side_output = args.output / label
    if side_output.exists() and args.clean:
        shutil.rmtree(side_output)
    side_output.mkdir(parents=True, exist_ok=True)

    side_args = make_side_args(args, side_output, script)
    results: list[dict[str, Any]] = []
    for game_map in side_args.map:
        for run_index in range(1, max(1, side_args.runs) + 1):
            result = run_one(side_args, game_map, run_index)
            result["side"] = label
            results.append(result)
            status = "timeout" if result["timedOut"] else f"exit {result['returnCode']}"
            parsed = result["traceSummary"]["parsed"]
            print(f"{label} {game_map} run {run_index}: {status}, traces parsed={parsed}, dir={result['runDir']}")

    (side_output / "manifest.json").write_text(json.dumps({"runs": results}, indent=2, sort_keys=True), encoding="utf-8")
    return results


def trace_dirs(results: list[dict[str, Any]]) -> list[str]:
    return [str(result["traceDir"]) for result in results]


def run_metrics(results: list[dict[str, Any]], summary: dict[str, Any]) -> dict[str, Any]:
    timeouts = sum(1 for result in results if result["timedOut"])
    nonzero = sum(1 for result in results if not result["timedOut"] and result["returnCode"] != 0)
    completed = sum(1 for result in results if not result["timedOut"] and result["returnCode"] == 0)
    elapsed = sum(float(result["elapsedSeconds"]) for result in results)
    parse_errors = len(summary.get("parse_errors", []))
    fallback_outputs = nested_int(summary, "output_statuses", "fallback")
    failed_actions = nested_int(summary, "progress", "failed")
    stopped_batches = nested_int(summary, "progress", "stopped_batches")
    executed_actions = nested_int(summary, "progress", "executed")
    visit_actions = nested_int(summary, "executed_actions", "visit_object")
    move_actions = nested_int(summary, "executed_actions", "move_hero")
    build_actions = nested_int(summary, "executed_actions", "build")
    recruit_actions = nested_int(summary, "executed_actions", "recruit")

    score = (
        completed * 1000
        - timeouts * 1200
        - nonzero * 600
        - parse_errors * 100
        - fallback_outputs * 250
        - failed_actions * 100
        - stopped_batches * 25
        + executed_actions * 5
        + visit_actions * 35
        + move_actions * 15
        + build_actions * 20
        + recruit_actions * 15
    )

    return {
        "runs": len(results),
        "completed": completed,
        "timeouts": timeouts,
        "nonzeroExit": nonzero,
        "elapsedSeconds": round(elapsed, 3),
        "parseErrors": parse_errors,
        "fallbackOutputs": fallback_outputs,
        "failedActions": failed_actions,
        "stoppedBatches": stopped_batches,
        "executedActions": executed_actions,
        "visitActions": visit_actions,
        "moveActions": move_actions,
        "buildActions": build_actions,
        "recruitActions": recruit_actions,
        "score": score,
    }


def print_metrics(label: str, metrics: dict[str, Any]) -> None:
    print(
        f"{label}: score={metrics['score']} completed={metrics['completed']}/{metrics['runs']} "
        f"timeouts={metrics['timeouts']} failed_actions={metrics['failedActions']} "
        f"fallbacks={metrics['fallbackOutputs']} executed={metrics['executedActions']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two ScriptedAdventureAI Lua scripts on fixed headless runs.")
    parser.add_argument("--client", required=True, help="Path to vcmiclient.")
    parser.add_argument("--map", action="append", required=True, help="VCMI map resource path. Can be repeated.")
    parser.add_argument("--baseline-script", required=True, help="Baseline script resource path or local Lua file.")
    parser.add_argument("--candidate-script", required=True, help="Candidate script resource path or local Lua file.")
    parser.add_argument("--ai", action="append", default=None, help="AI names for consecutive players.")
    parser.add_argument("--runs", type=int, default=3, help="Runs per map and side.")
    parser.add_argument("--testdays", type=int, default=7, help="Completed adventure days before each client exits.")
    parser.add_argument("--timeout", type=int, default=300, help="Seconds before stopping one run.")
    parser.add_argument("--output", type=Path, default=Path("scripted-ai-evaluation"), help="Evaluation output directory.")
    parser.add_argument("--cwd", default=None, help="Working directory for vcmiclient.")
    parser.add_argument("--clean", action="store_true", help="Delete existing side/run directories before reuse.")
    parser.add_argument("--extra-arg", action="append", default=[], help="Extra argument passed to vcmiclient.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable evaluation JSON.")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    baseline_snapshot = snapshot_script(args.baseline_script, "baseline", args.output, args.cwd)
    candidate_snapshot = snapshot_script(args.candidate_script, "candidate", args.output, args.cwd)

    baseline_results = run_side(args, "baseline", args.baseline_script)
    candidate_results = run_side(args, "candidate", args.candidate_script)
    comparison = compare(trace_dirs(baseline_results), trace_dirs(candidate_results))
    baseline_metrics = run_metrics(baseline_results, comparison["baseline"])
    candidate_metrics = run_metrics(candidate_results, comparison["candidate"])
    score_delta = candidate_metrics["score"] - baseline_metrics["score"]

    evaluation = {
        "baseline": {
            "script": script_override_value(args.baseline_script) or args.baseline_script,
            "snapshot": baseline_snapshot,
            "metrics": baseline_metrics,
            "runs": baseline_results,
        },
        "candidate": {
            "script": script_override_value(args.candidate_script) or args.candidate_script,
            "snapshot": candidate_snapshot,
            "metrics": candidate_metrics,
            "runs": candidate_results,
        },
        "comparison": comparison,
        "scoreDelta": score_delta,
        "scoreNotes": "Heuristic iteration score: reward completed runs and useful actions; penalize timeouts, nonzero exits, parse errors, fallbacks, and failed actions.",
    }

    (args.output / "evaluation.json").write_text(json.dumps(evaluation, indent=2, sort_keys=True), encoding="utf-8")
    if args.json:
        print(json.dumps(evaluation, indent=2, sort_keys=True))
    else:
        print_metrics("baseline", baseline_metrics)
        print_metrics("candidate", candidate_metrics)
        marker = "improved" if score_delta > 0 else "regressed" if score_delta < 0 else "unchanged"
        print(f"score delta: {score_delta} ({marker})")
        print(f"evaluation: {args.output / 'evaluation.json'}")

    has_failed_runs = baseline_metrics["timeouts"] or baseline_metrics["nonzeroExit"] or candidate_metrics["timeouts"] or candidate_metrics["nonzeroExit"]
    has_parse_errors = baseline_metrics["parseErrors"] or candidate_metrics["parseErrors"]
    return 1 if has_failed_runs or has_parse_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
