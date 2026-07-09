#!/usr/bin/env python3
"""Promote a candidate adventure AI Lua script after evaluation."""

from __future__ import annotations

import argparse
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def source_from_evaluation(evaluation: dict[str, Any]) -> Path:
    candidate = as_dict(evaluation.get("candidate"))
    for key in ("snapshot", "script"):
        value = candidate.get(key)
        if not value:
            continue
        text = str(value)
        if text.startswith("file:"):
            text = text[5:]
        path = Path(text)
        if path.is_file():
            return path
    raise ValueError("Evaluation does not reference a readable candidate script snapshot or file path")


def main() -> int:
    parser = argparse.ArgumentParser(description="Promote a ScriptedAdventureAI Lua candidate after evaluation.")
    parser.add_argument("--evaluation", type=Path, required=True, help="Path to evaluation.json.")
    parser.add_argument("--champion", type=Path, default=Path("scripts/ai/defaultAdventure.lua"), help="Champion script to replace.")
    parser.add_argument("--archive-dir", type=Path, default=Path("scripts/ai/archive"), help="Directory for archived champions.")
    parser.add_argument("--candidate", type=Path, help="Candidate script path. Defaults to candidate snapshot from evaluation.")
    parser.add_argument("--label", help="Archive label. Defaults to UTC timestamp and score delta.")
    parser.add_argument("--force", action="store_true", help="Promote even if evaluation verdict is not promote.")
    parser.add_argument("--dry-run", action="store_true", help="Print planned file operations without copying.")
    args = parser.parse_args()

    evaluation = load_json(args.evaluation)
    promotion = as_dict(evaluation.get("promotion"))
    verdict = promotion.get("verdict")
    if verdict != "promote" and not args.force:
        raise SystemExit(f"Refusing to promote because evaluation verdict is '{verdict}'. Use --force to override.")

    candidate = args.candidate or source_from_evaluation(evaluation)
    if not candidate.is_file():
        raise SystemExit(f"Candidate script does not exist: {candidate}")
    if not args.champion.is_file():
        raise SystemExit(f"Champion script does not exist: {args.champion}")

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    score_delta = evaluation.get("scoreDelta", "unknown")
    label = args.label or f"{timestamp}-score-{score_delta}"
    archive_path = args.archive_dir / f"{args.champion.stem}-{label}{args.champion.suffix}"
    manifest_path = args.archive_dir / f"{args.champion.stem}-{label}.promotion.json"

    operations = {
        "verdict": verdict,
        "candidate": str(candidate),
        "champion": str(args.champion),
        "archive": str(archive_path),
        "manifest": str(manifest_path),
        "scoreDelta": evaluation.get("scoreDelta"),
        "qualityDelta": evaluation.get("qualityDelta"),
        "promotion": promotion,
    }

    print(json.dumps(operations, indent=2, sort_keys=True))
    if args.dry_run:
        return 0

    args.archive_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.champion, archive_path)
    shutil.copy2(candidate, args.champion)
    manifest_path.write_text(json.dumps(operations, indent=2, sort_keys=True), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
