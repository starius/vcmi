#!/usr/bin/env python3
#
# Prototype parser/checker for VCMI readable game transcripts.

from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
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
    documents: list[dict[str, Any]], transcript_path: Path
) -> list[dict[str, Any]]:
    companion_info = documents[0].get("companions")
    if not isinstance(companion_info, dict) or not isinstance(companion_info.get("battles"), str):
        raise VGTError("header companions.battles is missing or invalid")
    companion_path = transcript_path.parent / companion_info["battles"]
    try:
        companion = yaml.safe_load(companion_path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise VGTError(f"failed to parse battle companion {companion_path}: {exc}") from exc
    except OSError as exc:
        raise VGTError(f"failed to read battle companion {companion_path}: {exc}") from exc
    if not isinstance(companion, dict) or companion.get("vgtBattles") != 4:
        raise VGTError(f"{companion_path} is not a VGT 4 battle companion")
    battles = companion.get("battles")
    if battles is None:
        battles = []
    if not isinstance(battles, list):
        raise VGTError(f"{companion_path} has no battles list")

    by_id: dict[int, dict[str, Any]] = {}
    for index, entry in enumerate(battles):
        battle = entry.get("battle") if isinstance(entry, dict) else None
        battle_id = battle.get("id") if isinstance(battle, dict) else None
        if type(battle_id) is not int or battle_id in by_id:
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
            reference = battle.get("tactics") if isinstance(battle, dict) else None
            if reference is None:
                records.append(record)
                continue
            if type(reference) is not int or reference not in by_id or reference in used:
                raise VGTError(f"invalid or repeated tactical battle reference {reference!r}")
            records.append({"battle": by_id[reference]})
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
    if isinstance(value, dict) and type(value.get("tactics")) is int:
        if not isinstance(value.get("forces"), dict) or not isinstance(value.get("outcome"), dict):
            raise VGTError(f"document {document_index} record {record_index} battle summary is incomplete")
        return
    if not isinstance(value, dict) or not isinstance(value.get("events"), list):
        raise VGTError(f"document {document_index} record {record_index} battle has no event list")
    random_before = value.get("randomBefore")
    if not isinstance(random_before, dict) or not isinstance(random_before.get("global"), str):
        raise VGTError(
            f"document {document_index} record {record_index} battle has no frozen initial RNG state"
        )
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
    units = value.get("units", {})
    if not isinstance(units, dict):
        raise VGTError(f"document {document_index} record {record_index} battle units is not a mapping")
    for name, unit in units.items():
        if not isinstance(name, str) or not name.startswith(("attacker/", "defender/")):
            raise VGTError(f"document {document_index} record {record_index} has non-descriptive battle unit {name!r}")
        if not isinstance(unit, dict) or type(unit.get("stack")) is not int:
            raise VGTError(f"document {document_index} record {record_index} battle unit {name!r} has no raw stack id")
    validate_battle_events(value["events"], document_index, record_index)


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
    companions = result.get("companions")
    if not isinstance(companions, dict) or not isinstance(companions.get("battles"), str):
        raise VGTError("header companions.battles is missing or invalid")
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
        detail = exc
        while detail.context:
            nested = jsonschema.exceptions.best_match(detail.context)
            if nested is None or nested is detail:
                break
            detail = nested
        location = "".join(f"[{part!r}]" for part in detail.absolute_path)
        raise VGTError(f"schema validation failed at transcript{location}: {detail.message}") from exc


def command_check(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    if args.schema:
        validate_schema(documents, args.schema)
    transcript_header = header(documents)
    if args.resource_root:
        validate_map_hash(transcript_header["map"], args.resource_root)
    expanded = expand_battle_companion(documents, args.transcript)
    if args.schema:
        validate_schema(expanded, args.schema)
    # Validate the optional tactical stream as eagerly as the main story.
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


_BATTLE_ROSTER_RE = re.compile(r'^        (.+): \{ stack: ([0-9]+),')
_AVAILABLE_RE = re.compile(r'^  - available: \{ (.+?): \{ (.*?) \} \}\s*$')
_PLAYER_COLORS = {"red", "blue", "tan", "green", "orange", "purple", "teal", "pink"}


def short_identifier(value: str) -> str:
    value = value.replace(":", "/")
    return value.removeprefix("core/")


def command_enrich_battle_outcomes(args: argparse.Namespace) -> int:
    try:
        snapshots = json.loads(args.captured.read_text(encoding="utf-8"))
        lines = args.transcript.read_text(encoding="utf-8").splitlines(keepends=True)
    except (OSError, json.JSONDecodeError) as exc:
        raise VGTError(f"failed to read enrichment input: {exc}") from exc
    if not isinstance(snapshots, list):
        raise VGTError("captured battle outcomes must be a JSON list")

    output: list[str] = []
    battle_index = 0
    battle_id: int | None = None
    roster: dict[int, str] = {}
    participants: dict[str, str] = {}
    in_battle = False
    in_units = False
    outcome_enrichment: list[str] = []
    seen_dwellings: set[str] = set()
    suppressed_refreshes = 0

    for line in lines:
        if outcome_enrichment and (
            line == "        aftermath:\n"
            or line.startswith("  - ")
            or line.startswith("---")
        ):
            output.extend(outcome_enrichment)
            outcome_enrichment = []
        if outcome_enrichment and line.startswith("        mana: "):
            continue
        if line == "  - battle:\n":
            battle_id = None
            roster = {}
            participants = {}
            in_battle = True
            in_units = False
        elif in_battle and line.startswith("      id: "):
            try:
                battle_id = int(line.removeprefix("      id: ").strip())
            except ValueError:
                pass
        elif in_battle and (line.startswith("      attacker: ") or line.startswith("      defender: ")):
            side, value = line.strip().split(": ", 1)
            participant = yaml.safe_load(value)
            if not isinstance(participant, str):
                raise VGTError(f"battle {battle_id} {side} is not a string")
            participants[side] = participant
        elif in_battle and line == "      units:\n":
            in_units = True
        elif line == "      events:\n":
            in_units = False
        elif in_units:
            match = _BATTLE_ROSTER_RE.match(line)
            if match:
                alias = yaml.safe_load(match.group(1))
                if not isinstance(alias, str):
                    raise VGTError("battle unit name is not a string")
                roster[int(match.group(2))] = alias

        available = _AVAILABLE_RE.match(line)
        if available:
            object_name = available.group(1)
            normalized = "/".join(
                part for part in object_name.split("/") if part not in _PLAYER_COLORS
            )
            is_refugee_camp = normalized.startswith("refugee-camp@")
            if not is_refugee_camp and normalized in seen_dwellings:
                suppressed_refreshes += 1
                continue
            seen_dwellings.add(normalized)

        output.append(line)
        if line != "      outcome:\n" or battle_id is None:
            continue
        if battle_index >= len(snapshots):
            raise VGTError("transcript contains more battles than captured outcomes")
        snapshot = snapshots[battle_index]
        battle_index += 1
        if not isinstance(snapshot, dict) or snapshot.get("battle") != battle_id:
            raise VGTError(
                f"captured battle {battle_index} does not match transcript battle id {battle_id}"
            )
        captured_survivors = snapshot.get("survivors")
        captured_created_units = snapshot.get("createdUnits")
        captured_mana = snapshot.get("mana")
        continuation = snapshot.get("continuation")
        if (
            not isinstance(captured_survivors, dict)
            or not isinstance(captured_created_units, dict)
            or not isinstance(captured_mana, dict)
            or not isinstance(continuation, dict)
        ):
            raise VGTError(f"captured battle {battle_index} has invalid outcome state")
        survivors: list[str] = []
        for stack_text, count in sorted(captured_survivors.items(), key=lambda item: int(item[0])):
            stack = int(stack_text)
            if stack not in roster:
                raise VGTError(
                    f"captured survivor stack {stack} is absent from battle {battle_index} roster"
                )
            survivors.append(f"{json.dumps(roster[stack], ensure_ascii=False)}: {int(count)}")
        outcome_enrichment.append(f"        survivors: {{ {', '.join(survivors)} }}\n")
        created_units: list[str] = []
        for stack_text, created in sorted(
            captured_created_units.items(), key=lambda item: int(item[0])
        ):
            stack = int(stack_text)
            if stack not in roster or stack_text not in captured_survivors:
                raise VGTError(f"captured created unit {stack} is invalid in battle {battle_index}")
            if not isinstance(created, dict) or not isinstance(created.get("creature"), str):
                raise VGTError(f"captured created unit {stack} has invalid state")
            created_units.append(
                f"{json.dumps(roster[stack], ensure_ascii=False)}: {{ creature: "
                f"{json.dumps(short_identifier(created['creature']), ensure_ascii=False)}, "
                f"count: {int(created['count'])}, hex: {int(created['hex'])} }}"
            )
        if created_units:
            outcome_enrichment.append(
                f"        createdUnits: {{ {', '.join(created_units)} }}\n"
            )
        outcome_enrichment.append(
            "        continuation: "
            + json.dumps(continuation, ensure_ascii=False, separators=(",", ":"))
            + "\n"
        )
        mana: list[str] = []
        for side in ("attacker", "defender"):
            if side not in captured_mana:
                continue
            if side not in participants:
                raise VGTError(f"captured battle {battle_index} has mana for absent {side}")
            mana.append(
                f"{json.dumps(participants[side], ensure_ascii=False)}: {int(captured_mana[side])}"
            )
        if mana:
            outcome_enrichment.append(f"        mana: {{ {', '.join(mana)} }}\n")
        battle_id = None
        in_battle = False

    output.extend(outcome_enrichment)
    if battle_index != len(snapshots):
        raise VGTError(
            f"captured outcome count mismatch: transcript used {battle_index}, capture has {len(snapshots)}"
        )
    try:
        args.output.write_text("".join(output), encoding="utf-8")
    except OSError as exc:
        raise VGTError(f"failed to write enriched transcript: {exc}") from exc
    print(f"enriched battles: {battle_index}")
    print(f"suppressed deterministic dwelling refreshes: {suppressed_refreshes}")
    return 0


def command_replay(args: argparse.Namespace) -> int:
    documents = load_documents(args.transcript)
    if args.schema:
        validate_schema(documents, args.schema)
    transcript_header = header(documents)
    if args.resource_root:
        validate_map_hash(transcript_header["map"], args.resource_root)
    documents = expand_battle_companion(documents, args.transcript)
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

    enrich = subcommands.add_parser(
        "enrich-battle-outcomes",
        help="add captured battle fast-forward state and suppress repeated deterministic dwelling refreshes",
    )
    enrich.add_argument("transcript", type=Path)
    enrich.add_argument("captured", type=Path)
    enrich.add_argument("output", type=Path)
    enrich.set_defaults(func=command_enrich_battle_outcomes)

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
