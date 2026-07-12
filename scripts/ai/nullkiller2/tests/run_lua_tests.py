#!/usr/bin/env python3
"""Run LuaNullkiller2 Lua regression tests."""

from __future__ import annotations

import argparse
import difflib
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
LUA_ROOT = REPO_ROOT / "scripts/ai/nullkiller2"
UNIT_ROOT = LUA_ROOT / "tests/unit"
DIFFERENTIAL_ROOT = LUA_ROOT / "tests/differential"
FIXTURE_ROOT = LUA_ROOT / "tests/fixtures/differential"


def find_lua_interpreter(explicit: str | None) -> str | None:
    candidates = [explicit] if explicit else []
    candidates.extend(["lua", "lua5.4", "lua5.3", "luajit"])
    for candidate in candidates:
        if candidate and shutil.which(candidate):
            return candidate
    return None


def lua_path() -> str:
    parts = [
        str(LUA_ROOT / "?.lua"),
        str(LUA_ROOT / "?/init.lua"),
        str(LUA_ROOT / "?/?.lua"),
    ]
    existing = os.environ.get("LUA_PATH")
    if existing:
        parts.append(existing)
    return ";".join(parts)


def unit_tests() -> list[Path]:
    return sorted(UNIT_ROOT.glob("test_*.lua"))


def differential_tests() -> list[Path]:
    return sorted(DIFFERENTIAL_ROOT.glob("*.lua"))


def run_tests(lua: str, tests: list[Path]) -> int:
    env = os.environ.copy()
    env["LUA_PATH"] = lua_path()
    failures = 0

    for test in tests:
        print(f"[lua] {test.relative_to(REPO_ROOT)}")
        completed = subprocess.run([lua, str(test)], cwd=REPO_ROOT, env=env)
        if completed.returncode != 0:
            failures += 1

    return 1 if failures else 0


def run_differential_tests(lua: str, tests: list[Path]) -> int:
    env = os.environ.copy()
    env["LUA_PATH"] = lua_path()
    failures = 0

    for test in tests:
        expected_path = FIXTURE_ROOT / f"{test.stem}.expected"
        print(f"[diff] {test.relative_to(REPO_ROOT)}")
        completed = subprocess.run(
            [lua, str(test)],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        if completed.returncode != 0:
            sys.stderr.write(completed.stderr)
            failures += 1
            continue

        expected = expected_path.read_text(encoding="utf-8").splitlines()
        actual = completed.stdout.splitlines()
        if actual != expected:
            failures += 1
            diff = difflib.unified_diff(
                expected,
                actual,
                fromfile=str(expected_path.relative_to(REPO_ROOT)),
                tofile=str(test.relative_to(REPO_ROOT)),
                lineterm="",
            )
            print("\n".join(diff), file=sys.stderr)

    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("preset", nargs="?", default="unit", choices=["unit", "differential-smoke", "all", "list"])
    parser.add_argument("--lua", help="Lua interpreter to use")
    args = parser.parse_args()

    if args.preset == "list":
        for test in unit_tests() + differential_tests():
            print(test.relative_to(REPO_ROOT))
        return 0

    lua = find_lua_interpreter(args.lua)
    if not lua:
        print("No Lua interpreter found. Install lua/lua5.4/luajit or pass --lua.", file=sys.stderr)
        return 77

    if args.preset == "unit":
        return run_tests(lua, unit_tests())
    if args.preset == "differential-smoke":
        return run_differential_tests(lua, differential_tests())

    unit_result = run_tests(lua, unit_tests())
    differential_result = run_differential_tests(lua, differential_tests())
    return 1 if unit_result or differential_result else 0


if __name__ == "__main__":
    raise SystemExit(main())
