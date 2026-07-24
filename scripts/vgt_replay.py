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
    "arrangeStacks",
    "availableCreatures",
    "buildStructure",
    "hireHero",
    "moveHero",
    "movementPoints",
    "newTurn",
    "objectProperty",
    "primarySkill",
    "queryAnswer",
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


def expand_battle_companion(
    documents: list[dict[str, Any]], transcript_path: Path, *, required: bool
) -> list[dict[str, Any]]:
    companion_path = Path(str(transcript_path) + ".battles.yaml")
    if not companion_path.is_file():
        if required:
            raise VGTError(f"tactical battle companion is missing: {companion_path}")
        return documents
    try:
        companion = yaml.safe_load(companion_path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise VGTError(f"failed to parse battle companion {companion_path}: {exc}") from exc
    except OSError as exc:
        raise VGTError(f"failed to read battle companion {companion_path}: {exc}") from exc
    if not isinstance(companion, dict) or companion.get("vgtBattles") != 4:
        raise VGTError(f"{companion_path} is not a VGT 4 battle companion")
    if set(companion) != {"vgtBattles", "main", "battles"}:
        raise VGTError(f"{companion_path} has unsupported top-level fields")
    if companion.get("main") != transcript_path.name:
        raise VGTError(
            f"{companion_path} belongs to {companion.get('main')!r}, not {transcript_path.name!r}"
        )
    battles = companion.get("battles")
    if battles is None:
        battles = []
    if not isinstance(battles, list):
        raise VGTError(f"{companion_path} has no battles list")

    by_id: dict[int, dict[str, Any]] = {}
    for index, entry in enumerate(battles):
        battle = entry.get("battle") if isinstance(entry, dict) else None
        battle_id = battle.get("id") if isinstance(battle, dict) else None
        if (
            not isinstance(entry, dict)
            or set(entry) != {"battle"}
            or not isinstance(battle, dict)
            or set(battle) != {"id", "randomBefore", "events"}
            or type(battle_id) is not int
            or battle_id < 0
            or battle_id in by_id
        ):
            raise VGTError(f"battle companion entry {index} has an invalid or duplicate id")
        by_id[battle_id] = battle

    expanded = [documents[0]]
    used: set[int] = set()
    for document in documents[1:]:
        copy = dict(document)
        record_key = "actions" if "actions" in copy else "events"
        records: list[Any] = []
        for record in copy[record_key]:
            battle = record.get("battle") if isinstance(record, dict) else None
            reference = battle.get("id") if isinstance(battle, dict) else None
            if reference is None:
                records.append(record)
                continue
            if type(reference) is not int or reference not in by_id or reference in used:
                raise VGTError(f"invalid or repeated tactical battle reference {reference!r}")
            merged = dict(battle)
            tactical = by_id[reference]
            for field in ("randomBefore", "events"):
                if field not in tactical:
                    raise VGTError(f"battle companion entry {reference} has no {field}")
                merged[field] = tactical[field]
            records.append({"battle": merged})
            used.add(reference)
        copy[record_key] = records
        expanded.append(copy)
    if used != set(by_id):
        missing = sorted(set(by_id) - used)
        raise VGTError(f"unreferenced battle companion entries: {missing[:10]}")
    return expanded


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
            if isinstance(record, str):
                if record not in {"endTurn", "ready"}:
                    raise VGTError(f"document {document_index} record {record_index} has unsupported scalar action {record!r}")
                yield document_index, record_index, record
                continue
            if not isinstance(record, dict):
                raise VGTError(f"document {document_index} record {record_index} is not a mapping or scalar action")
            hero_scene = "with" in record and "actions" in record
            if len(record) != 1 and not hero_scene:
                raise VGTError(
                    f"document {document_index} record {record_index} is neither a one-key record nor a bounded hero scene"
                )
            if "decision" in record:
                raise VGTError(f"document {document_index} record {record_index} uses the removed generic decision wrapper")
            removed = LEGACY_RECORD_KEYS.intersection(record)
            if removed:
                raise VGTError(
                    f"document {document_index} record {record_index} uses removed record key {sorted(removed)[0]!r}"
                )
            validate_short_identifiers(record, document_index, record_index)
            if "move" in record:
                validate_move(record["move"], document_index, record_index)
            for payload in find_movement_payloads(record):
                validate_move(payload, document_index, record_index, require_hero=False)
            if hero_scene:
                actions = record.get("actions")
                if not isinstance(actions, list) or len(actions) < 2:
                    raise VGTError(f"document {document_index} record {record_index} hero scene needs at least two actions")
                for action in actions:
                    if not isinstance(action, dict) or len(action) != 1:
                        raise VGTError(f"document {document_index} record {record_index} hero scene action is not a one-key mapping")
                    if "move" in action:
                        validate_move(action["move"], document_index, record_index, require_hero=False)
                    for payload in find_movement_payloads(action):
                        validate_move(payload, document_index, record_index, require_hero=False)
            if "battle" in record:
                validate_battle_block(record["battle"], document_index, record_index)
            yield document_index, record_index, record


def validate_short_identifiers(value: Any, document_index: int, record_index: int) -> None:
    if isinstance(value, dict):
        if "query" in value:
            raise VGTError(
                f"document {document_index} record {record_index} exposes removed server query bookkeeping"
            )
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


def find_movement_payloads(value: Any):
    if isinstance(value, list):
        for child in value:
            yield from find_movement_payloads(child)
        return
    if not isinstance(value, dict):
        return
    for key, child in value.items():
        if key == "approach" and isinstance(child, dict):
            yield child
        elif isinstance(child, (dict, list)):
            yield from find_movement_payloads(child)


def validate_position(value: Any, document_index: int, record_index: int) -> None:
    if not isinstance(value, list) or len(value) not in (2, 3) or any(type(coordinate) is not int for coordinate in value):
        raise VGTError(f"document {document_index} record {record_index} position must be [x,y] or [x,y,nonzero-z]")
    if len(value) == 3 and value[2] == 0:
        raise VGTError(f"document {document_index} record {record_index} writes redundant surface z=0")


def validate_move(value: Any, document_index: int, record_index: int, require_hero: bool = True) -> None:
    if not isinstance(value, dict):
        raise VGTError(f"document {document_index} record {record_index} move payload is not a mapping")
    removed = {"path", "route", "z"}.intersection(value)
    if removed:
        raise VGTError(f"document {document_index} record {record_index} uses removed movement field {sorted(removed)[0]!r}")
    if require_hero and not isinstance(value.get("hero"), str):
        raise VGTError(f"document {document_index} record {record_index} standalone move has no hero")
    if "to" not in value:
        raise VGTError(f"document {document_index} record {record_index} move has no destination")
    validate_position(value["to"], document_index, record_index)
    if "steps" in value:
        if not isinstance(value["steps"], str) or not value["steps"].strip():
            raise VGTError(f"document {document_index} record {record_index} encoded move needs non-empty steps and to")
        for token in value["steps"].split():
            direction, marker, count = token.partition("*")
            if direction not in {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}:
                raise VGTError(f"document {document_index} record {record_index} has invalid direction {direction!r}")
            if marker and (not count.isdigit() or int(count) < 2):
                raise VGTError(f"document {document_index} record {record_index} has invalid run length {token!r}")


def validate_battle_block(value: Any, document_index: int, record_index: int) -> None:
    if not isinstance(value, dict) or type(value.get("id")) is not int:
        raise VGTError(f"document {document_index} record {record_index} battle has no numeric id")
    for field in ("attacker", "defender"):
        if not isinstance(value.get(field), str):
            raise VGTError(
                f"document {document_index} record {record_index} battle has no {field}"
            )
    if not isinstance(value.get("forces"), dict):
        raise VGTError(f"document {document_index} record {record_index} battle has no forces mapping")
    outcome = value.get("outcome")
    if not isinstance(outcome, dict):
        raise VGTError(f"document {document_index} record {record_index} battle has no explicit outcome")
    for field in ("result", "winnerSide", "winner", "loser"):
        if not isinstance(outcome.get(field), str):
            raise VGTError(
                f"document {document_index} record {record_index} battle outcome has no {field}"
            )
    if not isinstance(outcome.get("casualties"), dict):
        raise VGTError(
            f"document {document_index} record {record_index} battle outcome has no casualties mapping"
        )
    if not isinstance(outcome.get("survivors"), dict):
        raise VGTError(
            f"document {document_index} record {record_index} battle outcome has no survivors mapping"
        )
    if not isinstance(outcome.get("armies"), dict):
        raise VGTError(
            f"document {document_index} record {record_index} battle outcome has no armies mapping"
        )
    random = outcome.get("random")
    if not isinstance(random, dict):
        raise VGTError(
            f"document {document_index} record {record_index} battle outcome has no random mapping"
        )
    validate_battle_randomizer(
        random.get("beforeContinuation"), document_index, record_index, "outcome.random.beforeContinuation"
    )
    if "atContinuation" in random:
        validate_battle_randomizer(
            random["atContinuation"], document_index, record_index, "outcome.random.atContinuation"
        )
    for name, count in value["forces"].items():
        if not isinstance(name, str) or not name.startswith(("attacker/", "defender/")):
            raise VGTError(f"document {document_index} record {record_index} has non-descriptive battle unit {name!r}")
        if type(count) is not int or count <= 0:
            raise VGTError(f"document {document_index} record {record_index} battle force {name!r} has invalid count")
    if ("events" in value) != ("randomBefore" in value):
        raise VGTError(
            f"document {document_index} record {record_index} battle tactical fields are incomplete"
        )
    if "events" in value:
        if not isinstance(value["events"], list):
            raise VGTError(f"document {document_index} record {record_index} battle events is not a list")
        validate_battle_randomizer(
            value.get("randomBefore"), document_index, record_index, "randomBefore"
        )
        validate_battle_events(value["events"], document_index, record_index)


def validate_battle_randomizer(
    value: Any, document_index: int, record_index: int, field: str
) -> None:
    if not isinstance(value, dict) or not isinstance(value.get("global"), str):
        raise VGTError(
            f"document {document_index} record {record_index} battle {field} is not a randomizer mapping"
        )
    for stream in ("goodMorale", "badMorale", "goodLuck", "badLuck", "combatAbility"):
        entries = value.get(stream)
        if entries is None:
            continue
        if not isinstance(entries, dict):
            raise VGTError(
                f"document {document_index} record {record_index} battle {field}.{stream} is not a mapping"
            )
        for object_id, state in entries.items():
            if not isinstance(object_id, str) or not object_id.isdigit():
                raise VGTError(
                    f"document {document_index} record {record_index} battle {field}.{stream} has invalid object id"
                )
            if type(state) is int:
                continue
            if (
                not isinstance(state, dict)
                or type(state.get("generator")) is not int
                or type(state.get("bias")) is not int
            ):
                raise VGTError(
                    f"document {document_index} record {record_index} battle {field}.{stream}[{object_id!r}] is invalid"
                )


def validate_battle_events(
    events: list[Any], document_index: int, record_index: int, path: str = "events"
) -> None:
    for battle_index, event in enumerate(events):
        event_path = f"{path}[{battle_index}]"
        if isinstance(event, dict) and "decision" in event:
            raise VGTError(
                f"document {document_index} record {record_index} battle {event_path} "
                "uses the removed generic decision wrapper"
            )
        if not isinstance(event, dict):
            raise VGTError(
                f"document {document_index} record {record_index} battle {event_path} is not a mapping"
            )
        if "round" in event:
            if set(event) != {"round", "events"} or type(event["round"]) is not int or event["round"] < 1:
                raise VGTError(
                    f"document {document_index} record {record_index} battle {event_path} is not a valid round group"
                )
            if not isinstance(event["events"], list) or not event["events"]:
                raise VGTError(
                    f"document {document_index} record {record_index} battle {event_path} has no events"
                )
            validate_battle_events(event["events"], document_index, record_index, f"{event_path}.events")
            continue
        if "startAction" in event or event.get("event") == "startAction":
            raise VGTError(
                f"document {document_index} record {record_index} battle {event_path} repeats startAction"
            )
        for removed_field in ("stack", "stackID", "stackId", "casterStack"):
            if removed_field in event:
                raise VGTError(
                    f"document {document_index} record {record_index} battle {event_path} exposes raw {removed_field}"
                )


def count_battle_events(events: list[Any]) -> int:
    count = 0
    for event in events:
        if isinstance(event, dict) and isinstance(event.get("events"), list) and "round" in event:
            count += count_battle_events(event["events"])
        else:
            count += 1
    return count


def record_key(record: dict[str, Any] | str) -> str:
    if isinstance(record, str):
        return record
    if "with" in record and "actions" in record:
        return "with"
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
    text_encoding = map_info.get("textEncoding")
    if not isinstance(text_encoding, dict) or text_encoding.get("stored") != "utf-8":
        raise VGTError("header map.textEncoding must declare stored: utf-8")
    if text_encoding.get("source") not in {"utf-8", "h3m-auto"}:
        raise VGTError("header map.textEncoding.source must be utf-8 or h3m-auto")
    if text_encoding["source"] == "h3m-auto" and not isinstance(text_encoding.get("fallback"), str):
        raise VGTError("header map.textEncoding.fallback is required for h3m-auto source text")
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
    for field in ("start", "startTime", "difficulty", "randomSeed"):
        if field not in settings:
            raise VGTError(f"header settings.{field} is missing")
    for field in ("simturns", "extraOptions", "gameSettingsOverrides"):
        if field in settings and not isinstance(settings[field], dict):
            raise VGTError(f"header settings.{field} must be a mapping")
    if "timer" in settings and not isinstance(settings["timer"], dict):
        raise VGTError("header settings.timer must be a mapping")
    def validate_players(players: Any, field_name: str, require_resolved: bool) -> None:
        if not isinstance(players, dict) or (require_resolved and not players):
            raise VGTError(f"header {field_name} field is missing or invalid")
        for color, player in players.items():
            if not isinstance(player, dict):
                raise VGTError(f"header {field_name}.{color} must be a mapping")
            if require_resolved:
                for field in ("controller", "faction"):
                    if field not in player:
                        raise VGTError(f"header {field_name}.{color}.{field} is missing")
            if "handicap" in player and not isinstance(player["handicap"], dict):
                raise VGTError(f"header {field_name}.{color}.handicap must be a mapping")

    players = result.get("players")
    validate_players(players, "players", True)
    validate_players(result.get("initialPlayers"), "initialPlayers", False)
    initial_state = result.get("initialState")
    if not isinstance(initial_state, dict) or not isinstance(initial_state.get("heroes"), dict):
        raise VGTError("header initialState.heroes must be a mapping keyed by hero")
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
        value = record.get(key) if isinstance(record, dict) else None
        if key == "battle" and isinstance(value, dict) and isinstance(value.get("events"), list):
            counter[key] += count_battle_events(value["events"])
        else:
            counter[key] += 1
    return counter


def fail_on_unmodelled(documents: list[dict[str, Any]]) -> None:
    for document_index, record_index, record in iter_records(documents):
        if isinstance(record, dict) and "unmodelled" in record:
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
        leaves: list[jsonschema.ValidationError] = []

        def collect_leaves(error: jsonschema.ValidationError) -> None:
            if not error.context:
                leaves.append(error)
                return
            for nested in error.context:
                collect_leaves(nested)

        collect_leaves(exc)
        detail = max(leaves, key=lambda error: len(error.absolute_path), default=exc)
        location = "".join(f"[{part!r}]" for part in detail.absolute_path)
        raise VGTError(f"schema validation failed at transcript{location}: {detail.message}") from exc


def command_check(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    if args.schema:
        validate_schema(documents, args.schema)
    transcript_header = header(documents)
    if args.resource_root:
        validate_map_hash(transcript_header["map"], args.resource_root)
    # The main story is independently valid. When present, validate the optional
    # tactical stream by joining it to the main battle boundaries.
    expanded = expand_battle_companion(documents, args.transcript, required=False)
    if expanded is not documents:
        if args.schema:
            validate_schema(expanded, args.schema)
        collections.deque(iter_records(expanded), maxlen=0)
    if args.strict:
        fail_on_unmodelled(expanded)

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
    if not args.fast_forward_battles:
        documents = expand_battle_companion(documents, args.transcript, required=True)
        if args.schema:
            validate_schema(documents, args.schema)
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
    ]
    if args.no_save:
        command.append("--vgt-replay-no-save")
    else:
        command.extend(["--vgt-replay-save", str(args.output_save)])
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
    if args.captured_battle_outcomes:
        command.extend([
            "--vgt-replay-captured-battle-outcomes",
            str(args.captured_battle_outcomes),
        ])
    if args.fast_forward_battles:
        command.append("--vgt-replay-fast-forward-battles")
    command.extend(["--vgt-replay-log-level", args.log_level])
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

    replay = subcommands.add_parser("replay", help="rebuild game state from a transcript")
    replay.add_argument("transcript", type=Path)
    replay.add_argument("--resource-root", action="append", type=Path, default=[], help="root used to resolve map.uri")
    replay.add_argument("--strict", action="store_true", help="fail if the transcript contains unmodelled records")
    replay.add_argument("--schema", type=Path, help="validate the parsed YAML stream against a JSON Schema")
    replay.add_argument("--normalized-json", type=Path, help="keep the normalized JSON passed to the engine")
    replay.add_argument("--engine-binary", type=Path, required=True, help="path to the VCMI executable with VGT replay support")
    output = replay.add_mutually_exclusive_group(required=True)
    output.add_argument("--output-save", type=Path, help="save file to write after replay")
    output.add_argument("--no-save", action="store_true", help="run replay without writing a final save")
    replay.add_argument("--output-game-state-save", type=Path, help="game-state-only save file to write after replay")
    replay.add_argument("--expected-turn-states", type=Path, help="directory of recorded turn-state saves to compare byte-for-byte")
    replay.add_argument("--output-turn-states", type=Path, help="directory in which to write replayed turn-state saves")
    replay.add_argument("--captured-battle-outcomes", type=Path, help="write tactical-end survivor and randomizer state as JSON")
    replay.add_argument("--fast-forward-battles", action="store_true", help="apply recorded battle outcomes without replaying tactical events")
    replay.add_argument(
        "--log-level",
        choices=("trace", "debug", "info", "warn", "error"),
        default="info",
        help="engine log level during replay (default: info)",
    )
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
