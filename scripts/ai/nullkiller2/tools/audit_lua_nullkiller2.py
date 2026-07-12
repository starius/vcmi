#!/usr/bin/env python3
"""Audit LuaNullkiller2 parity scaffolding and forbidden native dependencies."""

from __future__ import annotations

import argparse
import glob
import json
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
PORT_MAP = REPO_ROOT / "scripts/ai/nullkiller2/PORT_MAP.json"
LUA_ROOT = REPO_ROOT / "scripts/ai/nullkiller2"
CPP_ROOT = REPO_ROOT / "AI/LuaNullkiller2"


FORBIDDEN_SCRIPT_PATTERNS = [
    re.compile(pattern)
    for pattern in [
        r"\bNK2AI\b",
        r"\bAIGateway\b",
        r"fallbackToNullkiller",
        r"delegateToNullkiller",
        r"runNullkiller",
        r"getNullkillerTask",
        r"nullkillerTask",
        r"boundedNullkiller",
    ]
]

FORBIDDEN_CPP_PATTERNS = [
    re.compile(pattern)
    for pattern in [
        r"#\s*include\s+[<\"].*(?:AI/Nullkiller2|[/\\]Nullkiller2[/\\]|\.\./Nullkiller2)",
        r"\bNK2AI::",
        r"\bAIGateway\b",
        r"target_link_libraries\s*\([^)]*\bNullkiller2\b",
    ]
]


def iter_text_files(root: Path):
    if not root.exists():
        return

    suffixes = {".cpp", ".h", ".hpp", ".cmake", ".txt", ".lua", ".py"}
    names = {"CMakeLists.txt"}
    for path in root.rglob("*"):
        if path.is_file() and (path.suffix in suffixes or path.name in names):
            yield path


def load_port_map() -> dict:
    with PORT_MAP.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def check_port_map(errors: list[str]) -> None:
    data = load_port_map()
    entries = data.get("entries", [])
    mapped_lua = set()

    for index, entry in enumerate(entries):
        cpp = entry.get("cpp")
        lua = entry.get("lua")
        status = entry.get("status")
        symbols = entry.get("symbols")

        if not status:
            errors.append(f"PORT_MAP entry {index} has no status")
        if not symbols:
            errors.append(f"PORT_MAP entry {index} has no symbols")

        if cpp is not None and not (REPO_ROOT / cpp).exists():
            errors.append(f"PORT_MAP entry {index} missing C++ source: {cpp}")
        if lua:
            mapped_lua.add(lua)
            if status != "pending_lua_port" and not (REPO_ROOT / lua).exists():
                errors.append(f"PORT_MAP entry {index} missing Lua source: {lua}")

    for group in data.get("sourceFileGroups", []):
        cpp_glob = group.get("cppGlob")
        lua_dir = group.get("luaDir")
        if not cpp_glob or not glob.glob(str(REPO_ROOT / cpp_glob)):
            errors.append(f"PORT_MAP source group has no C++ matches: {cpp_glob}")
        if lua_dir and not (REPO_ROOT / lua_dir).exists():
            errors.append(f"PORT_MAP source group missing Lua dir: {lua_dir}")

    for lua_file in LUA_ROOT.rglob("*.lua"):
        rel = lua_file.relative_to(REPO_ROOT).as_posix()
        if "/tests/" in rel:
            continue
        if rel not in mapped_lua:
            errors.append(f"Lua policy file is not listed in PORT_MAP: {rel}")


def check_forbidden_patterns(errors: list[str]) -> None:
    for path in iter_text_files(LUA_ROOT):
        rel = path.relative_to(REPO_ROOT).as_posix()
        if rel.endswith("PORT_MAP.json") or "/tests/" in rel or "/tools/" in rel:
            continue
        text = path.read_text(encoding="utf-8")
        for pattern in FORBIDDEN_SCRIPT_PATTERNS:
            if pattern.search(text):
                errors.append(f"Forbidden script dependency '{pattern.pattern}' in {rel}")

    for path in iter_text_files(CPP_ROOT):
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        for pattern in FORBIDDEN_CPP_PATTERNS:
            if pattern.search(text):
                errors.append(f"Forbidden C++ dependency '{pattern.pattern}' in {rel}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true", help="emit machine-readable result")
    args = parser.parse_args()

    errors: list[str] = []
    check_port_map(errors)
    check_forbidden_patterns(errors)

    if args.json:
        print(json.dumps({"ok": not errors, "errors": errors}, indent=2, sort_keys=True))
    elif errors:
        for error in errors:
            print(error, file=sys.stderr)
    else:
        print("LuaNullkiller2 audit passed.")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
