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


def deep_int(summary: dict[str, Any], *keys: str) -> int:
    value: Any = summary
    for key in keys:
        if not isinstance(value, dict):
            return 0
        value = value.get(key, 0)
    return int(value) if isinstance(value, int) else 0


def safe_name(value: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in value).strip("_") or "scenario"


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def load_scenarios(args: argparse.Namespace) -> list[dict[str, Any]]:
    scenarios: list[dict[str, Any]] = []

    if args.scenario_file:
        with args.scenario_file.open("r", encoding="utf-8") as handle:
            raw = json.load(handle)
        if isinstance(raw, dict) and "map" in raw:
            raw_scenarios = [raw]
        elif isinstance(raw, dict):
            raw_scenarios = raw.get("scenarios", [])
        else:
            raw_scenarios = raw
        for index, scenario in enumerate(as_list(raw_scenarios), start=1):
            if not isinstance(scenario, dict):
                raise ValueError(f"Scenario entry {index} must be an object")
            if not scenario.get("map"):
                raise ValueError(f"Scenario entry {index} is missing 'map'")
            scenarios.append({
                "name": str(scenario.get("name") or safe_name(str(scenario["map"]))),
                "map": str(scenario["map"]),
                "runs": int(scenario.get("runs", args.runs)),
                "testdays": int(scenario.get("testdays", args.testdays)),
                "timeout": int(scenario.get("timeout", args.timeout)),
                "extra_arg": list(args.extra_arg) + [str(item) for item in as_list(scenario.get("extraArg"))],
            })

    for game_map in args.map or []:
        scenarios.append({
            "name": safe_name(game_map),
            "map": game_map,
            "runs": args.runs,
            "testdays": args.testdays,
            "timeout": args.timeout,
            "extra_arg": list(args.extra_arg),
        })

    if not scenarios:
        raise ValueError("At least one --map or --scenario-file entry is required")

    for scenario in scenarios:
        scenario["runs"] = max(1, int(scenario["runs"]))
        scenario["testdays"] = max(0, int(scenario["testdays"]))
        scenario["timeout"] = max(1, int(scenario["timeout"]))
    return scenarios


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


def make_side_args(args: argparse.Namespace, side_output: Path, script: str, scenario: dict[str, Any]) -> argparse.Namespace:
    return argparse.Namespace(
        client=args.client,
        map=[scenario["map"]],
        ai=args.ai or ["ScriptedAdventureAI"],
        runs=scenario["runs"],
        testdays=scenario["testdays"],
        timeout=scenario["timeout"],
        output=side_output,
        cwd=args.cwd,
        clean=args.clean,
        extra_arg=scenario["extra_arg"],
        script=script,
        trace=True,
        json=False,
    )


def run_side(args: argparse.Namespace, label: str, script: str, scenarios: list[dict[str, Any]]) -> list[dict[str, Any]]:
    side_output = args.output / label
    if side_output.exists() and args.clean:
        shutil.rmtree(side_output)
    side_output.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []
    for scenario in scenarios:
        scenario_output = side_output / safe_name(str(scenario["name"]))
        scenario_args = make_side_args(args, scenario_output, script, scenario)
        for run_index in range(1, scenario["runs"] + 1):
            result = run_one(scenario_args, scenario["map"], run_index)
            result["side"] = label
            result["scenario"] = scenario["name"]
            results.append(result)
            status = "timeout" if result["timedOut"] else f"exit {result['returnCode']}"
            parsed = result["traceSummary"]["parsed"]
            print(
                f"{label} {scenario['name']} {scenario['map']} run {run_index}: "
                f"{status}, traces parsed={parsed}, dir={result['runDir']}"
            )

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
    quality_score = deep_int(summary, "quality", "score")
    mistakes = deep_int(summary, "mistakes", "total")
    important_mistakes = deep_int(summary, "mistakes", "important")

    safety_score = (
        completed * 1000
        - timeouts * 1200
        - nonzero * 600
        - parse_errors * 100
        - fallback_outputs * 250
        - failed_actions * 100
        - stopped_batches * 25
    )
    activity_score = (
        + executed_actions * 5
        + visit_actions * 35
        + move_actions * 15
        + build_actions * 20
        + recruit_actions * 15
    )
    mistake_penalty = mistakes * 40 + important_mistakes * 160
    score = safety_score + activity_score + quality_score - mistake_penalty

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
        "qualityScore": quality_score,
        "mistakes": mistakes,
        "importantMistakes": important_mistakes,
        "safetyScore": safety_score,
        "activityScore": activity_score,
        "mistakePenalty": mistake_penalty,
        "score": score,
    }


def promotion_verdict(args: argparse.Namespace, baseline: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    score_delta = candidate["score"] - baseline["score"]
    quality_delta = candidate["qualityScore"] - baseline["qualityScore"]
    reasons: list[str] = []
    gates = {
        "scoreDelta": score_delta,
        "qualityDelta": quality_delta,
        "candidateCompletedAllRuns": candidate["completed"] == candidate["runs"],
        "candidateHasNoTimeouts": candidate["timeouts"] == 0,
        "candidateHasNoNonzeroExits": candidate["nonzeroExit"] == 0,
        "candidateHasNoParseErrors": candidate["parseErrors"] == 0,
        "candidateFallbacksNotWorse": candidate["fallbackOutputs"] <= baseline["fallbackOutputs"],
        "candidateFailedActionsAllowed": candidate["failedActions"] <= baseline["failedActions"] + args.allow_more_failed_actions,
        "candidateImportantMistakesAllowed": candidate["importantMistakes"] <= baseline["importantMistakes"] + args.allow_more_important_mistakes,
        "scoreDeltaEnough": score_delta >= args.min_score_delta,
        "qualityDeltaEnough": quality_delta >= args.min_quality_delta,
    }

    hard_fail_keys = [
        "candidateCompletedAllRuns",
        "candidateHasNoTimeouts",
        "candidateHasNoNonzeroExits",
        "candidateHasNoParseErrors",
        "candidateFallbacksNotWorse",
        "candidateFailedActionsAllowed",
        "candidateImportantMistakesAllowed",
    ]
    for key in hard_fail_keys:
        if not gates[key]:
            reasons.append(key)

    if reasons:
        verdict = "reject"
    elif gates["scoreDeltaEnough"] and gates["qualityDeltaEnough"]:
        verdict = "promote"
    elif score_delta < 0:
        verdict = "reject"
        reasons.append("scoreDeltaNegative")
    else:
        verdict = "inconclusive"
        if not gates["scoreDeltaEnough"]:
            reasons.append("scoreDeltaBelowThreshold")
        if not gates["qualityDeltaEnough"]:
            reasons.append("qualityDeltaBelowThreshold")

    return {
        "verdict": verdict,
        "reasons": reasons,
        "gates": gates,
        "notes": "Promote only when hard safety gates pass and candidate score/quality deltas meet configured thresholds.",
    }


def print_metrics(label: str, metrics: dict[str, Any]) -> None:
    print(
        f"{label}: score={metrics['score']} completed={metrics['completed']}/{metrics['runs']} "
        f"timeouts={metrics['timeouts']} failed_actions={metrics['failedActions']} "
        f"fallbacks={metrics['fallbackOutputs']} executed={metrics['executedActions']} "
        f"quality={metrics['qualityScore']} mistakes={metrics['mistakes']}/{metrics['importantMistakes']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two ScriptedAdventureAI Lua scripts on fixed headless runs.")
    parser.add_argument("--client", required=True, help="Path to vcmiclient.")
    parser.add_argument("--map", action="append", default=[], help="VCMI map resource path. Can be repeated.")
    parser.add_argument("--scenario-file", type=Path, help="JSON file with evaluation scenarios.")
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
    parser.add_argument("--min-score-delta", type=int, default=1, help="Minimum total score delta required for promotion.")
    parser.add_argument("--min-quality-delta", type=int, default=0, help="Minimum final-state quality delta required for promotion.")
    parser.add_argument("--allow-more-failed-actions", type=int, default=0, help="Candidate failed actions allowed above baseline.")
    parser.add_argument("--allow-more-important-mistakes", type=int, default=0, help="Candidate important mistakes allowed above baseline.")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    scenarios = load_scenarios(args)
    baseline_snapshot = snapshot_script(args.baseline_script, "baseline", args.output, args.cwd)
    candidate_snapshot = snapshot_script(args.candidate_script, "candidate", args.output, args.cwd)

    baseline_results = run_side(args, "baseline", args.baseline_script, scenarios)
    candidate_results = run_side(args, "candidate", args.candidate_script, scenarios)
    comparison = compare(trace_dirs(baseline_results), trace_dirs(candidate_results))
    baseline_metrics = run_metrics(baseline_results, comparison["baseline"])
    candidate_metrics = run_metrics(candidate_results, comparison["candidate"])
    score_delta = candidate_metrics["score"] - baseline_metrics["score"]
    quality_delta = candidate_metrics["qualityScore"] - baseline_metrics["qualityScore"]
    promotion = promotion_verdict(args, baseline_metrics, candidate_metrics)

    evaluation = {
        "scenarios": scenarios,
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
        "qualityDelta": quality_delta,
        "promotion": promotion,
        "scoreNotes": "Iteration score combines safety, useful actions, final visible-state quality, and mined mistake penalties. Hard promotion gates reject crashes, parse errors, extra fallbacks, extra failed actions, and extra important mistakes.",
    }

    (args.output / "evaluation.json").write_text(json.dumps(evaluation, indent=2, sort_keys=True), encoding="utf-8")
    if args.json:
        print(json.dumps(evaluation, indent=2, sort_keys=True))
    else:
        print_metrics("baseline", baseline_metrics)
        print_metrics("candidate", candidate_metrics)
        marker = "improved" if score_delta > 0 else "regressed" if score_delta < 0 else "unchanged"
        print(f"score delta: {score_delta} ({marker})")
        print(f"quality delta: {quality_delta}")
        print(f"promotion verdict: {promotion['verdict']} ({', '.join(promotion['reasons']) or 'all gates passed'})")
        print(f"evaluation: {args.output / 'evaluation.json'}")

    has_failed_runs = baseline_metrics["timeouts"] or baseline_metrics["nonzeroExit"] or candidate_metrics["timeouts"] or candidate_metrics["nonzeroExit"]
    has_parse_errors = baseline_metrics["parseErrors"] or candidate_metrics["parseErrors"]
    return 1 if has_failed_runs or has_parse_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
