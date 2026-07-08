#!/usr/bin/env python3
"""Run headless ScriptedAdventureAI matches and summarize traces."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from summarizeAdventureTrace import iter_trace_files, summarize  # noqa: E402


def command_for_run(args: argparse.Namespace, game_map: str, run_dir: Path) -> list[str]:
    command = [
        args.client,
        "--headless",
        "--testmap",
        game_map,
        "--logLocation",
        str(run_dir / "logs"),
    ]
    if args.testdays:
        command.extend(["--testdays", str(args.testdays)])
    for ai_name in args.ai:
        command.extend(["--ai", ai_name])
    command.extend(args.extra_arg)
    return command


def trace_dir_for_run(run_dir: Path) -> Path:
    return run_dir / "cache" / "vcmi" / "scriptedAdventureAI"


def run_one(args: argparse.Namespace, game_map: str, run_index: int) -> dict[str, Any]:
    safe_map = "".join(ch if ch.isalnum() else "_" for ch in game_map).strip("_") or "map"
    run_dir = args.output / f"{safe_map}-run-{run_index:03d}"
    if run_dir.exists() and args.clean:
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    command = command_for_run(args, game_map, run_dir)
    env = os.environ.copy()
    env["XDG_CACHE_HOME"] = str(run_dir / "cache")
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
        "map": game_map,
        "run": run_index,
        "runDir": str(run_dir),
        "command": command,
        "timeoutSeconds": args.timeout,
        "timedOut": timed_out,
        "returnCode": return_code,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "traceDir": str(trace_dir),
        "traceSummary": trace_summary,
    }
    (run_dir / "run.json").write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Run headless ScriptedAdventureAI matches and summarize traces.")
    parser.add_argument("--client", required=True, help="Path to vcmiclient.")
    parser.add_argument("--map", action="append", required=True, help="VCMI map resource path. Can be repeated.")
    parser.add_argument("--ai", action="append", default=None, help="AI names for consecutive players.")
    parser.add_argument("--runs", type=int, default=1, help="Runs per map.")
    parser.add_argument("--testdays", type=int, default=0, help="Completed adventure days before the client exits.")
    parser.add_argument("--timeout", type=int, default=300, help="Seconds before stopping one run.")
    parser.add_argument("--output", type=Path, default=Path("scripted-ai-runs"), help="Directory for run outputs.")
    parser.add_argument("--cwd", default=None, help="Working directory for vcmiclient.")
    parser.add_argument("--clean", action="store_true", help="Delete existing run directories before reuse.")
    parser.add_argument("--extra-arg", action="append", default=[], help="Extra argument passed to vcmiclient.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable batch manifest.")
    args = parser.parse_args()
    if args.ai is None:
        args.ai = ["ScriptedAdventureAI"]

    args.output.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, Any]] = []
    for game_map in args.map:
        for run_index in range(1, max(1, args.runs) + 1):
            result = run_one(args, game_map, run_index)
            results.append(result)
            if not args.json:
                status = "timeout" if result["timedOut"] else f"exit {result['returnCode']}"
                parsed = result["traceSummary"]["parsed"]
                print(f"{game_map} run {run_index}: {status}, traces parsed={parsed}, dir={result['runDir']}")

    manifest = {"runs": results}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    if args.json:
        print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
