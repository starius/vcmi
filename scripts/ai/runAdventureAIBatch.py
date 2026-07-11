#!/usr/bin/env python3
"""Run headless ScriptedAdventureAI matches and summarize traces."""

from __future__ import annotations

import argparse
import concurrent.futures
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
TERMINAL_OUTCOME_MARKERS = (
    "Red player won. Ending game.",
    "Red player lost. Ending game.",
)
TURN_START_DAY_RE = re.compile(r"Player \d+ \([^)]*\) starting turn, day (\d+)")
INFRASTRUCTURE_FAILURE_OUTCOMES = {"idle_timeout", "timeout", "nonzero_exit"}
INFRASTRUCTURE_FAILURE_TAIL_SIGNATURES = {"battle_ai_creation", "battle_ai_creation_invalid_stack"}
INFRASTRUCTURE_FAILURE_MISTAKES = {"inflight_imperative_command"}
TERMINAL_OUTCOMES = {"red_win", "red_loss"}
ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
AI_NAME_ALIASES = {
    "Nullkiller": "Nullkiller2",
}


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def safe_name(value: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in value).strip("_") or "scenario"


def string_list(value: Any) -> list[str]:
    return [str(item) for item in as_list(value)]


def normalize_ai_names(values: list[str] | None) -> list[str] | None:
    if values is None:
        return None
    return [AI_NAME_ALIASES.get(str(value), str(value)) for value in values]


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def script_override_value(script: str | None) -> str | None:
    if not script:
        return None
    if script.startswith("file:"):
        return script
    path = Path(script).expanduser()
    if path.is_file():
        return f"file:{path.resolve()}"
    return script


def player_script_env_name(player: str) -> str:
    key = str(player).strip()
    if not key:
        raise ValueError("Player script override is missing a player key")
    if key.isdigit():
        return f"VCMI_SCRIPTED_ADVENTURE_PLAYER_{int(key)}_SCRIPT"
    return f"VCMI_SCRIPTED_ADVENTURE_{safe_name(key).upper()}_SCRIPT"


def parse_player_script_override(raw: str) -> tuple[str, str]:
    if "=" not in raw:
        raise ValueError(f"Player script override must use PLAYER=SCRIPT syntax: {raw}")
    player, script = raw.split("=", 1)
    player = player.strip()
    script = script.strip()
    if not player or not script:
        raise ValueError(f"Player script override must use PLAYER=SCRIPT syntax: {raw}")
    return player, script


def player_script_overrides(args: argparse.Namespace, scenario: dict[str, Any]) -> dict[str, str]:
    overrides: dict[str, str] = {}
    for raw in getattr(args, "player_script", []) or []:
        player, script = parse_player_script_override(str(raw))
        overrides[player_script_env_name(player)] = script_override_value(script) or script

    for player, attr in (("red", "red_script"), ("blue", "blue_script")):
        if script := getattr(args, attr, None):
            overrides[player_script_env_name(player)] = script_override_value(script) or script

    for player, script in as_dict(scenario.get("player_scripts")).items():
        overrides[player_script_env_name(str(player))] = script_override_value(str(script)) or str(script)

    return overrides


def stdout_has_terminal_outcome(stdout_path: Path) -> bool:
    if not stdout_path.exists():
        return False
    text = stdout_path.read_text(encoding="utf-8", errors="replace")
    return any(marker in text for marker in TERMINAL_OUTCOME_MARKERS)


def clean_stdout_line(line: str) -> str:
    return ANSI_ESCAPE_RE.sub("", line.rstrip("\r\n"))


def stdout_tail_lines(stdout_path: Path, max_lines: int = 80) -> list[str]:
    if not stdout_path.exists():
        return []
    lines = stdout_path.read_text(encoding="utf-8", errors="replace").splitlines()
    return [clean_stdout_line(line) for line in lines[-max_lines:]]


def stdout_tail_signature(lines: list[str]) -> str:
    text = "\n".join(lines)
    if "Red player won. Ending game." in text:
        return "red_win"
    if "Red player lost. Ending game." in text:
        return "red_loss"
    if "Reached test day limit" in text:
        return "day_limit"
    if "Creating battle AI" in text:
        if "Invalid stack at tile" in text:
            return "battle_ai_creation_invalid_stack"
        return "battle_ai_creation"
    if "Player " in text and " starting turn" in text:
        return "turn_start"
    if "PERFORMANCE: NK2 updateState" in text:
        return "native_ai_state_update"
    return "unknown"


def stdout_summary(stdout_path: Path, last_output_at: float, now: float) -> dict[str, Any]:
    tail = stdout_tail_lines(stdout_path)
    return {
        "path": str(stdout_path),
        "bytes": stdout_path.stat().st_size if stdout_path.exists() else 0,
        "lastOutputAgeSeconds": round(max(0.0, now - last_output_at), 3),
        "tailSignature": stdout_tail_signature(tail),
        "tail": tail,
    }


def scenario_source(scenario: dict[str, Any]) -> tuple[str, str]:
    if "randomMap" in scenario:
        return "random", str(scenario.get("name") or "random-map")
    if scenario.get("save"):
        return "save", str(scenario["save"])
    return "map", str(scenario["map"])


def command_for_run(args: argparse.Namespace, scenario: dict[str, Any], run_dir: Path) -> list[str]:
    source_type, source = scenario_source(scenario)
    command = [args.client, "--headless"]
    if source_type == "random":
        random_map = as_dict(scenario.get("randomMap"))
        map_seed = scenario.get("seed", random_map.get("seed"))
        template = scenario.get("template", random_map.get("template"))
        command.append("--testrandommap")
        command.extend(["--randommap-size", str(random_map.get("size", "S"))])
        command.extend(["--randommap-levels", str(random_map.get("levels", 2))])
        command.extend(["--randommap-players", str(random_map.get("players", 2))])
        command.extend(["--randommap-teams", str(random_map.get("teams", 0))])
        command.extend(["--randommap-comp-only-players", str(random_map.get("compOnlyPlayers", 0))])
        command.extend(["--randommap-comp-only-teams", str(random_map.get("compOnlyTeams", 0))])
        command.extend(["--randommap-water", str(random_map.get("water", "none"))])
        command.extend(["--randommap-monsters", str(random_map.get("monsterStrength", "normal"))])
        if template is not None:
            command.extend(["--randommap-template", str(template)])
        if map_seed is not None:
            command.extend(["--randommap-seed", str(map_seed)])
    else:
        command.extend(["--testsave" if source_type == "save" else "--testmap", source])
    command.extend(["--logLocation", str(run_dir / "logs")])
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


def normalize_random_map(raw: dict[str, Any]) -> dict[str, Any]:
    random_map = dict(as_dict(raw.get("randomMap")))
    for raw_field, normalized_field in (
        ("size", "size"),
        ("levels", "levels"),
        ("players", "players"),
        ("teams", "teams"),
        ("compOnlyPlayers", "compOnlyPlayers"),
        ("compOnlyTeams", "compOnlyTeams"),
        ("water", "water"),
        ("monsterStrength", "monsterStrength"),
        ("template", "template"),
        ("seed", "seed"),
        ("mapSeed", "seed"),
    ):
        if raw_field in raw:
            random_map[normalized_field] = raw[raw_field]

    normalized = {
        "size": str(random_map.get("size", "S")),
        "levels": int(random_map.get("levels", 2)),
        "players": int(random_map.get("players", 2)),
        "teams": int(random_map.get("teams", 0)),
        "compOnlyPlayers": int(random_map.get("compOnlyPlayers", 0)),
        "compOnlyTeams": int(random_map.get("compOnlyTeams", 0)),
        "water": str(random_map.get("water", "none")),
        "monsterStrength": str(random_map.get("monsterStrength", "normal")),
    }
    if "template" in random_map:
        normalized["template"] = str(random_map["template"])
    if "seed" in random_map:
        normalized["seed"] = int(random_map["seed"])
    return normalized


def normalize_scenario(raw: dict[str, Any], index: int, args: argparse.Namespace) -> dict[str, Any]:
    if not raw.get("map") and not raw.get("save") and "randomMap" not in raw:
        raise ValueError(f"Scenario entry {index} is missing 'map', 'save', or 'randomMap'")

    source = str(raw.get("map") or raw.get("save") or raw.get("name") or f"random-map-{index}")
    scenario = {
        "name": str(raw.get("name") or safe_name(source)),
        "group": str(raw.get("group", DEFAULT_SCENARIO_GROUP)),
        "stage": str(raw.get("stage", DEFAULT_SCENARIO_STAGE)),
        "kind": str(raw.get("kind", DEFAULT_SCENARIO_KIND)),
        "runs": int(raw.get("runs", args.runs)),
        "testdays": int(raw.get("testdays", args.testdays)),
        "timeout": int(raw.get("timeout", args.timeout)),
        "idle_timeout": float(raw.get("idleTimeout", raw.get("idle_timeout", args.idle_timeout))),
        "infrastructure_retries": int(raw.get(
            "infrastructureRetries",
            raw.get("infrastructure_retries", raw.get("infraRetries", args.infrastructure_retries)),
        )),
        "extra_arg": list(args.extra_arg) + string_list(raw.get("extraArg")),
        "enabled": bool(raw.get("enabled", True)),
        "tags": string_list(raw.get("tags")),
    }
    player_scripts = raw.get("playerScripts", raw.get("player_scripts"))
    if isinstance(player_scripts, dict):
        scenario["player_scripts"] = {str(player): str(script) for player, script in player_scripts.items()}
    if raw.get("map"):
        scenario["map"] = str(raw["map"])
    if raw.get("save"):
        scenario["save"] = str(raw["save"])
    if "randomMap" in raw:
        scenario["randomMap"] = normalize_random_map(raw)
        if "seed" in scenario["randomMap"] and "seed" not in raw and "mapSeed" not in raw:
            scenario["seed"] = scenario["randomMap"]["seed"]
        if "template" in scenario["randomMap"] and "template" not in raw:
            scenario["template"] = scenario["randomMap"]["template"]
    for field in ("gameSeed", "seed", "template", "size", "levels", "water", "monsterStrength", "notes"):
        if field in raw:
            scenario[field] = raw[field]
    if "mapSeed" in raw:
        scenario["seed"] = raw["mapSeed"]
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
        if isinstance(raw, dict) and any(field in raw for field in ("map", "save", "randomMap")):
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
            "idle_timeout": args.idle_timeout,
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
        scenario["idle_timeout"] = max(0.0, float(scenario["idle_timeout"]))
        scenario["infrastructure_retries"] = max(0, int(scenario["infrastructure_retries"]))
    return scenarios


def args_for_scenario(args: argparse.Namespace, scenario: dict[str, Any]) -> argparse.Namespace:
    return argparse.Namespace(
        client=args.client,
        map=[scenario.get("map") or scenario.get("save")],
        ai=args.ai,
        runs=scenario["runs"],
        testdays=scenario["testdays"],
        timeout=scenario["timeout"],
        idle_timeout=scenario["idle_timeout"],
        infrastructure_retries=scenario["infrastructure_retries"],
        exit_grace_after_outcome=args.exit_grace_after_outcome,
        output=args.output / safe_name(str(scenario["name"])),
        cwd=args.cwd,
        clean=args.clean,
        extra_arg=scenario["extra_arg"],
        script=args.script,
        player_script=list(getattr(args, "player_script", []) or []),
        red_script=getattr(args, "red_script", None),
        blue_script=getattr(args, "blue_script", None),
        trace=args.trace,
        json=args.json,
    )


def run_outcome(stdout_path: Path, timed_out: bool, idle_timed_out: bool, return_code: int | None) -> dict[str, Any]:
    text = stdout_path.read_text(encoding="utf-8", errors="replace") if stdout_path.exists() else ""
    started_days = [int(match.group(1)) for match in TURN_START_DAY_RE.finditer(text)]
    outcome = {
        "result": "unknown",
        "completedDays": None,
        "lastStartedDay": max(started_days) if started_days else None,
    }
    if timed_out:
        outcome["result"] = "timeout"
    elif idle_timed_out:
        outcome["result"] = "idle_timeout"
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
    elif outcome["lastStartedDay"] is not None:
        outcome["completedDays"] = outcome["lastStartedDay"]
    return outcome


def infrastructure_failure_reason(result: dict[str, Any]) -> str | None:
    outcome = str(as_dict(result.get("outcome")).get("result", "unknown"))
    if outcome not in INFRASTRUCTURE_FAILURE_OUTCOMES:
        return None

    tail_signature = str(as_dict(result.get("stdoutSummary")).get("tailSignature", "unknown"))
    if tail_signature in INFRASTRUCTURE_FAILURE_TAIL_SIGNATURES:
        return tail_signature

    mistakes = as_dict(as_dict(result.get("traceSummary")).get("mistakes"))
    for item in as_list(mistakes.get("items")):
        item_data = as_dict(item)
        mistake_type = str(item_data.get("type", ""))
        if mistake_type in INFRASTRUCTURE_FAILURE_MISTAKES:
            details = as_dict(item_data.get("details"))
            action_type = str(details.get("actionType") or "unknown")
            return f"{mistake_type}:{action_type}"

    return None


def annotate_infrastructure_failure(result: dict[str, Any]) -> dict[str, Any]:
    reason = infrastructure_failure_reason(result)
    result["infrastructureFailure"] = reason is not None
    if reason:
        result["infrastructureFailureReason"] = reason
    return result


def persist_run_result(result: dict[str, Any]) -> None:
    (Path(str(result["runDir"])) / "run.json").write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")


def run_one(args: argparse.Namespace, scenario_or_map: dict[str, Any] | str, run_index: int, attempt: int = 1) -> dict[str, Any]:
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
            "idle_timeout": args.idle_timeout,
            "extra_arg": list(args.extra_arg),
            "enabled": True,
            "tags": [],
        }
    source_type, source = scenario_source(scenario)
    safe_map = safe_name(source)
    attempt_suffix = "" if attempt <= 1 else f"-retry-{attempt:02d}"
    run_dir = args.output / f"{safe_map}-run-{run_index:03d}{attempt_suffix}"
    if run_dir.exists() and args.clean:
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    command = command_for_run(args, scenario, run_dir)
    env = os.environ.copy()
    env["XDG_CACHE_HOME"] = str(run_dir / "cache")
    if script := script_override_value(args.script):
        env["VCMI_SCRIPTED_ADVENTURE_SCRIPT"] = script
    player_scripts = player_script_overrides(args, scenario)
    env.update(player_scripts)
    if args.trace:
        env["VCMI_SCRIPTED_ADVENTURE_TRACE"] = "1"
    started = time.monotonic()
    timed_out = False
    idle_timed_out = False
    terminated_after_outcome = False
    return_code: int | None
    stdout_path = run_dir / "stdout.log"

    with stdout_path.open("w", encoding="utf-8") as stdout:
        stdout.write("$ " + " ".join(command) + "\n")
        stdout.flush()
        process = subprocess.Popen(command, cwd=args.cwd, env=env, stdout=stdout, stderr=subprocess.STDOUT)
        outcome_seen_at: float | None = None
        last_output_at = started
        last_stdout_size = stdout_path.stat().st_size
        while True:
            return_code = process.poll()
            if return_code is not None:
                break

            now = time.monotonic()
            current_stdout_size = stdout_path.stat().st_size
            if current_stdout_size != last_stdout_size:
                last_stdout_size = current_stdout_size
                last_output_at = now
            if outcome_seen_at is None and stdout_has_terminal_outcome(stdout_path):
                outcome_seen_at = now
            if outcome_seen_at is not None and now - outcome_seen_at >= args.exit_grace_after_outcome:
                terminated_after_outcome = True
                process.terminate()
                try:
                    return_code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait()
                break
            if args.idle_timeout and now - last_output_at >= args.idle_timeout:
                idle_timed_out = True
                process.terminate()
                try:
                    return_code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait()
                break
            if now - started >= args.timeout:
                timed_out = True
                process.kill()
                process.wait()
                return_code = None
                break
            time.sleep(0.5)

    trace_dir = trace_dir_for_run(run_dir)
    trace_summary = summarize(iter_trace_files([str(trace_dir)])) if trace_dir.exists() else summarize([])
    outcome = run_outcome(stdout_path, timed_out, idle_timed_out, return_code)
    max_day = int(trace_summary.get("quality", {}).get("maxDay") or 0)
    if outcome["completedDays"] is None and max_day > 0:
        outcome["completedDays"] = max_day
    finished_at = time.monotonic()
    stdout_info = stdout_summary(stdout_path, last_output_at, finished_at)
    result = {
        "scenario": scenario.get("name"),
        "group": scenario.get("group"),
        "stage": scenario.get("stage"),
        "kind": scenario.get("kind"),
        "sourceType": source_type,
        "source": source,
        "map": scenario.get("map"),
        "save": scenario.get("save"),
        "randomMap": scenario.get("randomMap"),
        "seed": scenario.get("seed"),
        "gameSeed": scenario.get("gameSeed"),
        "template": scenario.get("template"),
        "tags": scenario.get("tags", []),
        "run": run_index,
        "attempt": attempt,
        "runDir": str(run_dir),
        "command": command,
        "script": script_override_value(args.script),
        "playerScripts": player_scripts,
        "timeoutSeconds": args.timeout,
        "idleTimeoutSeconds": args.idle_timeout,
        "timedOut": timed_out,
        "idleTimedOut": idle_timed_out,
        "terminatedAfterOutcome": terminated_after_outcome,
        "returnCode": return_code,
        "elapsedSeconds": round(finished_at - started, 3),
        "stdoutSummary": stdout_info,
        "ai": list(args.ai),
        "script": script_override_value(args.script),
        "trace": bool(args.trace),
        "traceDir": str(trace_dir),
        "traceSummary": trace_summary,
        "outcome": outcome,
    }
    annotate_infrastructure_failure(result)
    persist_run_result(result)
    return result


def run_one_with_infrastructure_retries(args: argparse.Namespace, scenario: dict[str, Any], run_index: int) -> dict[str, Any]:
    max_attempts = max(1, int(getattr(args, "infrastructure_retries", 0)) + 1)
    previous_attempts: list[dict[str, Any]] = []

    for attempt in range(1, max_attempts + 1):
        result = run_one(args, scenario, run_index, attempt)
        result["attemptsAllowed"] = max_attempts
        result["infrastructureRetriesUsed"] = len(previous_attempts)

        if not result.get("infrastructureFailure") or attempt == max_attempts:
            if previous_attempts:
                result["previousAttempts"] = previous_attempts
                persist_run_result(result)
            return result

        previous_attempts.append(compact_result(result))

    raise RuntimeError("Infrastructure retry loop ended without a result")


def result_winner(result: dict[str, Any]) -> str | None:
    ai_names = [str(item) for item in as_list(result.get("ai"))]
    outcome = as_dict(result.get("outcome"))
    outcome_result = outcome.get("result")
    if outcome_result == "red_win":
        return ai_names[0] if ai_names else "red"
    if outcome_result == "red_loss":
        return ai_names[1] if len(ai_names) > 1 else "red_opponent"
    return None


def compact_result(result: dict[str, Any]) -> dict[str, Any]:
    outcome = as_dict(result.get("outcome"))
    winner = result_winner(result)
    return {
        "scenario": result.get("scenario"),
        "run": result.get("run"),
        "winner": winner,
        "outcome": outcome.get("result"),
        "completedDays": outcome.get("completedDays"),
        "timedOut": result.get("timedOut"),
        "idleTimedOut": result.get("idleTimedOut"),
        "terminatedAfterOutcome": result.get("terminatedAfterOutcome"),
        "returnCode": result.get("returnCode"),
        "elapsedSeconds": result.get("elapsedSeconds"),
        "attempt": result.get("attempt", 1),
        "infrastructureFailure": result.get("infrastructureFailure", False),
        "infrastructureFailureReason": result.get("infrastructureFailureReason"),
        "infrastructureRetriesUsed": result.get("infrastructureRetriesUsed", 0),
        "stdoutTailSignature": as_dict(result.get("stdoutSummary")).get("tailSignature"),
        "sourceType": result.get("sourceType"),
        "source": result.get("source"),
        "randomMap": result.get("randomMap"),
        "mapSeed": result.get("seed"),
        "gameSeed": result.get("gameSeed"),
        "template": result.get("template"),
        "ai": result.get("ai"),
        "traceDir": result.get("traceDir"),
        "runDir": result.get("runDir"),
        "runJson": str(Path(str(result.get("runDir"))) / "run.json") if result.get("runDir") else None,
        "traceFilesParsed": as_dict(result.get("traceSummary")).get("parsed", 0),
        "importantMistakes": as_dict(as_dict(result.get("traceSummary")).get("mistakes")).get("important", 0),
    }


def summarize_results(results: list[dict[str, Any]]) -> dict[str, Any]:
    winners: dict[str, int] = {}
    outcomes: dict[str, int] = {}
    infrastructure_retried_attempts = 0
    terminal_runs = 0
    for result in results:
        winner = result_winner(result) or "none"
        winners[winner] = winners.get(winner, 0) + 1
        outcome = str(as_dict(result.get("outcome")).get("result", "unknown"))
        outcomes[outcome] = outcomes.get(outcome, 0) + 1
        if outcome in TERMINAL_OUTCOMES:
            terminal_runs += 1
        infrastructure_retried_attempts += len(as_list(result.get("previousAttempts")))

    completed_days = [
        int(as_dict(result.get("outcome")).get("completedDays"))
        for result in results
        if isinstance(as_dict(result.get("outcome")).get("completedDays"), int)
    ]
    return {
        "runs": len(results),
        "terminalRuns": terminal_runs,
        "nonTerminalRuns": len(results) - terminal_runs,
        "scriptedAdventureAIWins": winners.get("ScriptedAdventureAI", 0),
        "nullkiller2Wins": winners.get("Nullkiller2", 0),
        "redWins": outcomes.get("red_win", 0),
        "redLosses": outcomes.get("red_loss", 0),
        "winners": winners,
        "outcomes": outcomes,
        "timeouts": sum(1 for result in results if result.get("timedOut")),
        "idleTimeouts": sum(1 for result in results if result.get("idleTimedOut")),
        "infrastructureFailures": sum(1 for result in results if result.get("infrastructureFailure")),
        "infrastructureRetriedAttempts": infrastructure_retried_attempts,
        "terminatedAfterOutcome": sum(1 for result in results if result.get("terminatedAfterOutcome")),
        "nonzeroExit": sum(
            1
            for result in results
            if not result.get("timedOut") and not result.get("idleTimedOut") and result.get("returnCode") != 0
        ),
        "completedDayMin": min(completed_days) if completed_days else None,
        "completedDayMax": max(completed_days) if completed_days else None,
        "completedDayAverage": round(sum(completed_days) / len(completed_days), 2) if completed_days else None,
        "results": [compact_result(result) for result in sorted(results, key=lambda item: (str(item.get("scenario")), int(item.get("run", 0))))],
    }


def print_run_status(scenario: dict[str, Any], result: dict[str, Any]) -> None:
    status = "timeout" if result["timedOut"] else "idle-timeout" if result.get("idleTimedOut") else f"exit {result['returnCode']}"
    if result.get("infrastructureFailure"):
        status += f", infrastructure={result.get('infrastructureFailureReason')}"
    if result.get("infrastructureRetriesUsed"):
        status += f", retries={result['infrastructureRetriesUsed']}"
    parsed = result["traceSummary"]["parsed"]
    winner = result_winner(result) or "<none>"
    days = as_dict(result.get("outcome")).get("completedDays")
    print(
        f"{scenario['name']} {scenario_source(scenario)[1]} run {result['run']}: "
        f"{status}, outcome={result['outcome']['result']}, winner={winner}, days={days}, "
        f"traces parsed={parsed}, dir={result['runDir']}"
    )


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
    parser.add_argument("--idle-timeout", type=float, default=0.0, help="Seconds without stdout progress before stopping one run. Disabled at 0.")
    parser.add_argument("--infrastructure-retries", type=int, default=0, help="Retry runs that end in known infrastructure signatures such as battle AI creation stalls.")
    parser.add_argument("--exit-grace-after-outcome", type=float, default=10.0, help="Seconds to wait for clean client exit after a terminal game outcome appears in stdout.")
    parser.add_argument("--output", type=Path, default=Path("scripted-ai-runs"), help="Directory for run outputs.")
    parser.add_argument("--cwd", default=None, help="Working directory for vcmiclient.")
    parser.add_argument("--clean", action="store_true", help="Delete existing run directories before reuse.")
    parser.add_argument("--extra-arg", action="append", default=[], help="Extra argument passed to vcmiclient.")
    parser.add_argument("--script", default=None, help="Script resource path or local Lua file used by ScriptedAdventureAI.")
    parser.add_argument("--player-script", action="append", default=[], help="Per-player script override as PLAYER=SCRIPT, e.g. red=ai/a.lua or 1=file:/tmp/blue.lua.")
    parser.add_argument("--red-script", default=None, help="Shortcut for --player-script red=SCRIPT.")
    parser.add_argument("--blue-script", default=None, help="Shortcut for --player-script blue=SCRIPT.")
    parser.add_argument("--trace", action="store_true", help="Enable ScriptedAdventureAI trace files for each run.")
    parser.add_argument("--jobs", type=int, default=1, help="Number of runs to execute in parallel.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable batch manifest.")
    args = parser.parse_args()
    if args.ai is None:
        args.ai = ["ScriptedAdventureAI"]
    else:
        args.ai = normalize_ai_names(args.ai)

    args.output.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, Any]] = []
    scenarios = load_scenarios(args)
    jobs = max(1, int(args.jobs))
    tasks: list[tuple[argparse.Namespace, dict[str, Any], int]] = []
    for scenario in scenarios:
        scenario_args = args_for_scenario(args, scenario)
        for run_index in range(1, scenario["runs"] + 1):
            tasks.append((scenario_args, scenario, run_index))

    if jobs == 1:
        for scenario_args, scenario, run_index in tasks:
            result = run_one_with_infrastructure_retries(scenario_args, scenario, run_index)
            results.append(result)
            if not args.json:
                print_run_status(scenario, result)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_to_scenario = {
                executor.submit(run_one_with_infrastructure_retries, scenario_args, scenario, run_index): scenario
                for scenario_args, scenario, run_index in tasks
            }
            for future in concurrent.futures.as_completed(future_to_scenario):
                scenario = future_to_scenario[future]
                result = future.result()
                results.append(result)
                if not args.json:
                    print_run_status(scenario, result)

    manifest = {"scenarios": scenarios, "runs": results}
    summary = summarize_results(results)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    (args.output / "results.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    if args.json:
        print(json.dumps({"manifest": manifest, "summary": summary}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
