#!/usr/bin/env python3
"""Audit Lua adventure scripts for accidental full-day Nullkiller delegation."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


FULL_DELEGATION_RE = re.compile(r"\bai\s*:\s*nullkiller\s*\(")

# Full-day delegation is allowed for the explicit control script only. Normal
# policies should use bounded helpers such as ai:nullkillerBoundedDay or
# ai:nullkillerTurnSlice so Lua regains control between native subroutines.
INTENTIONAL_FULL_DELEGATION: dict[str, str] = {
    "scripts/ai/candidates/fallbackAdventure.lua": "explicit all-fallback control/champion script",
}


def strip_lua_comments(text: str) -> str:
    text = re.sub(r"--\[\[.*?\]\]", "", text, flags=re.DOTALL)
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


def lua_scripts(repo_root: Path) -> list[Path]:
    return sorted((repo_root / "scripts/ai").rglob("*.lua"))


def audit(repo_root: Path) -> dict[str, Any]:
    direct_delegations: dict[str, list[int]] = {}

    for path in lua_scripts(repo_root):
        relative = path.relative_to(repo_root).as_posix()
        text = strip_lua_comments(path.read_text(encoding="utf-8"))
        line_numbers = [
            line_number
            for line_number, line in enumerate(text.splitlines(), start=1)
            if FULL_DELEGATION_RE.search(line)
        ]
        if line_numbers and relative not in INTENTIONAL_FULL_DELEGATION:
            direct_delegations[relative] = line_numbers

    stale_allowlist = sorted(
        relative
        for relative in INTENTIONAL_FULL_DELEGATION
        if not (repo_root / relative).is_file()
    )

    return {
        "ok": not direct_delegations and not stale_allowlist,
        "scriptCount": len(lua_scripts(repo_root)),
        "directDelegations": direct_delegations,
        "staleFullDelegationAllowlist": stale_allowlist,
        "intentionalFullDelegation": INTENTIONAL_FULL_DELEGATION,
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
