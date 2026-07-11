#!/usr/bin/env python3
"""Audit ScriptedAdventureAI callback visibility and base forwarding."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


CALLBACK_HEADERS = (
    "lib/callback/CGameInterface.h",
    "lib/callback/IGameEventsReceiver.h",
    "AI/Nullkiller2/AIGateway.h",
)
SCRIPTED_HEADER = "AI/ScriptedAdventure/CScriptedAdventureAI.h"
SCRIPTED_SOURCE = "AI/ScriptedAdventure/CScriptedAdventureAI.cpp"

# These are interface/planner-control hooks, not player-visible adventure facts
# that should be mirrored into the Lua update/query stream.
INTENTIONAL_MISSING_OVERRIDES: dict[str, str] = {
    "finish": "inherited gateway finish handling has no additional script-visible adventure state",
    "getBattleAIName": "static battle AI selection is configuration, not a per-game callback event",
    "invalidatePaths": "planner cache invalidation is an internal Nullkiller service",
}

INTENTIONAL_NO_SCRIPT_EXPOSURE: dict[str, str] = {
    "initGameInterface": "initializes callback pointers, config, and persisted script memory before any turn state exists",
    "requestRealized": "request completion plumbing updates waiters and query bookkeeping",
    "requestSent": "request send plumbing tracks waiters and query replies",
    "yourTurn": "turn entry starts the scripted day loop after acknowledging the engine turn query",
}

INTENTIONAL_NO_BASE_FORWARD: dict[str, str] = {
    "yourTurn": "scripted turn execution replaces the gateway full-turn entry point; fallback calls gateway makeTurn explicitly",
}


def strip_line_comment(line: str) -> str:
    return line.split("//", 1)[0].strip()


def declared_methods(text: str, *, include_virtual_defaults: bool = True) -> set[str]:
    methods: set[str] = set()
    for raw_line in text.splitlines():
        line = strip_line_comment(raw_line)
        if "(" not in line:
            continue
        if "override" not in line and (not include_virtual_defaults or "virtual" not in line):
            continue

        prefix = line.split("(", 1)[0].strip()
        if not prefix:
            continue

        name = prefix.split()[-1]
        name = name.split("::")[-1]
        name = name.lstrip("*&")
        if not name or name.startswith("~"):
            continue
        methods.add(name)

    return methods


def callback_methods_by_header(repo_root: Path) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for relative in CALLBACK_HEADERS:
        result[relative] = declared_methods(
            (repo_root / relative).read_text(encoding="utf-8"),
            include_virtual_defaults=relative != "AI/Nullkiller2/AIGateway.h",
        )
    return result


def scripted_override_methods(repo_root: Path) -> set[str]:
    return declared_methods((repo_root / SCRIPTED_HEADER).read_text(encoding="utf-8"))


def method_body(source: str, method: str) -> str | None:
    needle = f"CScriptedAdventureAI::{method}"
    name_index = source.find(needle)
    if name_index < 0:
        return None

    open_index = source.find("{", name_index)
    if open_index < 0:
        return None

    depth = 0
    for index in range(open_index, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[open_index + 1:index]

    raise ValueError(f"Method body is not closed: {method}")


def audit(repo_root: Path) -> dict[str, Any]:
    callbacks_by_header = callback_methods_by_header(repo_root)
    callback_methods = set().union(*callbacks_by_header.values())
    gateway_methods = callbacks_by_header["AI/Nullkiller2/AIGateway.h"]
    scripted_methods = scripted_override_methods(repo_root)
    source = (repo_root / SCRIPTED_SOURCE).read_text(encoding="utf-8")

    missing_overrides = sorted(
        method
        for method in callback_methods
        if method not in scripted_methods and method not in INTENTIONAL_MISSING_OVERRIDES
    )
    stale_missing_exclusions = sorted(
        method
        for method in INTENTIONAL_MISSING_OVERRIDES
        if method not in callback_methods
    )

    missing_script_exposure: list[str] = []
    missing_implementations: list[str] = []
    missing_base_forward: list[str] = []
    stale_no_exposure_exclusions = sorted(
        method
        for method in INTENTIONAL_NO_SCRIPT_EXPOSURE
        if method not in scripted_methods
    )
    stale_no_base_forward_exclusions = sorted(
        method
        for method in INTENTIONAL_NO_BASE_FORWARD
        if method not in scripted_methods
    )

    for method in sorted(scripted_methods & callback_methods):
        body = method_body(source, method)
        if body is None:
            missing_implementations.append(method)
            continue

        has_script_exposure = "appendScriptUpdate(" in body or "recordScriptQuery(" in body
        if not has_script_exposure and method not in INTENTIONAL_NO_SCRIPT_EXPOSURE:
            missing_script_exposure.append(method)

        if method in gateway_methods and method not in INTENTIONAL_NO_BASE_FORWARD:
            if f"AIGateway::{method}(" not in body:
                missing_base_forward.append(method)

    exposed_methods = sorted(
        method
        for method in scripted_methods & callback_methods
        if (body := method_body(source, method)) is not None
        and ("appendScriptUpdate(" in body or "recordScriptQuery(" in body)
    )

    return {
        "ok": (
            not missing_overrides
            and not stale_missing_exclusions
            and not missing_implementations
            and not missing_script_exposure
            and not stale_no_exposure_exclusions
            and not missing_base_forward
            and not stale_no_base_forward_exclusions
        ),
        "callbackMethodCount": len(callback_methods),
        "scriptedOverrideCount": len(scripted_methods & callback_methods),
        "scriptExposedCallbackCount": len(exposed_methods),
        "callbacksByHeader": {header: sorted(methods) for header, methods in callbacks_by_header.items()},
        "missingOverrides": missing_overrides,
        "missingImplementations": missing_implementations,
        "missingScriptExposure": missing_script_exposure,
        "missingBaseForward": missing_base_forward,
        "staleMissingOverrideExclusions": stale_missing_exclusions,
        "staleNoScriptExposureExclusions": stale_no_exposure_exclusions,
        "staleNoBaseForwardExclusions": stale_no_base_forward_exclusions,
        "intentionalMissingOverrideExclusions": INTENTIONAL_MISSING_OVERRIDES,
        "intentionalNoScriptExposureExclusions": INTENTIONAL_NO_SCRIPT_EXPOSURE,
        "intentionalNoBaseForwardExclusions": INTENTIONAL_NO_BASE_FORWARD,
    }


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=default_repo_root())
    args = parser.parse_args()

    result = audit(args.repo_root.resolve())
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
