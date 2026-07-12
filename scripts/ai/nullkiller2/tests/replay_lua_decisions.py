#!/usr/bin/env python3
"""Replay normalized LuaNullkiller2 decision fixtures."""

from __future__ import annotations

import argparse
import difflib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[4]
LUA_ROOT = REPO_ROOT / "scripts/ai/nullkiller2"
REPLAY_ROOT = LUA_ROOT / "tests/fixtures/replay"
DISCREPANCY_ROOT = LUA_ROOT / "tests/fixtures/discrepancies"


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


def lua_literal(value: Any) -> str:
    if value is None:
        return "nil"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, list):
        return "{ " + ", ".join(lua_literal(item) for item in value) + " }"
    if isinstance(value, dict):
        parts: list[str] = []
        for key, item in value.items():
            if isinstance(key, str) and key.lstrip("-").isdigit():
                parts.append(f"[{key}] = {lua_literal(item)}")
            elif isinstance(key, str) and key.isidentifier():
                parts.append(f"{key} = {lua_literal(item)}")
            else:
                parts.append(f"[{lua_literal(key)}] = {lua_literal(item)}")
        return "{ " + ", ".join(parts) + " }"
    raise TypeError(f"cannot encode {type(value).__name__} as Lua literal")


def fixture_paths(explicit: list[Path]) -> list[Path]:
    if explicit:
        return [path if path.is_absolute() else REPO_ROOT / path for path in explicit]
    return sorted(REPLAY_ROOT.glob("*.json"))


def replay_script(function_name: str, input_value: Any) -> str:
    return f"""
local Script = require("main")

local function isArray(value)
	if type(value) ~= "table" then
		return false
	end
	local count = 0
	for key, _ in pairs(value) do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return false
		end
		count = count + 1
	end
	if count == 0 then
		return false
	end
	for index = 1, count do
		if value[index] == nil then
			return false
		end
	end
	return true
end

local function escapeString(value)
	value = value:gsub("\\\\", "\\\\\\\\")
	value = value:gsub('"', '\\\\"')
	value = value:gsub("\\n", "\\\\n")
	value = value:gsub("\\r", "\\\\r")
	value = value:gsub("\\t", "\\\\t")
	return '"' .. value .. '"'
end

local function encode(value)
	local kind = type(value)
	if kind == "nil" then
		return "null"
	elseif kind == "boolean" then
		return value and "true" or "false"
	elseif kind == "number" then
		return tostring(value)
	elseif kind == "string" then
		return escapeString(value)
	elseif kind == "table" then
		if isArray(value) then
			local items = {{}}
			for index = 1, #value do
				table.insert(items, encode(value[index]))
			end
			return "[" .. table.concat(items, ",") .. "]"
		end
		local keys = {{}}
		for key, _ in pairs(value) do
			table.insert(keys, tostring(key))
		end
		table.sort(keys)
		local items = {{}}
		for _, key in ipairs(keys) do
			local item = value[key]
			if item == nil then
				item = value[tonumber(key)]
			end
			table.insert(items, escapeString(key) .. ":" .. encode(item))
		end
		return "{{" .. table.concat(items, ",") .. "}}"
	end
	return "null"
end

local events = {{}}
local hostCommands = {{}}
local ended = false

local ai = {{
	trace = function(_, event, data)
		table.insert(events, {{ event = event, data = data or {{}} }})
	end,
	command = function(_, name, payload)
		table.insert(hostCommands, {{ name = name, payload = payload or {{}} }})
		return {{ ok = true }}
	end,
	endTurn = function(_)
		ended = true
		return {{ ok = true }}
	end
}}

local result = Script[{json.dumps(function_name)}](ai, {lua_literal(input_value)})
print(encode({{
	status = result.status,
	intent = result.intent,
	selection = result.selection,
	side = result.side,
	ended = ended,
	commandJournal = result.commandJournal,
	hostCommands = hostCommands,
	trace = events,
	resultTrace = result.trace,
	memory = result.memory
}}))
"""


def run_fixture(lua: str, path: Path) -> tuple[dict[str, Any] | None, str]:
    fixture = json.loads(path.read_text(encoding="utf-8"))
    script = replay_script(fixture["function"], fixture.get("input", {}))
    env = os.environ.copy()
    env["LUA_PATH"] = lua_path()
    completed = subprocess.run(
        [lua, "-"],
        input=script,
        cwd=REPO_ROOT,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        return None, completed.stderr
    try:
        return json.loads(completed.stdout), ""
    except json.JSONDecodeError as exc:
        return None, f"{exc}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"


def assert_subset(expected: Any, actual: Any, path: str = "$") -> list[str]:
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{path}: expected object, got {type(actual).__name__}"]
        errors: list[str] = []
        for key, value in expected.items():
            if key not in actual:
                errors.append(f"{path}.{key}: missing")
            else:
                errors.extend(assert_subset(value, actual[key], f"{path}.{key}"))
        return errors
    if isinstance(expected, list):
        if not isinstance(actual, list):
            return [f"{path}: expected array, got {type(actual).__name__}"]
        if len(expected) != len(actual):
            return [f"{path}: expected {len(expected)} items, got {len(actual)}"]
        errors = []
        for index, value in enumerate(expected):
            errors.extend(assert_subset(value, actual[index], f"{path}[{index}]"))
        return errors
    if expected != actual:
        return [f"{path}: expected {expected!r}, got {actual!r}"]
    return []


def write_discrepancy(path: Path, expected: Any, actual: Any, errors: list[str]) -> None:
    DISCREPANCY_ROOT.mkdir(parents=True, exist_ok=True)
    stem = path.stem
    expected_text = json.dumps(expected, indent=2, sort_keys=True).splitlines()
    actual_text = json.dumps(actual, indent=2, sort_keys=True).splitlines()
    diff = "\n".join(difflib.unified_diff(expected_text, actual_text, fromfile="expected", tofile="actual", lineterm=""))
    (DISCREPANCY_ROOT / f"{stem}.actual.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (DISCREPANCY_ROOT / f"{stem}.summary.txt").write_text("\n".join(errors) + "\n\n" + diff + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixtures", nargs="*", type=Path)
    parser.add_argument("--lua", help="Lua interpreter to use")
    args = parser.parse_args()

    lua = find_lua_interpreter(args.lua)
    if not lua:
        print("No Lua interpreter found. Install lua/lua5.4/luajit or pass --lua.", file=sys.stderr)
        return 77

    failures = 0
    for path in fixture_paths(args.fixtures):
        fixture = json.loads(path.read_text(encoding="utf-8"))
        print(f"[replay] {path.relative_to(REPO_ROOT)}")
        actual, error = run_fixture(lua, path)
        if actual is None:
            print(error, file=sys.stderr)
            failures += 1
            continue
        expected = fixture.get("expect", {})
        errors = assert_subset(expected, actual)
        if errors:
            write_discrepancy(path, expected, actual, errors)
            print("\n".join(errors), file=sys.stderr)
            failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
