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


LEGACY_RECORD_KEYS = {
    "availableCreatures",
    "buildStructure",
    "moveHero",
    "movementPoints",
    "newTurn",
    "objectProperty",
    "primarySkill",
    "recruitCreatures",
    "rewardable",
    "secondarySkill",
}

LEGACY_IDENTIFIER_PREFIXES = ("core/", "core:", "hero/", "object/")


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
        if set(document) == {"turn", "actions"}:
            records = document["actions"]
        elif set(document) == {"world", "events"}:
            records = document["events"]
        else:
            raise VGTError(f"document {document_index} is neither a current turn nor world document")
        if not isinstance(records, list):
            raise VGTError(f"document {document_index} records field is not a list")
        for record_index, record in enumerate(records):
            if not isinstance(record, dict):
                raise VGTError(f"document {document_index} record {record_index} is not a mapping")
            contextual_build = "at" in record and "build" in record
            contextual_move = "with" in record and "move" in record
            if len(record) != 1 and not contextual_build and not contextual_move:
                raise VGTError(
                    f"document {document_index} record {record_index} is neither a one-key record nor a contextual scene"
                )
            if "decision" in record:
                raise VGTError(f"document {document_index} record {record_index} uses the removed generic decision wrapper")
            removed = LEGACY_RECORD_KEYS.intersection(record)
            if removed:
                raise VGTError(
                    f"document {document_index} record {record_index} uses removed record key {sorted(removed)[0]!r}"
                )
            validate_short_identifiers(record, document_index, record_index)
            if contextual_move:
                value = dict(record["move"])
                value["hero"] = record["with"]
                validate_move(value, document_index, record_index)
            elif "move" in record:
                validate_move(record["move"], document_index, record_index)
            if "battle" in record:
                validate_battle_block(record["battle"], document_index, record_index)
            yield document_index, record_index, record


def validate_short_identifiers(value: Any, document_index: int, record_index: int) -> None:
    if isinstance(value, dict):
        for child in value.values():
            validate_short_identifiers(child, document_index, record_index)
        return
    if isinstance(value, list):
        for child in value:
            validate_short_identifiers(child, document_index, record_index)
        return
    if not isinstance(value, str):
        return
    if value.startswith(LEGACY_IDENTIFIER_PREFIXES) or "/at-" in value:
        raise VGTError(
            f"document {document_index} record {record_index} contains removed identifier spelling {value!r}"
        )


def validate_move(value: Any, document_index: int, record_index: int) -> None:
    if not isinstance(value, dict):
        raise VGTError(f"document {document_index} record {record_index} move payload is not a mapping")
    if "path" in value:
        raise VGTError(f"document {document_index} record {record_index} uses the removed move path field")
    forms = sum(field in value for field in ("route", "steps"))
    if forms > 1:
        raise VGTError(f"document {document_index} record {record_index} move mixes route and steps")
    if "steps" in value:
        if not isinstance(value["steps"], str) or not value["steps"].strip() or "to" not in value:
            raise VGTError(f"document {document_index} record {record_index} encoded move needs non-empty steps and to")
        for token in value["steps"].split():
            direction, marker, count = token.partition("*")
            if direction not in {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}:
                raise VGTError(f"document {document_index} record {record_index} has invalid direction {direction!r}")
            if marker and (not count.isdigit() or int(count) < 2):
                raise VGTError(f"document {document_index} record {record_index} has invalid run length {token!r}")
    if "route" not in value:
        return
    route = value["route"]
    if type(value.get("z")) is not int:
        raise VGTError(f"document {document_index} record {record_index} move route has no integer z")
    if not isinstance(route, list) or len(route) < 2 or any(
        not isinstance(point, list)
        or len(point) != 2
        or any(type(coordinate) is not int for coordinate in point)
        for point in route
    ):
        raise VGTError(f"document {document_index} record {record_index} move route must contain only [x, y] points")


def validate_battle_block(value: Any, document_index: int, record_index: int) -> None:
    if not isinstance(value, dict) or not isinstance(value.get("events"), list):
        raise VGTError(f"document {document_index} record {record_index} battle has no event list")
    units = value.get("units", {})
    if not isinstance(units, dict):
        raise VGTError(f"document {document_index} record {record_index} battle units is not a mapping")
    for name, unit in units.items():
        if not isinstance(name, str) or not name.startswith(("attacker/", "defender/")):
            raise VGTError(f"document {document_index} record {record_index} has non-descriptive battle unit {name!r}")
        if not isinstance(unit, dict) or type(unit.get("stack")) is not int:
            raise VGTError(f"document {document_index} record {record_index} battle unit {name!r} has no raw stack id")
    for battle_index, event in enumerate(value["events"]):
        if isinstance(event, dict) and "decision" in event:
            raise VGTError(
                f"document {document_index} record {record_index} battle event {battle_index} "
                "uses the removed generic decision wrapper"
            )
        if not isinstance(event, dict):
            raise VGTError(
                f"document {document_index} record {record_index} battle event {battle_index} is not a mapping"
            )
        if "startAction" in event or event.get("event") == "startAction":
            raise VGTError(
                f"document {document_index} record {record_index} battle event {battle_index} repeats startAction"
            )
        for removed_field in ("stack", "stackID", "stackId", "casterStack"):
            if removed_field in event:
                raise VGTError(
                    f"document {document_index} record {record_index} battle event {battle_index} exposes raw {removed_field}"
                )


def record_key(record: dict[str, Any]) -> str:
    if "at" in record and "build" in record:
        return "build"
    if "with" in record and "move" in record:
        return "move"
    return next(iter(record))


def header(documents: list[dict[str, Any]]) -> dict[str, Any]:
    result = documents[0]
    if result.get("vgt") != 4:
        raise VGTError(f"unsupported VGT version: {result.get('vgt')!r}")
    if result.get("format") != "VCMI readable event transcript":
        raise VGTError(f"unsupported VGT format name: {result.get('format')!r}")
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
    for field in ("simturns", "extraOptions", "gameSettingsOverrides"):
        if not isinstance(settings[field], dict):
            raise VGTError(f"header settings.{field} must be a mapping")
    if settings["timer"] != "none" and not isinstance(settings["timer"], dict):
        raise VGTError("header settings.timer must be a mapping or none")
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
    validate_players(result.get("initialPlayers"), "initialPlayers")
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
        key = record_key(record)
        value = record.get(key)
        if key == "battle" and isinstance(value, dict) and isinstance(value.get("events"), list):
            counter[key] += len(value["events"])
        else:
            counter[key] += 1
    return counter


def fail_on_unmodelled(documents: list[dict[str, Any]]) -> None:
    for document_index, record_index, record in iter_records(documents):
        if "unmodelled" in record:
            raise VGTError(f"unmodelled record at document {document_index}, record {record_index}")


def validate_schema(documents: list[dict[str, Any]], schema_path: Path) -> None:
    try:
        import jsonschema
    except ImportError as exc:
        raise VGTError("--schema requires the Python jsonschema package") from exc

    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        validator = jsonschema.validators.validator_for(schema)
        validator.check_schema(schema)
        validator(schema).validate(documents)
    except OSError as exc:
        raise VGTError(f"failed to read schema: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise VGTError(f"failed to parse schema JSON: {exc}") from exc
    except jsonschema.SchemaError as exc:
        raise VGTError(f"invalid schema: {exc.message}") from exc
    except jsonschema.ValidationError as exc:
        location = "".join(f"[{part!r}]" for part in exc.absolute_path)
        raise VGTError(f"schema validation failed at transcript{location}: {exc.message}") from exc


def command_check(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    if args.schema:
        validate_schema(documents, args.schema)
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
    if args.schema:
        print("schema: ok")
    print("eventKeys:")
    for key, count in counts.most_common():
        print(f"  {key}: {count}")

    if args.normalized_json:
        with args.normalized_json.open("w", encoding="utf-8") as handle:
            json.dump(documents, handle, indent=2, sort_keys=True, ensure_ascii=False)
            handle.write("\n")
    return 0


def write_normalized_json(documents: list[dict[str, Any]], path: Path) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(documents, handle, indent=2, sort_keys=True, ensure_ascii=False)
        handle.write("\n")


def command_replay(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    if args.schema:
        validate_schema(documents, args.schema)
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
    if args.expected_turn_states:
        command.extend([
            "--vgt-replay-expected-turn-states",
            str(args.expected_turn_states),
        ])
    if args.output_turn_states:
        command.extend([
            "--vgt-replay-turn-states",
            str(args.output_turn_states),
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
    check.add_argument("--schema", type=Path, help="validate the parsed YAML stream against a JSON Schema")
    check.add_argument("--normalized-json", type=Path, help="write parsed documents as normalized JSON")
    check.set_defaults(func=command_check)

    replay = subcommands.add_parser("replay", help="rebuild game state from a transcript and write a save")
    replay.add_argument("transcript", type=Path)
    replay.add_argument("--resource-root", action="append", type=Path, default=[], help="root used to resolve map.uri")
    replay.add_argument("--strict", action="store_true", help="fail if the transcript contains unmodelled records")
    replay.add_argument("--schema", type=Path, help="validate the parsed YAML stream against a JSON Schema")
    replay.add_argument("--normalized-json", type=Path, help="keep the normalized JSON passed to the engine")
    replay.add_argument("--engine-binary", type=Path, required=True, help="path to the VCMI executable with VGT replay support")
    replay.add_argument("--output-save", type=Path, required=True, help="save file to write after replay")
    replay.add_argument("--output-game-state-save", type=Path, help="game-state-only save file to write after replay")
    replay.add_argument("--expected-turn-states", type=Path, help="directory of recorded turn-state saves to compare byte-for-byte")
    replay.add_argument("--output-turn-states", type=Path, help="directory in which to write replayed turn-state saves")
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
