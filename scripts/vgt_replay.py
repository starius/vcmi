#!/usr/bin/env python3
#
# Prototype parser/checker for VCMI readable game transcripts.

from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any

import yaml


class VGTError(RuntimeError):
    pass


def load_documents(path: Path) -> list[dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            documents = list(yaml.safe_load_all(handle))
    except yaml.YAMLError as exc:
        raise VGTError(f"failed to parse YAML: {exc}") from exc
    except OSError as exc:
        raise VGTError(f"failed to read transcript: {exc}") from exc

    if not documents:
        raise VGTError("transcript is empty")
    for index, document in enumerate(documents):
        if not isinstance(document, dict):
            raise VGTError(f"document {index} is not a mapping")
    return documents


def iter_records(documents: list[dict[str, Any]]):
    for document_index, document in enumerate(documents[1:], start=1):
        records = document.get("actions", document.get("events"))
        if records is None:
            raise VGTError(f"document {document_index} has neither actions nor events")
        if not isinstance(records, list):
            raise VGTError(f"document {document_index} records field is not a list")
        for record_index, record in enumerate(records):
            if not isinstance(record, dict) or len(record) != 1:
                raise VGTError(f"document {document_index} record {record_index} is not a one-key mapping")
            yield document_index, record_index, record


def header(documents: list[dict[str, Any]]) -> dict[str, Any]:
    result = documents[0]
    if result.get("vgt") != 3:
        raise VGTError(f"unsupported VGT version: {result.get('vgt')!r}")
    map_info = result.get("map")
    if not isinstance(map_info, dict):
        raise VGTError("header map field is missing or invalid")
    hash_info = map_info.get("hash")
    if not isinstance(hash_info, dict):
        raise VGTError("header map.hash field is missing or invalid")
    if hash_info.get("algorithm") != "sha256":
        raise VGTError(f"unsupported map hash algorithm: {hash_info.get('algorithm')!r}")
    if not isinstance(hash_info.get("value"), str) or not hash_info["value"]:
        raise VGTError("header map.hash.value is missing")
    settings = result.get("settings")
    if not isinstance(settings, dict):
        raise VGTError("header settings field is missing or invalid")
    for field in ("start", "startTime", "difficulty", "randomSeed", "simturns", "timer", "extraOptions", "gameSettingsOverrides"):
        if field not in settings:
            raise VGTError(f"header settings.{field} is missing")
    for field in ("simturns", "timer", "extraOptions", "gameSettingsOverrides"):
        if not isinstance(settings[field], dict):
            raise VGTError(f"header settings.{field} must be a mapping")
    def validate_players(players: Any, field_name: str) -> None:
        if not isinstance(players, dict) or not players:
            raise VGTError(f"header {field_name} field is missing or invalid")
        for color, player in players.items():
            if not isinstance(player, dict):
                raise VGTError(f"header {field_name}.{color} must be a mapping")
            for field in ("controller", "faction", "hero", "heroPortrait", "heroNameTextId", "startingBonus", "handicap", "name", "connections", "computerOnly"):
                if field not in player:
                    raise VGTError(f"header {field_name}.{color}.{field} is missing")
            if not isinstance(player["handicap"], dict):
                raise VGTError(f"header {field_name}.{color}.handicap must be a mapping")

    players = result.get("players")
    validate_players(players, "players")
    initial_players = result.get("initialPlayers")
    if initial_players is not None:
        validate_players(initial_players, "initialPlayers")
    return result


def map_candidates(uri: str, roots: list[Path]) -> list[Path]:
    path = Path(uri)
    candidates: list[Path] = []
    if path.is_absolute():
        candidates.append(path)
    for root in roots:
        candidates.append(root / uri)
        candidates.append(root / "vcmi" / uri)
    return candidates


def validate_map_hash(map_info: dict[str, Any], roots: list[Path]) -> Path:
    uri = map_info.get("uri")
    if not isinstance(uri, str) or not uri:
        raise VGTError("header map.uri is missing")
    expected = map_info["hash"]["value"].lower()

    checked: list[str] = []
    for candidate in map_candidates(uri, roots):
        checked.append(str(candidate))
        if candidate.is_file():
            actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
            if actual != expected:
                raise VGTError(f"map hash mismatch for {candidate}: expected {expected}, got {actual}")
            return candidate

    raise VGTError("map file not found; checked: " + ", ".join(checked))


def summarize(documents: list[dict[str, Any]]) -> collections.Counter[str]:
    counter: collections.Counter[str] = collections.Counter()
    for _, _, record in iter_records(documents):
        counter[next(iter(record))] += 1
    return counter


def fail_on_unmodelled(documents: list[dict[str, Any]]) -> None:
    for document_index, record_index, record in iter_records(documents):
        if "unmodelled" in record:
            raise VGTError(f"unmodelled record at document {document_index}, record {record_index}")


def command_check(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    transcript_header = header(documents)
    if args.resource_root:
        validate_map_hash(transcript_header["map"], args.resource_root)
    if args.strict:
        fail_on_unmodelled(documents)

    counts = summarize(documents)
    total_records = sum(counts.values())
    print(f"documents: {len(documents)}")
    print(f"records: {total_records}")
    if args.resource_root:
        print("mapHash: ok")
    print("eventKeys:")
    for key, count in counts.most_common():
        print(f"  {key}: {count}")

    if args.normalized_json:
        with args.normalized_json.open("w", encoding="utf-8") as handle:
            json.dump(documents, handle, indent=2, sort_keys=True)
            handle.write("\n")
    return 0


def write_normalized_json(documents: list[dict[str, Any]], path: Path) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(documents, handle, indent=2, sort_keys=True)
        handle.write("\n")


def command_replay(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    transcript_header = header(documents)
    if args.resource_root:
        validate_map_hash(transcript_header["map"], args.resource_root)
    if args.strict:
        fail_on_unmodelled(documents)

    temporary_path: Path | None = None
    json_path = args.normalized_json
    if json_path is None:
        handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".json", delete=False)
        temporary_path = Path(handle.name)
        handle.close()
        json_path = temporary_path

    replay_documents = [transcript_header] if args.header_only else documents
    write_normalized_json(replay_documents, json_path)
    engine_binary = args.engine_binary
    if not engine_binary.is_absolute():
        engine_binary = (Path.cwd() / engine_binary).resolve()

    command = [
        str(engine_binary),
        "--vgt-replay-json",
        str(json_path),
        "--vgt-replay-save",
        str(args.output_save),
    ]
    if args.output_game_state_save:
        command.extend([
            "--vgt-replay-game-state-save",
            str(args.output_game_state_save),
        ])
    try:
        completed = subprocess.run(command, check=False)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    return completed.returncode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Parse and validate a VCMI readable game transcript.")
    subcommands = parser.add_subparsers(dest="command", required=True)

    check = subcommands.add_parser("check", help="validate transcript shape and print event coverage")
    check.add_argument("transcript", type=Path)
    check.add_argument("--resource-root", action="append", type=Path, default=[], help="root used to resolve map.uri")
    check.add_argument("--strict", action="store_true", help="fail if the transcript contains unmodelled records")
    check.add_argument("--normalized-json", type=Path, help="write parsed documents as normalized JSON")
    check.set_defaults(func=command_check)

    replay = subcommands.add_parser("replay", help="rebuild game state from a transcript and write a save")
    replay.add_argument("transcript", type=Path)
    replay.add_argument("--resource-root", action="append", type=Path, default=[], help="root used to resolve map.uri")
    replay.add_argument("--strict", action="store_true", help="fail if the transcript contains unmodelled records")
    replay.add_argument("--normalized-json", type=Path, help="keep the normalized JSON passed to the engine")
    replay.add_argument("--engine-binary", type=Path, required=True, help="path to the VCMI executable with VGT replay support")
    replay.add_argument("--output-save", type=Path, required=True, help="save file to write after replay")
    replay.add_argument("--output-game-state-save", type=Path, help="game-state-only save file to write after replay")
    replay.add_argument("--header-only", action="store_true", help="rebuild only the initialized state from the transcript header")
    replay.set_defaults(func=command_replay)
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except VGTError as exc:
        print(f"vgt_replay: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
