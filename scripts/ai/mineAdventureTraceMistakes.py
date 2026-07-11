#!/usr/bin/env python3
"""Mine ScriptedAdventureAI traces for likely policy mistakes."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from summarizeAdventureTrace import as_dict, iter_trace_files, summarize  # noqa: E402


ACTION_EXPECTATION_TO_COMMAND_EXPECTATION = {
    "firstAction": "firstCommand",
    "actionsExact": "commandsExact",
    "actionsContain": "commandsContain",
    "actionsDoNotContain": "commandsDoNotContain",
}


def slug(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()
    return cleaned or "mistake"


def deep_copy_json(value: Any) -> Any:
    return json.loads(json.dumps(value))


def imperative_fixture(fixture: dict[str, Any]) -> dict[str, Any]:
    result = deep_copy_json(fixture)
    result["mode"] = "imperative"
    expect = as_dict(result.get("expect"))
    for action_key, command_key in ACTION_EXPECTATION_TO_COMMAND_EXPECTATION.items():
        if action_key in expect and command_key not in expect:
            expect[command_key] = expect[action_key]
        expect.pop(action_key, None)
    result["expect"] = expect

    note = "Draft imperative fixture: add refreshInput or hostResponses if the script needs fresh state after yielded commands."
    existing_notes = str(result.get("notes", "")).strip()
    result["notes"] = f"{existing_notes} {note}".strip() if existing_notes else note
    return result


def fixture_items(summary: dict[str, Any], fixture_mode: str = "plan") -> list[dict[str, Any]]:
    fixtures: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in as_dict(summary.get("mistakes")).get("items", []):
        item_dict = as_dict(item)
        source_fixture = as_dict(item_dict.get("fixture"))
        if not source_fixture:
            continue
        fixture = deep_copy_json(source_fixture)
        if fixture_mode == "imperative":
            fixture = imperative_fixture(fixture)
        key = json.dumps({
            "type": item_dict.get("type"),
            "expect": fixture.get("expect"),
            "input": fixture.get("input"),
        }, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        fixture["sourceMistake"] = {
            "type": item_dict.get("type"),
            "severity": item_dict.get("severity"),
            "trace": item_dict.get("trace"),
            "description": item_dict.get("description"),
        }
        fixtures.append(fixture)
    return fixtures


def write_fixtures(fixtures: list[dict[str, Any]], output: Path, limit: int) -> list[Path]:
    output.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for index, fixture in enumerate(fixtures[:limit], start=1):
        name = slug(str(fixture.get("name") or f"mistake-{index:03d}"))
        path = output / f"{index:03d}-{name}.json"
        path.write_text(json.dumps(fixture, indent=2, sort_keys=True), encoding="utf-8")
        written.append(path)
    return written


def print_text(summary: dict[str, Any], limit: int) -> None:
    mistakes = as_dict(summary.get("mistakes"))
    print(f"mistakes: {mistakes.get('total', 0)}")
    print(f"important mistakes: {mistakes.get('important', 0)}")
    counts = as_dict(mistakes.get("counts"))
    if counts:
        print("types:")
        for key, count in list(counts.items())[:limit]:
            print(f"  {key}: {count}")
    for item in mistakes.get("items", [])[:limit]:
        item_dict = as_dict(item)
        print(
            f"- severity={item_dict.get('severity')} type={item_dict.get('type')} "
            f"day={item_dict.get('day')} player={item_dict.get('player')}: {item_dict.get('description')}"
        )
        if trace := item_dict.get("trace"):
            print(f"  trace: {trace}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Mine ScriptedAdventureAI traces for likely policy mistakes.")
    parser.add_argument("paths", nargs="+", help="Trace files or directories containing trace JSON files.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON.")
    parser.add_argument("--limit", type=int, default=20, help="Maximum text rows or fixture files.")
    parser.add_argument("--mistake-limit", type=int, default=500, help="Maximum mistakes analyzed from the summary.")
    parser.add_argument("--write-fixtures", type=Path, help="Write draft JSON policy fixtures to this directory.")
    parser.add_argument("--fixture-mode", choices=("plan", "imperative"), default="plan", help="Shape written fixture expectations for legacy planDay output actions or imperative runDay command streams.")
    args = parser.parse_args()

    summary = summarize(iter_trace_files(args.paths), max_mistakes=max(0, args.mistake_limit))
    fixtures = fixture_items(summary, args.fixture_mode)
    written: list[Path] = []
    if args.write_fixtures:
        written = write_fixtures(fixtures, args.write_fixtures, max(1, args.limit))

    result = {
        "mistakes": summary["mistakes"],
        "quality": summary["quality"],
        "fixtureCandidates": fixtures[: max(0, args.limit)],
        "writtenFixtures": [str(path) for path in written],
    }
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_text(summary, max(1, args.limit))
        if written:
            print("written fixtures:")
            for path in written:
                print(f"  {path}")
    return 1 if summary["parse_errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
