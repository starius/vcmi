#!/usr/bin/env python3
"""Run headless ScriptedAdventureAI matches and summarize traces."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from summarizeAdventureTrace import iter_trace_files, summarize  # noqa: E402


DEFAULT_SCENARIO_GROUP = "training"
DEFAULT_SCENARIO_KIND = "handcrafted"
DEFAULT_SCENARIO_STAGE = "smoke"


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def safe_name(value: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in value).strip("_") or "scenario"


def string_list(value: Any) -> list[str]:
    return [str(item) for item in as_list(value)]


def script_override_value(script: str | None) -> str | None:
    if not script:
        return None
    if script.startswith("file:"):
        return script
    path = Path(script).expanduser()
    if path.is_file():
        return f"file:{path.resolve()}"
    return script


def scenario_source(scenario: dict[str, Any]) -> tuple[str, str]:
    if scenario.get("save"):
        return "save", str(scenario["save"])
    return "map", str(scenario["map"])


def command_for_run(args: argparse.Namespace, scenario: dict[str, Any], run_dir: Path) -> list[str]:
    source_type, source = scenario_source(scenario)
    command = [
        args.client,
        "--headless",
        "--testsave" if source_type == "save" else "--testmap",
        source,
        "--logLocation",
        str(run_dir / "logs"),
    ]
    if args.testdays:
        command.extend(["--testdays", str(args.testdays)])
    if scenario.get("gameSeed") is not None:
        command.extend(["--seed", str(scenario["gameSeed"])])
    for ai_name in args.ai:
        command.extend(["--ai", ai_name])
    command.extend(args.extra_arg)
    return command


def trace_dir_for_run(run_dir: Path) -> Path:
    return run_dir / "cache" / "vcmi" / "scriptedAdventureAI"


def normalize_scenario(raw: dict[str, Any], index: int, args: argparse.Namespace) -> dict[str, Any]:
    if not raw.get("map") and not raw.get("save"):
        raise ValueError(f"Scenario entry {index} is missing 'map' or 'save'")

    source = str(raw.get("map") or raw.get("save"))
    scenario = {
        "name": str(raw.get("name") or safe_name(source)),
        "group": str(raw.get("group", DEFAULT_SCENARIO_GROUP)),
        "stage": str(raw.get("stage", DEFAULT_SCENARIO_STAGE)),
        "kind": str(raw.get("kind", DEFAULT_SCENARIO_KIND)),
        "runs": int(raw.get("runs", args.runs)),
        "testdays": int(raw.get("testdays", args.testdays)),
        "timeout": int(raw.get("timeout", args.timeout)),
        "extra_arg": list(args.extra_arg) + string_list(raw.get("extraArg")),
        "enabled": bool(raw.get("enabled", True)),
        "tags": string_list(raw.get("tags")),
    }
    if raw.get("map"):
        scenario["map"] = str(raw["map"])
    if raw.get("save"):
        scenario["save"] = str(raw["save"])
    for field in ("gameSeed", "seed", "template", "size", "levels", "water", "monsterStrength", "notes"):
        if field in raw:
            scenario[field] = raw[field]
    return scenario


def scenario_selected(args: argparse.Namespace, scenario: dict[str, Any]) -> bool:
    if not scenario.get("enabled", True) and not getattr(args, "include_disabled", False):
        return False
    if getattr(args, "group", []) and scenario.get("group") not in args.group:
        return False
    if getattr(args, "stage", []) and scenario.get("stage") not in args.stage:
        return False
    if getattr(args, "kind", []) and scenario.get("kind") not in args.kind:
        return False
    return True


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
            normalized = normalize_scenario(scenario, index, args)
            if scenario_selected(args, normalized):
                scenarios.append(normalized)

    for game_map in args.map or []:
        scenario = {
            "name": safe_name(game_map),
            "group": DEFAULT_SCENARIO_GROUP,
            "stage": DEFAULT_SCENARIO_STAGE,
            "kind": DEFAULT_SCENARIO_KIND,
            "map": game_map,
            "runs": args.runs,
            "testdays": args.testdays,
            "timeout": args.timeout,
            "extra_arg": list(args.extra_arg),
            "enabled": True,
            "tags": [],
        }
        if scenario_selected(args, scenario):
            scenarios.append(scenario)

    if not scenarios:
        raise ValueError("At least one enabled scenario from --map or --scenario-file is required")

    for scenario in scenarios:
        scenario["runs"] = max(1, int(scenario["runs"]))
        scenario["testdays"] = max(0, int(scenario["testdays"]))
        scenario["timeout"] = max(1, int(scenario["timeout"]))
    return scenarios


def args_for_scenario(args: argparse.Namespace, scenario: dict[str, Any]) -> argparse.Namespace:
    return argparse.Namespace(
        client=args.client,
        map=[scenario.get("map") or scenario.get("save")],
        ai=args.ai,
        runs=scenario["runs"],
        testdays=scenario["testdays"],
        timeout=scenario["timeout"],
        output=args.output / safe_name(str(scenario["name"])),
        cwd=args.cwd,
        clean=args.clean,
        extra_arg=scenario["extra_arg"],
        script=args.script,
        trace=args.trace,
        json=args.json,
    )


def run_outcome(stdout_path: Path, timed_out: bool, return_code: int | None) -> dict[str, Any]:
    text = stdout_path.read_text(encoding="utf-8", errors="replace") if stdout_path.exists() else ""
    outcome = {
        "result": "unknown",
        "completedDays": None,
    }
    if timed_out:
        outcome["result"] = "timeout"
    elif return_code not in (0, None):
        outcome["result"] = "nonzero_exit"
    if "Red player won. Ending game." in text:
        outcome["result"] = "red_win"
    elif "Red player lost. Ending game." in text:
        outcome["result"] = "red_loss"

    if match := re.search(r"Reached test day limit \d+ after completing day (\d+)", text):
        outcome["completedDays"] = int(match.group(1))
        if outcome["result"] == "unknown":
            outcome["result"] = "day_limit"
    return outcome


def run_one(args: argparse.Namespace, scenario_or_map: dict[str, Any] | str, run_index: int) -> dict[str, Any]:
    if isinstance(scenario_or_map, dict):
        scenario = scenario_or_map
    else:
        scenario = {
            "name": safe_name(scenario_or_map),
            "group": DEFAULT_SCENARIO_GROUP,
            "stage": DEFAULT_SCENARIO_STAGE,
            "kind": DEFAULT_SCENARIO_KIND,
            "map": scenario_or_map,
            "runs": args.runs,
            "testdays": args.testdays,
            "timeout": args.timeout,
            "extra_arg": list(args.extra_arg),
            "enabled": True,
            "tags": [],
        }
    source_type, source = scenario_source(scenario)
    safe_map = safe_name(source)
    run_dir = args.output / f"{safe_map}-run-{run_index:03d}"
    if run_dir.exists() and args.clean:
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    command = command_for_run(args, scenario, run_dir)
    env = os.environ.copy()
    env["XDG_CACHE_HOME"] = str(run_dir / "cache")
    if script := script_override_value(args.script):
        env["VCMI_SCRIPTED_ADVENTURE_SCRIPT"] = script
    if args.trace:
        env["VCMI_SCRIPTED_ADVENTURE_TRACE"] = "1"
    started = time.monotonic()
    timed_out = False
    return_code: int | None
    stdout_path = run_dir / "stdout.log"

    with stdout_path.open("w", encoding="utf-8") as stdout:
        stdout.write("$ " + " ".join(command) + "\n")
        stdout.flush()
        try:
            completed = subprocess.run(
                command,
                cwd=args.cwd,
                env=env,
                stdout=stdout,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                check=False,
            )
            return_code = completed.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            return_code = None

    trace_dir = trace_dir_for_run(run_dir)
    trace_summary = summarize(iter_trace_files([str(trace_dir)])) if trace_dir.exists() else summarize([])
    result = {
        "scenario": scenario.get("name"),
        "group": scenario.get("group"),
        "stage": scenario.get("stage"),
        "kind": scenario.get("kind"),
        "sourceType": source_type,
        "source": source,
        "map": scenario.get("map"),
        "save": scenario.get("save"),
        "seed": scenario.get("seed"),
        "template": scenario.get("template"),
        "tags": scenario.get("tags", []),
        "run": run_index,
        "runDir": str(run_dir),
        "command": command,
        "timeoutSeconds": args.timeout,
        "timedOut": timed_out,
        "returnCode": return_code,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "script": script_override_value(args.script),
        "trace": bool(args.trace),
        "traceDir": str(trace_dir),
        "traceSummary": trace_summary,
        "outcome": run_outcome(stdout_path, timed_out, return_code),
    }
    (run_dir / "run.json").write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Run headless ScriptedAdventureAI matches and summarize traces.")
    parser.add_argument("--client", required=True, help="Path to vcmiclient.")
    parser.add_argument("--map", action="append", default=[], help="VCMI map resource path. Can be repeated.")
    parser.add_argument("--scenario-file", type=Path, help="JSON file with batch scenarios.")
    parser.add_argument("--group", action="append", default=[], help="Only run scenarios in this group. Can be repeated.")
    parser.add_argument("--stage", action="append", default=[], help="Only run scenarios in this ladder stage. Can be repeated.")
    parser.add_argument("--kind", action="append", default=[], help="Only run scenarios of this kind. Can be repeated.")
    parser.add_argument("--include-disabled", action="store_true", help="Include scenarios marked enabled=false.")
    parser.add_argument("--ai", action="append", default=None, help="AI names for consecutive players.")
    parser.add_argument("--runs", type=int, default=1, help="Runs per map.")
    parser.add_argument("--testdays", type=int, default=0, help="Completed adventure days before the client exits.")
    parser.add_argument("--timeout", type=int, default=300, help="Seconds before stopping one run.")
    parser.add_argument("--output", type=Path, default=Path("scripted-ai-runs"), help="Directory for run outputs.")
    parser.add_argument("--cwd", default=None, help="Working directory for vcmiclient.")
    parser.add_argument("--clean", action="store_true", help="Delete existing run directories before reuse.")
    parser.add_argument("--extra-arg", action="append", default=[], help="Extra argument passed to vcmiclient.")
    parser.add_argument("--script", default=None, help="Script resource path or local Lua file used by ScriptedAdventureAI.")
    parser.add_argument("--trace", action="store_true", help="Enable ScriptedAdventureAI trace files for each run.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable batch manifest.")
    args = parser.parse_args()
    if args.ai is None:
        args.ai = ["ScriptedAdventureAI"]

    args.output.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, Any]] = []
    scenarios = load_scenarios(args)
    for scenario in scenarios:
        scenario_args = args_for_scenario(args, scenario)
        for run_index in range(1, scenario["runs"] + 1):
            result = run_one(scenario_args, scenario, run_index)
            results.append(result)
            if not args.json:
                status = "timeout" if result["timedOut"] else f"exit {result['returnCode']}"
                parsed = result["traceSummary"]["parsed"]
                print(
                    f"{scenario['name']} {scenario_source(scenario)[1]} run {run_index}: "
                    f"{status}, outcome={result['outcome']['result']}, traces parsed={parsed}, dir={result['runDir']}"
                )

    manifest = {"scenarios": scenarios, "runs": results}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    if args.json:
        print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
