#!/usr/bin/env python3
"""Compare normalized native Nullkiller2 decision traces against Lua replay."""

from __future__ import annotations

import argparse
import difflib
import json
import sys
from pathlib import Path
from typing import Any

from replay_lua_decisions import (
    DISCREPANCY_ROOT,
    REPO_ROOT,
    display_path,
    find_lua_interpreter,
    run_decision,
    assert_subset,
    expected_failure_reason,
    has_expected_failure_marker,
)


LUA_ROOT = REPO_ROOT / "scripts/ai/nullkiller2"
TRACE_ROOT = LUA_ROOT / "tests/fixtures/native_trace"
DEFAULT_COMPARE_FIELDS = ["status", "intent", "selection", "role", "side", "ended", "commandJournal"]
MISSING = object()


def trace_paths(explicit: list[Path]) -> list[Path]:
    if explicit:
        return [path if path.is_absolute() else REPO_ROOT / path for path in explicit]
    return sorted(TRACE_ROOT.glob("*.json"))


def trace_decisions(document: Any) -> list[dict[str, Any]]:
    if isinstance(document, list):
        decisions = document
    elif isinstance(document, dict) and isinstance(document.get("decisions"), list):
        decisions = document["decisions"]
    elif isinstance(document, dict) and "function" in document:
        decisions = [document]
    else:
        raise ValueError("native trace must be a decision object, a decision array, or an object with decisions")

    for index, decision in enumerate(decisions):
        if not isinstance(decision, dict):
            raise ValueError(f"decision {index} is not an object")
    return decisions


def lookup(value: Any, path: str) -> Any:
    current = value
    for part in path.split("."):
        if isinstance(current, dict):
            if part not in current:
                return MISSING
            current = current[part]
            continue
        if isinstance(current, list) and part.isdigit():
            index = int(part)
            if index >= len(current):
                return MISSING
            current = current[index]
            continue
        return MISSING
    return current


def assert_equal(expected: Any, actual: Any, path: str = "$") -> list[str]:
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{path}: expected object, got {type(actual).__name__}"]
        errors: list[str] = []
        expected_keys = set(expected)
        actual_keys = set(actual)
        for key in sorted(expected_keys - actual_keys):
            errors.append(f"{path}.{key}: missing")
        for key in sorted(actual_keys - expected_keys):
            errors.append(f"{path}.{key}: unexpected")
        for key in sorted(expected_keys & actual_keys):
            errors.extend(assert_equal(expected[key], actual[key], f"{path}.{key}"))
        return errors
    if isinstance(expected, list):
        if not isinstance(actual, list):
            return [f"{path}: expected array, got {type(actual).__name__}"]
        if len(expected) != len(actual):
            return [f"{path}: expected {len(expected)} items, got {len(actual)}"]
        errors = []
        for index, value in enumerate(expected):
            errors.extend(assert_equal(value, actual[index], f"{path}[{index}]"))
        return errors
    if expected != actual:
        return [f"{path}: expected {expected!r}, got {actual!r}"]
    return []


def native_output(decision: dict[str, Any]) -> dict[str, Any]:
    for key in ("native", "expected", "expect", "output"):
        value = decision.get(key)
        if isinstance(value, dict):
            return value
    raise ValueError("decision has no native/expected/expect/output object to compare")


def compare_fields(decision: dict[str, Any], trace_defaults: list[str] | None) -> list[str]:
    fields = decision.get("compare") or trace_defaults or DEFAULT_COMPARE_FIELDS
    if not isinstance(fields, list) or not all(isinstance(item, str) for item in fields):
        raise ValueError("compare must be an array of field paths")
    return fields


def compare_decision(
    decision: dict[str, Any],
    actual: dict[str, Any],
    fields: list[str],
    compare_mode: str,
) -> list[str]:
    expected = native_output(decision)
    errors: list[str] = []
    for field in fields:
        expected_value = lookup(expected, field)
        actual_value = lookup(actual, field)
        if expected_value is MISSING:
            errors.append(f"native.{field}: missing")
            continue
        if actual_value is MISSING:
            errors.append(f"lua.{field}: missing")
            continue
        if compare_mode == "subset":
            errors.extend(assert_subset(expected_value, actual_value, field))
        else:
            errors.extend(assert_equal(expected_value, actual_value, field))
    return errors


def write_trace_discrepancy(path: Path, decision_id: str, native: Any, actual: Any, errors: list[str]) -> None:
    DISCREPANCY_ROOT.mkdir(parents=True, exist_ok=True)
    stem = f"{path.stem}.{decision_id}".replace("/", "_")
    native_text = json.dumps(native, indent=2, sort_keys=True).splitlines()
    actual_text = json.dumps(actual, indent=2, sort_keys=True).splitlines()
    diff = "\n".join(difflib.unified_diff(native_text, actual_text, fromfile="native", tofile="lua", lineterm=""))
    (DISCREPANCY_ROOT / f"{stem}.lua.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (DISCREPANCY_ROOT / f"{stem}.summary.txt").write_text("\n".join(errors) + "\n\n" + diff + "\n", encoding="utf-8")


def decision_expected_failure(document: Any, decision: dict[str, Any]) -> str | None:
    if has_expected_failure_marker(decision):
        return expected_failure_reason(decision)
    if isinstance(document, dict):
        return expected_failure_reason(document)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("traces", nargs="*", type=Path)
    parser.add_argument("--lua", help="Lua interpreter to use")
    args = parser.parse_args()

    lua = find_lua_interpreter(args.lua)
    if not lua:
        print("No Lua interpreter found. Install lua/lua5.4/luajit or pass --lua.", file=sys.stderr)
        return 77

    failures = 0
    for path in trace_paths(args.traces):
        document = json.loads(path.read_text(encoding="utf-8"))
        trace_defaults = document.get("compare") if isinstance(document, dict) else None
        compare_mode = document.get("compareMode", "exact") if isinstance(document, dict) else "exact"
        if compare_mode not in ("exact", "subset"):
            print(f"{path}: compareMode must be exact or subset", file=sys.stderr)
            failures += 1
            continue

        for index, decision in enumerate(trace_decisions(document)):
            decision_id = str(decision.get("id") or f"decision-{index + 1}")
            expected_failure = decision_expected_failure(document, decision)
            label = display_path(path)
            print(f"[native-trace] {label}#{decision_id}")
            function_name = decision.get("function")
            if not isinstance(function_name, str):
                print(f"{path}#{decision_id}: function must be a string", file=sys.stderr)
                failures += 1
                continue
            actual, error = run_decision(lua, function_name, decision.get("input", {}))
            if actual is None:
                if expected_failure:
                    print(
                        f"[expected-failure] {label}#{decision_id}: {expected_failure}",
                        file=sys.stderr,
                    )
                    continue
                print(error, file=sys.stderr)
                failures += 1
                continue

            try:
                fields = compare_fields(decision, trace_defaults)
                errors = compare_decision(decision, actual, fields, decision.get("compareMode", compare_mode))
            except ValueError as exc:
                print(f"{path}#{decision_id}: {exc}", file=sys.stderr)
                failures += 1
                continue

            if errors:
                write_trace_discrepancy(path, decision_id, native_output(decision), actual, errors)
                print("\n".join(errors), file=sys.stderr)
                if expected_failure:
                    print(
                        f"[expected-failure] {label}#{decision_id}: {expected_failure}",
                        file=sys.stderr,
                    )
                    continue
                failures += 1
            elif expected_failure:
                print(
                    f"{label}#{decision_id}: expected failure passed; remove the marker",
                    file=sys.stderr,
                )
                failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
