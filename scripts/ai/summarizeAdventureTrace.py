#!/usr/bin/env python3
"""Summarize ScriptedAdventureAI trace JSON files."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any


DAY_PATTERN = re.compile(r"-day-(\d+)-")


def as_list(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    return []


def as_dict(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    return {}


def nested(value: Any, *keys: str) -> Any:
    for key in keys:
        value = as_dict(value).get(key)
    return value


def iter_trace_files(paths: list[str]) -> list[Path]:
    files: list[Path] = []
    for raw_path in paths:
        path = Path(raw_path).expanduser()
        if path.is_dir():
            files.extend(sorted(path.glob("*.json")))
        elif path.is_file():
            files.append(path)
        else:
            print(f"warning: trace path does not exist: {path}", file=sys.stderr)
    return files


def counter_to_dict(counter: Counter[str]) -> dict[str, int]:
    return dict(counter.most_common())


def count_actions(counter: Counter[str], actions: Any) -> None:
    for action in as_list(actions):
        action_type = as_dict(action).get("type", "<missing>")
        counter[str(action_type)] += 1


def day_from_path(path: Path) -> str | None:
    match = DAY_PATTERN.search(path.name)
    if match:
        return match.group(1)
    return None


def summarize(files: list[Path]) -> dict[str, Any]:
    labels: Counter[str] = Counter()
    players: Counter[str] = Counter()
    scripts: Counter[str] = Counter()
    days: Counter[str] = Counter()
    output_statuses: Counter[str] = Counter()
    output_intents: Counter[str] = Counter()
    requested_actions: Counter[str] = Counter()
    executed_actions: Counter[str] = Counter()
    failed_actions: Counter[str] = Counter()
    failure_errors: Counter[str] = Counter()
    update_types: Counter[str] = Counter()
    opponent_update_types: Counter[str] = Counter()
    parse_errors: list[str] = []
    progress_counts: Counter[str] = Counter()
    analysis_counts: Counter[str] = Counter()

    for path in files:
        try:
            with path.open("r", encoding="utf-8") as handle:
                trace = json.load(handle)
        except (OSError, json.JSONDecodeError) as error:
            parse_errors.append(f"{path}: {error}")
            continue

        label = str(trace.get("label", "<missing>"))
        labels[label] += 1
        players[str(trace.get("player", "<missing>"))] += 1
        scripts[str(trace.get("script", "<missing>"))] += 1
        if day := day_from_path(path):
            days[day] += 1

        payload = as_dict(trace.get("payload"))
        if label == "output":
            output = as_dict(payload.get("output"))
            output_statuses[str(output.get("status", "<missing>"))] += 1
            if output.get("intent"):
                output_intents[str(output["intent"])] += 1
            count_actions(requested_actions, output.get("actions"))
        elif label == "progress":
            progress = as_dict(payload.get("progress"))
            executed = as_list(progress.get("executed"))
            failed = as_list(progress.get("failed"))
            remaining = as_list(progress.get("remaining"))
            progress_counts["executed"] += len(executed)
            progress_counts["failed"] += len(failed)
            progress_counts["remaining"] += len(remaining)
            if payload.get("stopped"):
                progress_counts["stopped_batches"] += 1
            count_actions(executed_actions, executed)
            count_actions(failed_actions, failed)
            for item in failed:
                error = as_dict(item).get("error", "<missing>")
                failure_errors[str(error)] += 1
        elif label == "input":
            script_input = as_dict(payload.get("input"))
            updates = as_list(nested(script_input, "updates", "events"))
            opponent_updates = as_list(nested(script_input, "opponentUpdates", "events"))
            analysis = as_dict(script_input.get("analysis"))
            progress_counts["input_update_events"] += len(updates)
            progress_counts["input_opponent_events"] += len(opponent_updates)
            analysis_counts["defense_alerts"] += len(as_list(analysis.get("defenseAlerts")))
            analysis_counts["visible_enemy_heroes"] += len(as_list(analysis.get("visibleEnemyHeroes")))
            analysis_counts["visible_enemy_towns"] += len(as_list(analysis.get("visibleEnemyTowns")))
            for event in updates:
                update_types[str(as_dict(event).get("type", "<missing>"))] += 1
            for event in opponent_updates:
                opponent_update_types[str(as_dict(event).get("type", "<missing>"))] += 1

    return {
        "files": len(files),
        "parsed": len(files) - len(parse_errors),
        "parse_errors": parse_errors,
        "labels": counter_to_dict(labels),
        "players": counter_to_dict(players),
        "scripts": counter_to_dict(scripts),
        "days": counter_to_dict(days),
        "output_statuses": counter_to_dict(output_statuses),
        "output_intents": counter_to_dict(output_intents),
        "requested_actions": counter_to_dict(requested_actions),
        "executed_actions": counter_to_dict(executed_actions),
        "failed_actions": counter_to_dict(failed_actions),
        "failure_errors": counter_to_dict(failure_errors),
        "progress": counter_to_dict(progress_counts),
        "analysis": counter_to_dict(analysis_counts),
        "update_types": counter_to_dict(update_types),
        "opponent_update_types": counter_to_dict(opponent_update_types),
    }


def print_counter(title: str, values: dict[str, int], limit: int) -> None:
    if not values:
        return
    print(f"{title}:")
    for key, count in list(values.items())[:limit]:
        print(f"  {key}: {count}")


def print_text(summary: dict[str, Any], limit: int) -> None:
    print(f"files: {summary['files']}")
    print(f"parsed: {summary['parsed']}")
    print_counter("labels", summary["labels"], limit)
    print_counter("players", summary["players"], limit)
    print_counter("scripts", summary["scripts"], limit)
    print_counter("days", summary["days"], limit)
    print_counter("output statuses", summary["output_statuses"], limit)
    print_counter("requested actions", summary["requested_actions"], limit)
    print_counter("executed actions", summary["executed_actions"], limit)
    print_counter("failed actions", summary["failed_actions"], limit)
    print_counter("failure errors", summary["failure_errors"], limit)
    print_counter("progress totals", summary["progress"], limit)
    print_counter("analysis totals", summary["analysis"], limit)
    print_counter("update types", summary["update_types"], limit)
    print_counter("opponent update types", summary["opponent_update_types"], limit)
    print_counter("intents", summary["output_intents"], limit)
    if summary["parse_errors"]:
        print("parse errors:")
        for error in summary["parse_errors"][:limit]:
            print(f"  {error}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize ScriptedAdventureAI trace JSON files.")
    parser.add_argument("paths", nargs="+", help="Trace files or directories containing trace JSON files.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON summary.")
    parser.add_argument("--limit", type=int, default=12, help="Maximum entries per text section.")
    args = parser.parse_args()

    files = iter_trace_files(args.paths)
    summary = summarize(files)
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print_text(summary, max(1, args.limit))
    return 1 if summary["parse_errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
