#!/usr/bin/env python3
"""Run baseline-vs-candidate ScriptedAdventureAI evaluations."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import shutil
import sys
from collections import Counter
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from compareAdventureTrace import compare  # noqa: E402
from runAdventureAIBatch import load_scenarios, normalize_ai_names, run_one, safe_name, scenario_source, script_override_value  # noqa: E402


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
        map=[scenario.get("map") or scenario.get("save")],
        group=args.group,
        stage=args.stage,
        kind=args.kind,
        include_disabled=args.include_disabled,
        ai=args.ai or ["ScriptedAdventureAI"],
        runs=scenario["runs"],
        testdays=scenario["testdays"],
        timeout=scenario["timeout"],
        exit_grace_after_outcome=args.exit_grace_after_outcome,
        output=side_output,
        cwd=args.cwd,
        clean=args.clean,
        extra_arg=scenario["extra_arg"],
        script=script,
        trace=args.trace,
        json=False,
    )


def run_side(args: argparse.Namespace, label: str, script: str, scenarios: list[dict[str, Any]]) -> list[dict[str, Any]]:
    side_output = args.output / label
    if side_output.exists() and args.clean:
        shutil.rmtree(side_output)
    side_output.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []
    tasks: list[tuple[argparse.Namespace, dict[str, Any], int]] = []
    for scenario in scenarios:
        scenario_output = side_output / safe_name(str(scenario["name"]))
        scenario_args = make_side_args(args, scenario_output, script, scenario)
        for run_index in range(1, scenario["runs"] + 1):
            tasks.append((scenario_args, scenario, run_index))

    def finish_result(scenario: dict[str, Any], result: dict[str, Any]) -> None:
        result["side"] = label
        results.append(result)
        status = "timeout" if result["timedOut"] else f"exit {result['returnCode']}"
        parsed = result["traceSummary"]["parsed"]
        source_type, source = scenario_source(scenario)
        trace_status = f"traces parsed={parsed}" if args.trace else "traces disabled"
        print(
            f"{label} {scenario['name']} {source_type}:{source} run {result['run']}: "
            f"{status}, outcome={result['outcome']['result']}, {trace_status}, dir={result['runDir']}"
        )

    jobs = max(1, int(args.jobs))
    if jobs == 1:
        for scenario_args, scenario, run_index in tasks:
            finish_result(scenario, run_one(scenario_args, scenario, run_index))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_to_scenario = {
                executor.submit(run_one, scenario_args, scenario, run_index): scenario
                for scenario_args, scenario, run_index in tasks
            }
            for future in concurrent.futures.as_completed(future_to_scenario):
                finish_result(future_to_scenario[future], future.result())

    results.sort(key=lambda item: (str(item.get("scenario")), int(item.get("run", 0))))

    (side_output / "manifest.json").write_text(json.dumps({"runs": results}, indent=2, sort_keys=True), encoding="utf-8")
    return results


def trace_dirs(results: list[dict[str, Any]]) -> list[str]:
    return [str(result["traceDir"]) for result in results]


def outcome_counts(results: list[dict[str, Any]]) -> Counter[str]:
    counts: Counter[str] = Counter()
    for result in results:
        outcome = result.get("outcome", {})
        if isinstance(outcome, dict):
            counts[str(outcome.get("result", "unknown"))] += 1
        else:
            counts["unknown"] += 1
    return counts


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
    outcomes = outcome_counts(results)

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
    outcome_score = outcomes["red_win"] * 4000 - outcomes["red_loss"] * 4000
    score = safety_score + activity_score + quality_score + outcome_score - mistake_penalty

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
        "outcomes": dict(outcomes),
        "redWins": outcomes["red_win"],
        "redLosses": outcomes["red_loss"],
        "safetyScore": safety_score,
        "activityScore": activity_score,
        "outcomeScore": outcome_score,
        "mistakePenalty": mistake_penalty,
        "score": score,
    }


def metric_delta(baseline: dict[str, Any], candidate: dict[str, Any]) -> dict[str, int]:
    fields = (
        "score",
        "qualityScore",
        "mistakes",
        "importantMistakes",
        "fallbackOutputs",
        "failedActions",
        "redWins",
        "redLosses",
    )
    return {field: int(candidate.get(field, 0)) - int(baseline.get(field, 0)) for field in fields}


def compare_result_traces(trace_enabled: bool, baseline_results: list[dict[str, Any]], candidate_results: list[dict[str, Any]]) -> dict[str, Any]:
    if not trace_enabled:
        return compare([], [])
    return compare(trace_dirs(baseline_results), trace_dirs(candidate_results))


def bucket_report_for_trace_mode(trace_enabled: bool, baseline_results: list[dict[str, Any]], candidate_results: list[dict[str, Any]]) -> dict[str, Any]:
    comparison = compare_result_traces(trace_enabled, baseline_results, candidate_results)
    baseline_metrics = run_metrics(baseline_results, comparison["baseline"])
    candidate_metrics = run_metrics(candidate_results, comparison["candidate"])
    return {
        "baseline": baseline_metrics,
        "candidate": candidate_metrics,
        "delta": metric_delta(baseline_metrics, candidate_metrics),
    }


def scenario_buckets(
    trace_enabled: bool,
    scenarios: list[dict[str, Any]],
    baseline_results: list[dict[str, Any]],
    candidate_results: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    reports: dict[str, dict[str, Any]] = {}
    for field in ("group", "stage", "kind"):
        reports[field] = {}
        values = sorted({str(scenario.get(field, "")) for scenario in scenarios if scenario.get(field)})
        for value in values:
            baseline_subset = [result for result in baseline_results if str(result.get(field)) == value]
            candidate_subset = [result for result in candidate_results if str(result.get(field)) == value]
            if baseline_subset or candidate_subset:
                reports[field][value] = bucket_report_for_trace_mode(trace_enabled, baseline_subset, candidate_subset)
    return reports


def bucket_delta(buckets: dict[str, dict[str, Any]], group: str) -> dict[str, int] | None:
    report = buckets.get("group", {}).get(group)
    if not report:
        return None
    delta = report.get("delta")
    return delta if isinstance(delta, dict) else None


def promotion_verdict(
    args: argparse.Namespace,
    baseline: dict[str, Any],
    candidate: dict[str, Any],
    buckets: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    score_delta = candidate["score"] - baseline["score"]
    quality_delta = candidate["qualityScore"] - baseline["qualityScore"]
    training_delta = bucket_delta(buckets, "training")
    heldout_delta = bucket_delta(buckets, "heldout")
    reasons: list[str] = []
    gates = {
        "scoreDelta": score_delta,
        "qualityDelta": quality_delta,
        "trainingScoreDelta": training_delta.get("score") if training_delta else None,
        "trainingQualityDelta": training_delta.get("qualityScore") if training_delta else None,
        "heldoutScoreDelta": heldout_delta.get("score") if heldout_delta else None,
        "heldoutQualityDelta": heldout_delta.get("qualityScore") if heldout_delta else None,
        "heldoutImportantMistakeDelta": heldout_delta.get("importantMistakes") if heldout_delta else None,
        "candidateCompletedAllRuns": candidate["completed"] == candidate["runs"],
        "candidateHasNoTimeouts": candidate["timeouts"] == 0,
        "candidateHasNoNonzeroExits": candidate["nonzeroExit"] == 0,
        "candidateHasNoParseErrors": candidate["parseErrors"] == 0,
        "candidateFallbacksNotWorse": candidate["fallbackOutputs"] <= baseline["fallbackOutputs"],
        "candidateFailedActionsAllowed": candidate["failedActions"] <= baseline["failedActions"] + args.allow_more_failed_actions,
        "candidateImportantMistakesAllowed": candidate["importantMistakes"] <= baseline["importantMistakes"] + args.allow_more_important_mistakes,
        "scoreDeltaEnough": score_delta >= args.min_score_delta,
        "qualityDeltaEnough": quality_delta >= args.min_quality_delta,
        "trainingImproved": training_delta is None
        or (
            training_delta.get("score", 0) >= args.min_training_score_delta
            and training_delta.get("qualityScore", 0) >= args.min_training_quality_delta
        ),
        "heldoutNotRegressed": heldout_delta is None
        or (
            heldout_delta.get("score", 0) >= -args.allow_heldout_score_regression
            and heldout_delta.get("qualityScore", 0) >= -args.allow_heldout_quality_regression
            and heldout_delta.get("importantMistakes", 0) <= args.allow_heldout_important_mistake_regression
        ),
    }

    hard_fail_keys = [
        "candidateCompletedAllRuns",
        "candidateHasNoTimeouts",
        "candidateHasNoNonzeroExits",
        "candidateHasNoParseErrors",
        "candidateFallbacksNotWorse",
        "candidateFailedActionsAllowed",
        "candidateImportantMistakesAllowed",
        "trainingImproved",
        "heldoutNotRegressed",
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
        "notes": "Promote only when hard safety gates pass, training improves, held-out groups do not regress, and score/quality deltas meet configured thresholds.",
    }


def print_metrics(label: str, metrics: dict[str, Any]) -> None:
    print(
        f"{label}: score={metrics['score']} completed={metrics['completed']}/{metrics['runs']} "
        f"timeouts={metrics['timeouts']} failed_actions={metrics['failedActions']} "
        f"fallbacks={metrics['fallbackOutputs']} executed={metrics['executedActions']} "
        f"quality={metrics['qualityScore']} mistakes={metrics['mistakes']}/{metrics['importantMistakes']} "
        f"red_w/l={metrics['redWins']}/{metrics['redLosses']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two ScriptedAdventureAI Lua scripts on fixed headless runs.")
    parser.add_argument("--client", required=True, help="Path to vcmiclient.")
    parser.add_argument("--map", action="append", default=[], help="VCMI map resource path. Can be repeated.")
    parser.add_argument("--scenario-file", type=Path, help="JSON file with evaluation scenarios.")
    parser.add_argument("--group", action="append", default=[], help="Only run scenarios in this group. Can be repeated.")
    parser.add_argument("--stage", action="append", default=[], help="Only run scenarios in this ladder stage. Can be repeated.")
    parser.add_argument("--kind", action="append", default=[], help="Only run scenarios of this kind. Can be repeated.")
    parser.add_argument("--include-disabled", action="store_true", help="Include scenarios marked enabled=false.")
    parser.add_argument("--baseline-script", required=True, help="Baseline script resource path or local Lua file.")
    parser.add_argument("--candidate-script", required=True, help="Candidate script resource path or local Lua file.")
    parser.add_argument("--ai", action="append", default=None, help="AI names for consecutive players.")
    parser.add_argument("--runs", type=int, default=3, help="Runs per map and side.")
    parser.add_argument("--testdays", type=int, default=7, help="Completed adventure days before each client exits.")
    parser.add_argument("--timeout", type=int, default=300, help="Seconds before stopping one run.")
    parser.add_argument("--exit-grace-after-outcome", type=float, default=10.0, help="Seconds to wait for clean client exit after a terminal game outcome appears in stdout.")
    parser.add_argument("--output", type=Path, default=Path("scripted-ai-evaluation"), help="Evaluation output directory.")
    parser.add_argument("--cwd", default=None, help="Working directory for vcmiclient.")
    parser.add_argument("--clean", action="store_true", help="Delete existing side/run directories before reuse.")
    parser.add_argument("--extra-arg", action="append", default=[], help="Extra argument passed to vcmiclient.")
    parser.add_argument("--jobs", type=int, default=1, help="Number of runs per evaluation side to execute in parallel.")
    parser.add_argument("--trace", dest="trace", action="store_true", default=True, help="Enable ScriptedAdventureAI trace collection for trace-local metrics.")
    parser.add_argument("--no-trace", dest="trace", action="store_false", help="Disable trace collection for outcome-focused promotion runs.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable evaluation JSON.")
    parser.add_argument("--min-score-delta", type=int, default=1, help="Minimum total score delta required for promotion.")
    parser.add_argument("--min-quality-delta", type=int, default=0, help="Minimum final-state quality delta required for promotion.")
    parser.add_argument("--min-training-score-delta", type=int, default=1, help="Minimum training-group score delta required for promotion.")
    parser.add_argument("--min-training-quality-delta", type=int, default=0, help="Minimum training-group quality delta required for promotion.")
    parser.add_argument("--allow-heldout-score-regression", type=int, default=0, help="Allowed held-out score regression before rejecting promotion.")
    parser.add_argument("--allow-heldout-quality-regression", type=int, default=0, help="Allowed held-out quality regression before rejecting promotion.")
    parser.add_argument("--allow-heldout-important-mistake-regression", type=int, default=0, help="Allowed held-out important-mistake increase before rejecting promotion.")
    parser.add_argument("--allow-more-failed-actions", type=int, default=0, help="Candidate failed actions allowed above baseline.")
    parser.add_argument("--allow-more-important-mistakes", type=int, default=0, help="Candidate important mistakes allowed above baseline.")
    args = parser.parse_args()
    args.ai = normalize_ai_names(args.ai)

    args.output.mkdir(parents=True, exist_ok=True)
    scenarios = load_scenarios(args)
    baseline_snapshot = snapshot_script(args.baseline_script, "baseline", args.output, args.cwd)
    candidate_snapshot = snapshot_script(args.candidate_script, "candidate", args.output, args.cwd)

    baseline_results = run_side(args, "baseline", args.baseline_script, scenarios)
    candidate_results = run_side(args, "candidate", args.candidate_script, scenarios)
    comparison = compare_result_traces(args.trace, baseline_results, candidate_results)
    baseline_metrics = run_metrics(baseline_results, comparison["baseline"])
    candidate_metrics = run_metrics(candidate_results, comparison["candidate"])
    buckets = scenario_buckets(args.trace, scenarios, baseline_results, candidate_results)
    score_delta = candidate_metrics["score"] - baseline_metrics["score"]
    quality_delta = candidate_metrics["qualityScore"] - baseline_metrics["qualityScore"]
    promotion = promotion_verdict(args, baseline_metrics, candidate_metrics, buckets)

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
        "buckets": buckets,
        "traceEnabled": args.trace,
        "scoreDelta": score_delta,
        "qualityDelta": quality_delta,
        "promotion": promotion,
        "scoreNotes": "Iteration score combines safety, useful actions, final visible-state quality, mined mistake penalties, and outcomes when traces are enabled. With --no-trace, trace-local quality, action, and mistake metrics are zeroed and the score is outcome/safety focused. Hard promotion gates reject crashes, parse errors, extra fallbacks, extra failed actions, and extra important mistakes.",
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
