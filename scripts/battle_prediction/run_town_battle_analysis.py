#!/usr/bin/env python3
"""Run the standard town/siege battle-predictor diagnostics for one dataset."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class CommandRun:
    name: str
    command: list[str]
    stdout_path: Path
    stderr_path: Path
    returncode: int | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--output-dir", help="Directory for analysis artifacts. Defaults to DATASET/v3-analysis for directory datasets.")
    parser.add_argument("--scope", choices=["all", "deployed-static", "town"], default="town")
    parser.add_argument("--group-key", choices=["setup", "shard"], default="shard")
    parser.add_argument("--complete-shards-only", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--expected-rows", type=int)
    parser.add_argument("--expected-schema", type=int)
    parser.add_argument("--expected-shards", type=int)
    parser.add_argument("--expected-shard-size", type=int)
    parser.add_argument("--expected-groups", type=int)
    parser.add_argument("--battle-types", default="town-hero")
    parser.add_argument("--require-schema3-rich-fields", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--require-mmai-initialized", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--require-no-mmai-fallback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--schema6-structural-gates", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--require-schema6-wall-mutation", action="store_true")
    parser.add_argument(
        "--schema6-min-counter",
        action="append",
        default=[],
        metavar="NAME=COUNT",
        help="Extra schema6 battle-start counter gate to pass to validate_battle_dataset.py.",
    )
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("--segments", type=int, default=40)
    parser.add_argument("--l2", type=float, default=0.03)
    parser.add_argument("--sample-counts", default="8,15,20")
    parser.add_argument("--fallback-sample-count", type=int, default=20)
    parser.add_argument("--min-fallback-groups", type=int, default=60)
    parser.add_argument("--min-fallback-accuracy50", type=float, default=0.95)
    parser.add_argument("--min-fallback-safety-accuracy", type=float, default=0.95)
    parser.add_argument("--max-fallback-false-safe-groups", type=int, default=0)
    parser.add_argument("--max-fallback-false-unsafe-groups", type=int, default=1)
    parser.add_argument("--town-deployable-safe-probability", type=float, default=0.62)
    parser.add_argument("--town-danger-factors", default="1.0,1.25,1.5,1.75,2.0")
    parser.add_argument("--guard-results", type=int, default=40)
    parser.add_argument("--dry-run", action="store_true", help="Print and record commands without executing them.")
    return parser.parse_args()


def default_output_dir(dataset: str) -> Path:
    path = Path(dataset)
    if path.is_dir() or not path.suffix:
        return path / "v3-analysis"
    return path.parent / f"{path.name}-v3-analysis"


def script_path(name: str) -> str:
    return str(Path(__file__).resolve().parent / name)


def add_optional_int(command: list[str], flag: str, value: int | None) -> None:
    if value is not None:
        command.extend([flag, str(value)])


def schema6_counter_gates(args: argparse.Namespace) -> list[str]:
    gates = list(args.schema6_min_counter)
    if args.expected_schema is None or args.expected_schema < 6:
        return gates

    if args.schema6_structural_gates:
        if args.expected_rows is None:
            raise SystemExit("--schema6-structural-gates needs --expected-rows")
        expected_rows = str(args.expected_rows)
        for name in (
            "rows",
            "stacks_array",
            "nonempty_stacks",
            "attacker_stack_rows",
            "defender_stack_rows",
            "obstacles_array",
            "wall_state_rows",
        ):
            gates.append(f"{name}={expected_rows}")
        gates.extend(["turret_rows=1", "nonempty_obstacles=1"])

    if args.require_schema6_wall_mutation:
        gates.extend(["wall_changed_rows=1", "gate_changed_rows=1"])

    return gates


def validation_command(args: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        script_path("validate_battle_dataset.py"),
        args.dataset,
        "--group-key",
        args.group_key,
        "--require-battle-types",
        args.battle_types,
        "--json",
    ]
    add_optional_int(command, "--expected-rows", args.expected_rows)
    add_optional_int(command, "--expected-schema", args.expected_schema)
    add_optional_int(command, "--expected-shards", args.expected_shards)
    add_optional_int(command, "--expected-shard-size", args.expected_shard_size)
    add_optional_int(command, "--expected-groups", args.expected_groups)
    if args.complete_shards_only:
        if args.expected_shard_size is None:
            raise SystemExit("--complete-shards-only validation needs --expected-shard-size")
        command.append("--require-complete-shards")
    if args.require_no_mmai_fallback:
        command.append("--require-no-mmai-fallback")
    if args.require_mmai_initialized:
        command.append("--require-mmai-initialized")
    if args.require_schema3_rich_fields:
        command.append("--require-schema3-rich-fields")
    for gate in schema6_counter_gates(args):
        command.extend(["--min-schema6-battle-start-counter", gate])
    return command


def common_analysis_args(args: argparse.Namespace) -> list[str]:
    command = [
        args.dataset,
        "--scope",
        args.scope,
        "--group-key",
        args.group_key,
    ]
    if args.complete_shards_only:
        command.append("--complete-shards-only")
    return command


def predictor_command(args: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        script_path("evaluate_nullkiller_predictor.py"),
        *common_analysis_args(args),
        "--l2",
        str(args.l2),
        "--print-near-even",
        str(args.limit),
        "--print-worst",
        str(args.limit),
        "--print-v3-false-safe",
        str(args.limit),
        "--print-v3-false-unsafe",
        str(args.limit),
        "--print-town-deployable-false-safe",
        str(args.limit),
        "--print-town-deployable-false-unsafe",
        str(args.limit),
        "--town-deployable-safe-probability",
        str(args.town_deployable_safe_probability),
        "--json",
    ]
    return command


def static_miss_command(args: argparse.Namespace) -> list[str]:
    return [
        sys.executable,
        script_path("report_v3_static_misses.py"),
        *common_analysis_args(args),
        "--limit",
        str(args.limit),
        "--segments",
        str(args.segments),
        "--simulation-sample-counts",
        args.sample_counts,
        "--town-danger-factors",
        args.town_danger_factors,
        "--probe-town-guards",
        "--guard-results",
        str(args.guard_results),
        "--json",
    ]


def fallback_command(args: argparse.Namespace, json_output: Path) -> list[str]:
    return [
        sys.executable,
        script_path("evaluate_simulation_fallback.py"),
        *common_analysis_args(args),
        "--min-group-size",
        str(args.expected_shard_size or 1),
        "--sample-counts",
        args.sample_counts,
        "--static-model",
        "cxx-v3",
        "--safe-policy",
        "all-wins",
        "--json-output",
        str(json_output),
        "--require-fallback-sample-count",
        str(args.fallback_sample_count),
        "--min-fallback-groups",
        str(args.min_fallback_groups),
        "--min-fallback-accuracy50",
        str(args.min_fallback_accuracy50),
        "--min-fallback-safety-accuracy",
        str(args.min_fallback_safety_accuracy),
        "--max-fallback-false-safe-groups",
        str(args.max_fallback_false_safe_groups),
        "--max-fallback-false-unsafe-groups",
        str(args.max_fallback_false_unsafe_groups),
        "--print-worst",
        str(args.limit),
        "--print-false-safe",
        str(args.limit),
        "--print-false-unsafe",
        str(args.limit),
    ]


def segment_command(args: argparse.Namespace, mode: str) -> list[str]:
    command = [
        sys.executable,
        script_path("analyze_v3_failure_segments.py"),
        *common_analysis_args(args),
        "--predictor",
        "cxx-v3",
        "--top",
        str(args.segments),
        "--print-groups",
        str(args.limit),
    ]
    if mode == "close_even":
        command.extend(["--actual-min", "0.25", "--actual-max", "0.75", "--error-min", "0.25"])
    elif mode == "false_safe":
        command.extend(["--prediction-min", "0.60", "--actual-max", "0.949999", "--sort", "false-safe"])
    elif mode == "false_unsafe":
        command.extend(["--prediction-max", "0.599999", "--actual-min", "0.95", "--sort", "false-unsafe"])
    else:
        raise ValueError(f"Unknown segment mode: {mode}")
    return command


def build_runs(args: argparse.Namespace, output_dir: Path) -> list[CommandRun]:
    return [
        CommandRun("validation", validation_command(args), output_dir / "validation.json", output_dir / "validation.stderr.txt"),
        CommandRun("predictor", predictor_command(args), output_dir / "nullkiller-predictor.json", output_dir / "nullkiller-predictor.stderr.txt"),
        CommandRun("static-misses", static_miss_command(args), output_dir / "v3-static-misses.json", output_dir / "v3-static-misses.stderr.txt"),
        CommandRun("close-even-segments", segment_command(args, "close_even"), output_dir / "close-even-segments.txt", output_dir / "close-even-segments.stderr.txt"),
        CommandRun("false-safe-segments", segment_command(args, "false_safe"), output_dir / "false-safe-segments.txt", output_dir / "false-safe-segments.stderr.txt"),
        CommandRun("false-unsafe-segments", segment_command(args, "false_unsafe"), output_dir / "false-unsafe-segments.txt", output_dir / "false-unsafe-segments.stderr.txt"),
        CommandRun("fallback-proof", fallback_command(args, output_dir / "fallback-proof.json"), output_dir / "fallback-proof.txt", output_dir / "fallback-proof.stderr.txt"),
    ]


def write_command_file(output_dir: Path, runs: list[CommandRun]) -> None:
    lines = []
    for run in runs:
        lines.append(f"# {run.name}")
        lines.append(" ".join(shlex.quote(part) for part in run.command))
        lines.append("")
    (output_dir / "commands.sh").write_text("\n".join(lines), encoding="utf-8")


def execute_run(run: CommandRun, dry_run: bool) -> None:
    print(f"{run.name}: {' '.join(shlex.quote(part) for part in run.command)}")
    if dry_run:
        run.returncode = None
        return

    completed = subprocess.run(run.command, text=True, capture_output=True, check=False)
    run.stdout_path.write_text(completed.stdout, encoding="utf-8")
    run.stderr_path.write_text(completed.stderr, encoding="utf-8")
    run.returncode = completed.returncode
    if completed.returncode != 0:
        print(f"{run.name} failed with exit code {completed.returncode}", file=sys.stderr)


def summary_to_dict(args: argparse.Namespace, output_dir: Path, runs: list[CommandRun]) -> dict[str, Any]:
    return {
        "dataset": args.dataset,
        "outputDir": str(output_dir),
        "scope": args.scope,
        "groupKey": args.group_key,
        "completeShardsOnly": args.complete_shards_only,
        "expectedRows": args.expected_rows,
        "expectedSchema": args.expected_schema,
        "expectedShards": args.expected_shards,
        "expectedShardSize": args.expected_shard_size,
        "expectedGroups": args.expected_groups,
        "dryRun": args.dry_run,
        "ok": all(run.returncode in (0, None) for run in runs),
        "runs": [
            {
                "name": run.name,
                "returnCode": run.returncode,
                "stdout": str(run.stdout_path),
                "stderr": str(run.stderr_path),
                "command": run.command,
            }
            for run in runs
        ],
    }


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir) if args.output_dir else default_output_dir(args.dataset)
    output_dir.mkdir(parents=True, exist_ok=True)

    runs = build_runs(args, output_dir)
    write_command_file(output_dir, runs)
    for run in runs:
        execute_run(run, args.dry_run)

    summary = summary_to_dict(args, output_dir, runs)
    (output_dir / "analysis-summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if summary["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
