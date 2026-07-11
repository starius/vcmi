#!/usr/bin/env python3
"""Validate generated battle-prediction datasets before using them as evidence."""

from __future__ import annotations

import argparse
import glob
import json
import os
import tarfile
from collections import Counter
from dataclasses import dataclass, field
from typing import Any, Iterable

from evaluate_nullkiller_predictor import battle_type, iter_json_lines, setup_key


MMAI_FALLBACK_PATTERNS = [
    "Could not load MMAI config",
    "falling back to BattleAI",
    "no path configured",
    "MMAI: load error",
]
MMAI_INIT_PATTERN = "MMAI version 13 initialized"


@dataclass
class LogScan:
    files: int = 0
    files_with_mmai_init: int = 0
    fallback_lines: list[str] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--expected-rows", type=int)
    parser.add_argument("--expected-schema", type=int)
    parser.add_argument("--expected-shards", type=int)
    parser.add_argument("--expected-shard-size", type=int)
    parser.add_argument("--min-setup-groups", type=int)
    parser.add_argument("--require-complete-shards", action="store_true")
    parser.add_argument("--require-battle-types", help="Comma-separated battle types that must be present")
    parser.add_argument("--require-no-mmai-fallback", action="store_true")
    parser.add_argument("--require-mmai-initialized", action="store_true")
    parser.add_argument("--print-fallback-lines", type=int, default=20)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def parse_required_types(value: str | None) -> set[str]:
    if not value:
        return set()
    return {part.strip() for part in value.split(",") if part.strip()}


def iter_logs_from_directory(path: str) -> Iterable[tuple[str, Iterable[str]]]:
    for file_name in sorted(glob.glob(os.path.join(path, "shard-*.log"))):
        with open(file_name, encoding="utf-8", errors="replace") as handle:
            yield file_name, handle


def iter_logs_from_archive(path: str) -> Iterable[tuple[str, Iterable[str]]]:
    with tarfile.open(path, "r:gz") as archive:
        members = sorted(
            (member for member in archive.getmembers() if os.path.basename(member.name).startswith("shard-") and member.name.endswith(".log")),
            key=lambda member: member.name,
        )
        for member in members:
            extracted = archive.extractfile(member)
            if extracted is None:
                continue
            yield member.name, (raw.decode("utf-8", errors="replace") for raw in extracted)


def iter_logs(path: str) -> Iterable[tuple[str, Iterable[str]]]:
    if os.path.isdir(path):
        yield from iter_logs_from_directory(path)
    elif path.endswith(".tar.gz") or path.endswith(".tgz"):
        yield from iter_logs_from_archive(path)


def scan_logs(path: str) -> LogScan:
    result = LogScan()
    for file_name, lines in iter_logs(path):
        result.files += 1
        has_mmai_init = False
        for number, line in enumerate(lines, start=1):
            if MMAI_INIT_PATTERN in line:
                has_mmai_init = True
            if any(pattern in line for pattern in MMAI_FALLBACK_PATTERNS):
                result.fallback_lines.append(f"{file_name}:{number}:{line.rstrip()}")
        result.files_with_mmai_init += int(has_mmai_init)
    return result


def load_summary(path: str) -> dict[str, Any]:
    schemas: Counter[int] = Counter()
    battle_types: Counter[str] = Counter()
    shard_rows: Counter[int] = Counter()
    setup_groups: Counter[str] = Counter()
    rows = 0

    for row in iter_json_lines(path):
        rows += 1
        schemas[int(row.get("schema", 1))] += 1
        battle_types[battle_type(row)] += 1
        if row.get("shardIndex") is not None:
            shard_rows[int(row["shardIndex"])] += 1
        setup_groups[setup_key(row)] += 1

    return {
        "rows": rows,
        "schemas": dict(sorted(schemas.items())),
        "battle_types": dict(sorted(battle_types.items())),
        "shards": len(shard_rows),
        "shard_rows_min": min(shard_rows.values()) if shard_rows else 0,
        "shard_rows_max": max(shard_rows.values()) if shard_rows else 0,
        "shard_rows": dict(sorted(shard_rows.items())),
        "setup_groups": len(setup_groups),
        "setup_group_rows_min": min(setup_groups.values()) if setup_groups else 0,
        "setup_group_rows_max": max(setup_groups.values()) if setup_groups else 0,
    }


def validate(args: argparse.Namespace, summary: dict[str, Any], logs: LogScan) -> list[str]:
    errors = []
    required_types = parse_required_types(args.require_battle_types)

    if args.expected_rows is not None and summary["rows"] != args.expected_rows:
        errors.append(f"expected {args.expected_rows} rows, got {summary['rows']}")

    if args.expected_schema is not None and summary["schemas"] != {args.expected_schema: summary["rows"]}:
        errors.append(f"expected only schema {args.expected_schema}, got {summary['schemas']}")

    if args.expected_shards is not None and summary["shards"] != args.expected_shards:
        errors.append(f"expected {args.expected_shards} shards, got {summary['shards']}")

    if args.expected_shard_size is not None:
        if summary["shard_rows_min"] != args.expected_shard_size or summary["shard_rows_max"] != args.expected_shard_size:
            errors.append(
                f"expected shard size {args.expected_shard_size}, got min={summary['shard_rows_min']} max={summary['shard_rows_max']}"
            )

    if args.require_complete_shards and args.expected_shard_size is None:
        errors.append("--require-complete-shards needs --expected-shard-size")

    if args.min_setup_groups is not None and summary["setup_groups"] < args.min_setup_groups:
        errors.append(f"expected at least {args.min_setup_groups} setup groups, got {summary['setup_groups']}")

    missing_types = required_types - set(summary["battle_types"])
    if missing_types:
        errors.append(f"missing battle types: {sorted(missing_types)}")

    if args.require_no_mmai_fallback and logs.fallback_lines:
        errors.append(f"found {len(logs.fallback_lines)} MMAI fallback/config-error log lines")

    if args.require_mmai_initialized:
        if logs.files == 0:
            errors.append("no shard logs found for MMAI initialization check")
        elif logs.files_with_mmai_init != logs.files:
            errors.append(f"MMAI initialized in {logs.files_with_mmai_init}/{logs.files} shard logs")

    return errors


def main() -> int:
    args = parse_args()
    summary = load_summary(args.dataset)
    logs = scan_logs(args.dataset)
    errors = validate(args, summary, logs)

    output = {
        **summary,
        "log_files": logs.files,
        "log_files_with_mmai_init": logs.files_with_mmai_init,
        "mmai_fallback_lines": len(logs.fallback_lines),
        "ok": not errors,
        "errors": errors,
    }

    if args.json:
        print(json.dumps(output, sort_keys=True))
    else:
        print(
            f"rows={summary['rows']} schemas={summary['schemas']} "
            f"battle_types={summary['battle_types']} shards={summary['shards']} "
            f"shard_rows={summary['shard_rows_min']}..{summary['shard_rows_max']} "
            f"setup_groups={summary['setup_groups']} logs={logs.files} "
            f"mmai_init_logs={logs.files_with_mmai_init} mmai_fallback_lines={len(logs.fallback_lines)}"
        )
        if logs.fallback_lines and args.print_fallback_lines > 0:
            print("fallback/config-error lines:")
            for line in logs.fallback_lines[: args.print_fallback_lines]:
                print(f"  {line}")
        if errors:
            print("FAIL:")
            for error in errors:
                print(f"  {error}")
        else:
            print("OK")

    return 0 if not errors else 2


if __name__ == "__main__":
    raise SystemExit(main())
