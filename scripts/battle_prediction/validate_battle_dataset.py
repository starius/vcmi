#!/usr/bin/env python3
"""Validate generated battle-prediction datasets before using them as evidence."""

from __future__ import annotations

import argparse
import glob
import json
import os
import tarfile
from collections import Counter
from dataclasses import dataclass, field
from typing import Any, Iterable

from evaluate_nullkiller_predictor import battle_type, iter_json_lines, setup_key, shard_setup_key


MMAI_FALLBACK_PATTERNS = [
    "Could not load MMAI config",
    "falling back to BattleAI",
    "no path configured",
    "MMAI: load error",
]
MMAI_INIT_PATTERN = "MMAI version 13 initialized"


@dataclass
class LogScan:
    files: int = 0
    files_with_mmai_init: int = 0
    fallback_lines: list[str] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--expected-rows", type=int)
    parser.add_argument("--expected-schema", type=int)
    parser.add_argument("--expected-shards", type=int)
    parser.add_argument("--expected-shard-size", type=int)
    parser.add_argument("--expected-groups", type=int, help="Expected number of grouped battle setups for --group-key")
    parser.add_argument(
        "--group-key",
        choices=["setup", "shard"],
        default="setup",
        help="Grouping key used for --expected-groups and --min-groups. Use shard for generated repeated-simulation datasets.",
    )
    parser.add_argument("--min-groups", type=int, help="Minimum number of grouped battle setups for --group-key")
    parser.add_argument("--min-setup-groups", type=int)
    parser.add_argument("--require-complete-shards", action="store_true")
    parser.add_argument("--require-battle-types", help="Comma-separated battle types that must be present")
    parser.add_argument("--require-schema3-rich-fields", action="store_true", help="Require schema3 hero, army, and town/siege predictor fields")
    parser.add_argument(
        "--min-schema6-battle-start-counter",
        action="append",
        default=[],
        metavar="NAME=COUNT",
        help=(
            "Require schema6_battle_start counter NAME to be at least COUNT. "
            "Useful counters include rows, stacks_array, attacker_stack_rows, defender_stack_rows, "
            "wall_state_rows, wall_changed_rows, gate_changed_rows, and nonempty_obstacles."
        ),
    )
    parser.add_argument("--require-no-mmai-fallback", action="store_true")
    parser.add_argument("--require-mmai-initialized", action="store_true")
    parser.add_argument("--print-fallback-lines", type=int, default=20)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def parse_required_types(value: str | None) -> set[str]:
    if not value:
        return set()
    return {part.strip() for part in value.split(",") if part.strip()}


def parse_min_counters(values: list[str]) -> tuple[dict[str, int], list[str]]:
    result: dict[str, int] = {}
    errors = []
    for value in values:
        if "=" not in value:
            errors.append(f"invalid --min-schema6-battle-start-counter {value!r}, expected NAME=COUNT")
            continue

        name, count_text = value.split("=", 1)
        name = name.strip()
        count_text = count_text.strip()
        if not name:
            errors.append(f"invalid --min-schema6-battle-start-counter {value!r}, empty counter name")
            continue
        try:
            count = int(count_text)
        except ValueError:
            errors.append(f"invalid --min-schema6-battle-start-counter {value!r}, COUNT is not an integer")
            continue
        if count < 0:
            errors.append(f"invalid --min-schema6-battle-start-counter {value!r}, COUNT must be non-negative")
            continue
        result[name] = count
    return result, errors


def missing_keys(node: dict[str, Any], keys: list[str]) -> list[str]:
    return [key for key in keys if key not in node]


def validate_hero_fields(row: dict[str, Any], side: str) -> list[str]:
    hero = row.get(f"{side}Hero")
    if hero is None:
        return []
    if not isinstance(hero, dict):
        return [f"{side}Hero is not an object"]

    required = [
        "objectId",
        "type",
        "level",
        "mana",
        "currentMana",
        "manaLimit",
        "hasSpellbook",
        "combatSpellCount",
        "fightingStrength",
        "magicStrength",
        "heroStrength",
        "secondary",
        "spells",
        "combatSpells",
        "primary",
    ]
    errors = [f"{side}Hero missing {key}" for key in missing_keys(hero, required)]
    if "secondary" in hero and not isinstance(hero["secondary"], list):
        errors.append(f"{side}Hero.secondary is not an array")
    if "spells" in hero and not isinstance(hero["spells"], list):
        errors.append(f"{side}Hero.spells is not an array")
    if "combatSpells" in hero and not isinstance(hero["combatSpells"], list):
        errors.append(f"{side}Hero.combatSpells is not an array")
    if "primary" in hero and (not isinstance(hero["primary"], list) or len(hero["primary"]) != 4):
        errors.append(f"{side}Hero.primary is not a 4-item array")
    return errors


def validate_army_fields(row: dict[str, Any], side: str) -> list[str]:
    army = row.get(f"{side}Army")
    if not isinstance(army, list):
        return [f"{side}Army is not an array"]

    errors = []
    required_stack = ["slot", "creature", "count", "power", "stats", "experience"]
    required_stats = [
        "level",
        "faction",
        "fightValue",
        "aiValue",
        "growth",
        "attack",
        "defense",
        "damageMin",
        "damageMax",
        "hitPoints",
        "speed",
        "shots",
        "spellPoints",
        "doubleWide",
        "shooter",
        "flying",
        "blocksRetaliation",
        "unlimitedRetaliations",
        "additionalAttack",
        "additionalRetaliation",
        "returnAfterStrike",
        "twoHexAttackBreath",
        "attacksAllAdjacent",
        "threeHeadedAttack",
        "spellAfterAttack",
        "spellcaster",
        "mindImmune",
        "undead",
        "nonLiving",
        "magicResistance",
        "levelSpellImmunity",
        "spellDamageReduction",
        "blockAllMagic",
        "spellSchoolImmunity",
    ]
    for index, stack in enumerate(army):
        if not isinstance(stack, dict):
            errors.append(f"{side}Army[{index}] is not an object")
            continue
        errors.extend(f"{side}Army[{index}] missing {key}" for key in missing_keys(stack, required_stack))
        stats = stack.get("stats")
        if not isinstance(stats, dict):
            errors.append(f"{side}Army[{index}].stats is not an object")
            continue
        errors.extend(f"{side}Army[{index}].stats missing {key}" for key in missing_keys(stats, required_stats))
    return errors


def validate_army_snapshot(node: Any, name: str) -> list[str]:
    if not isinstance(node, dict):
        return [f"{name} is not an object"]

    errors = [f"{name} missing {key}" for key in missing_keys(node, ["objectId", "armyStrength", "stacks"])]
    stacks = node.get("stacks")
    if not isinstance(stacks, list):
        errors.append(f"{name}.stacks is not an array")
        return errors

    required_stack = ["slot", "creature", "count", "power", "stats", "experience"]
    required_stats = [
        "level",
        "faction",
        "fightValue",
        "aiValue",
        "growth",
        "attack",
        "defense",
        "damageMin",
        "damageMax",
        "hitPoints",
        "speed",
        "shots",
        "spellPoints",
        "doubleWide",
        "shooter",
        "flying",
        "blocksRetaliation",
        "unlimitedRetaliations",
        "additionalAttack",
        "additionalRetaliation",
        "returnAfterStrike",
        "twoHexAttackBreath",
        "attacksAllAdjacent",
        "threeHeadedAttack",
        "spellAfterAttack",
        "spellcaster",
        "mindImmune",
        "undead",
        "nonLiving",
        "magicResistance",
        "levelSpellImmunity",
        "spellDamageReduction",
        "blockAllMagic",
        "spellSchoolImmunity",
    ]
    for index, stack in enumerate(stacks):
        if not isinstance(stack, dict):
            errors.append(f"{name}.stacks[{index}] is not an object")
            continue
        errors.extend(f"{name}.stacks[{index}] missing {key}" for key in missing_keys(stack, required_stack))
        stats = stack.get("stats")
        if not isinstance(stats, dict):
            errors.append(f"{name}.stacks[{index}].stats is not an object")
            continue
        errors.extend(f"{name}.stacks[{index}].stats missing {key}" for key in missing_keys(stats, required_stats))
    return errors


def validate_town_pre_merge_state(row: dict[str, Any], town: dict[str, Any]) -> list[str]:
    if int(row.get("schema", 1)) < 5:
        return []

    if "townPreMergeState" not in row:
        return ["town battle missing townPreMergeState field"]

    pre_merge = row.get("townPreMergeState")
    needs_pre_merge = (
        battle_type(row) == "town-hero"
        and town.get("defendingHeroSource") == "visiting"
        and not town.get("hasGarrisonHero")
    )
    if pre_merge is None:
        return ["town-hero visiting siege missing townPreMergeState object"] if needs_pre_merge else []
    if not isinstance(pre_merge, dict):
        return ["townPreMergeState is not an object"]

    errors = [f"townPreMergeState missing {key}" for key in missing_keys(pre_merge, ["townId", "defendingHeroId", "townArmy", "defendingHeroArmy"])]
    errors.extend(validate_army_snapshot(pre_merge.get("townArmy"), "townPreMergeState.townArmy"))
    errors.extend(validate_army_snapshot(pre_merge.get("defendingHeroArmy"), "townPreMergeState.defendingHeroArmy"))
    return errors


def validate_battle_start_stacks(row: dict[str, Any]) -> list[str]:
    if int(row.get("schema", 1)) < 6:
        return []

    errors = []
    stacks = row.get("battleStartStacks")
    if not isinstance(stacks, list):
        errors.append("schema6 row missing battleStartStacks array")
        stacks = []

    required_stack = [
        "unitId",
        "side",
        "slot",
        "creature",
        "count",
        "baseAmount",
        "position",
        "initialPosition",
        "availableHealth",
        "totalHealth",
        "maxHealth",
        "firstHPLeft",
        "meleeAttack",
        "rangedAttack",
        "meleeDefense",
        "rangedDefense",
        "meleeDamageMin",
        "meleeDamageMax",
        "rangedDamageMin",
        "rangedDamageMax",
        "speed",
        "movementRange",
        "morale",
        "luck",
        "shotsAvailable",
        "shotsTotal",
        "castsAvailable",
        "castsTotal",
        "retaliationsAvailable",
        "retaliationsTotal",
        "alive",
        "validTarget",
        "doubleWide",
        "shooter",
        "canShoot",
        "caster",
        "canCast",
        "turret",
        "catapult",
        "ballista",
        "firstAidTent",
        "ammoCart",
        "summoned",
    ]
    for index, stack in enumerate(stacks):
        if not isinstance(stack, dict):
            errors.append(f"battleStartStacks[{index}] is not an object")
            continue
        errors.extend(f"battleStartStacks[{index}] missing {key}" for key in missing_keys(stack, required_stack))

    obstacles = row.get("battleStartObstacles")
    if not isinstance(obstacles, list):
        errors.append("schema6 row missing battleStartObstacles array")
        obstacles = []

    required_obstacle = [
        "uniqueId",
        "id",
        "type",
        "position",
        "trigger",
        "turnsRemaining",
        "spellLevel",
        "casterSide",
        "blocksTiles",
        "stopsMovement",
        "triggersEffects",
        "hidden",
        "passable",
        "trap",
        "removeOnTrigger",
        "revealed",
        "affectedTiles",
    ]
    for index, obstacle in enumerate(obstacles):
        if not isinstance(obstacle, dict):
            errors.append(f"battleStartObstacles[{index}] is not an object")
            continue
        errors.extend(f"battleStartObstacles[{index}] missing {key}" for key in missing_keys(obstacle, required_obstacle))
        if "affectedTiles" in obstacle and not isinstance(obstacle["affectedTiles"], list):
            errors.append(f"battleStartObstacles[{index}].affectedTiles is not an array")
    return errors


def validate_town_fields(row: dict[str, Any]) -> list[str]:
    type_name = battle_type(row)
    if not type_name.startswith("town"):
        return []

    town = row.get("defendedTown")
    if not isinstance(town, dict):
        return ["town battle missing defendedTown object"]

    required_town = [
        "objectId",
        "faction",
        "fortLevel",
        "hallLevel",
        "mageGuildLevel",
        "hasFort",
        "hasBuiltTavern",
        "hasBuiltGrail",
        "hasVisitingHero",
        "hasGarrisonHero",
        "defendingHeroSource",
        "battleTerrain",
        "armyStrength",
        "buildings",
        "fortifications",
        "towerDamage",
        "keepDamage",
    ]
    errors = [f"defendedTown missing {key}" for key in missing_keys(town, required_town)]
    if "buildings" in town and not isinstance(town["buildings"], list):
        errors.append("defendedTown.buildings is not an array")

    fortifications = town.get("fortifications")
    if isinstance(fortifications, dict):
        required_fortifications = [
            "wallsHealth",
            "citadelHealth",
            "upperTowerHealth",
            "lowerTowerHealth",
            "hasMoat",
            "citadelShooter",
            "upperTowerShooter",
            "lowerTowerShooter",
            "moatSpell",
        ]
        errors.extend(f"defendedTown.fortifications missing {key}" for key in missing_keys(fortifications, required_fortifications))
    elif "fortifications" in town:
        errors.append("defendedTown.fortifications is not an object")

    for key in ("towerDamage", "keepDamage"):
        damage = town.get(key)
        if isinstance(damage, dict):
            errors.extend(f"defendedTown.{key} missing {part}" for part in missing_keys(damage, ["min", "max"]))
        elif key in town:
            errors.append(f"defendedTown.{key} is not an object")

    required_wall_state = [
        "keep",
        "bottomTower",
        "bottomWall",
        "belowGate",
        "overGate",
        "upperWall",
        "upperTower",
        "gate",
        "gateState",
    ]
    wall_state_fields = ["finalWallState"]
    if int(row.get("schema", 1)) >= 4:
        wall_state_fields.append("initialWallState")
    if int(row.get("schema", 1)) >= 6:
        wall_state_fields.append("battleStartWallState")

    for key in wall_state_fields:
        wall_state = row.get(key)
        if isinstance(wall_state, dict):
            errors.extend(f"{key} missing {part}" for part in missing_keys(wall_state, required_wall_state))
        else:
            errors.append(f"town battle missing {key} object")

    defender_hero = row.get("defenderHero")
    if type_name == "town-hero":
        if not isinstance(defender_hero, dict):
            errors.append("town-hero battle missing defenderHero object")
        if not town.get("hasVisitingHero") and not town.get("hasGarrisonHero"):
            errors.append("town-hero battle has no visiting or garrison hero marker")
        if town.get("defendingHeroSource") not in ("visiting", "garrison"):
            errors.append(f"town-hero battle has invalid defendingHeroSource={town.get('defendingHeroSource')}")
    elif defender_hero is not None:
        errors.append(f"{type_name} battle unexpectedly has defenderHero")
    errors.extend(validate_town_pre_merge_state(row, town))
    return errors


def validate_schema3_rich_fields(path: str, max_examples: int = 20) -> dict[str, Any]:
    rows = 0
    invalid_rows = 0
    examples: list[str] = []

    for row in iter_json_lines(path):
        rows += 1
        row_errors = []
        if int(row.get("schema", 1)) < 3:
            row_errors.append(f"schema is {row.get('schema')}, expected at least 3")

        row_errors.extend(validate_hero_fields(row, "attacker"))
        row_errors.extend(validate_hero_fields(row, "defender"))
        row_errors.extend(validate_army_fields(row, "attacker"))
        row_errors.extend(validate_army_fields(row, "defender"))
        row_errors.extend(validate_battle_start_stacks(row))
        row_errors.extend(validate_town_fields(row))

        for key in ["attackerArmyStrength", "defenderArmyStrength", "battleType", "hasFortifications", "hasMoat"]:
            if key not in row:
                row_errors.append(f"row missing {key}")

        if row_errors:
            invalid_rows += 1
            if len(examples) < max_examples:
                shard = row.get("shardIndex", "?")
                row_index = row.get("row", "?")
                examples.append(f"shard={shard} row={row_index}: " + "; ".join(row_errors[:8]))

    return {
        "rows_checked": rows,
        "invalid_rows": invalid_rows,
        "examples": examples,
    }


def iter_logs_from_directory(path: str) -> Iterable[tuple[str, Iterable[str]]]:
    for file_name in sorted(glob.glob(os.path.join(path, "shard-*.log"))):
        with open(file_name, encoding="utf-8", errors="replace") as handle:
            yield file_name, handle


def iter_logs_from_archive(path: str) -> Iterable[tuple[str, Iterable[str]]]:
    with tarfile.open(path, "r:gz") as archive:
        members = sorted(
            (member for member in archive.getmembers() if os.path.basename(member.name).startswith("shard-") and member.name.endswith(".log")),
            key=lambda member: member.name,
        )
        for member in members:
            extracted = archive.extractfile(member)
            if extracted is None:
                continue
            yield member.name, (raw.decode("utf-8", errors="replace") for raw in extracted)


def iter_logs(path: str) -> Iterable[tuple[str, Iterable[str]]]:
    if os.path.isdir(path):
        yield from iter_logs_from_directory(path)
    elif path.endswith(".tar.gz") or path.endswith(".tgz"):
        yield from iter_logs_from_archive(path)


def scan_logs(path: str) -> LogScan:
    result = LogScan()
    for file_name, lines in iter_logs(path):
        result.files += 1
        has_mmai_init = False
        for number, line in enumerate(lines, start=1):
            if MMAI_INIT_PATTERN in line:
                has_mmai_init = True
            if any(pattern in line for pattern in MMAI_FALLBACK_PATTERNS):
                result.fallback_lines.append(f"{file_name}:{number}:{line.rstrip()}")
        result.files_with_mmai_init += int(has_mmai_init)
    return result


def has_stack_side(stacks: Any, side: int) -> bool:
    if not isinstance(stacks, list):
        return False
    return any(isinstance(stack, dict) and stack.get("side") == side for stack in stacks)


def wall_part(state: Any, part: str) -> Any:
    if not isinstance(state, dict):
        return None
    return state.get(part)


def load_summary(path: str, group_key: str) -> dict[str, Any]:
    schemas: Counter[int] = Counter()
    battle_types: Counter[str] = Counter()
    shard_rows: Counter[int] = Counter()
    setup_groups: Counter[str] = Counter()
    shard_groups: Counter[str] = Counter()
    town_fort_levels: Counter[int] = Counter()
    town_hero_sources: Counter[str] = Counter()
    town_has_fortifications: Counter[str] = Counter()
    schema6_battle_start: Counter[str] = Counter()
    rows = 0

    for row in iter_json_lines(path):
        rows += 1
        type_name = battle_type(row)
        schemas[int(row.get("schema", 1))] += 1
        battle_types[type_name] += 1
        if row.get("shardIndex") is not None:
            shard_rows[int(row["shardIndex"])] += 1
        setup_groups[setup_key(row)] += 1
        shard_groups[shard_setup_key(row)] += 1
        town = row.get("defendedTown") or {}
        if type_name.startswith("town") and isinstance(town, dict):
            if town.get("fortLevel") is not None:
                town_fort_levels[int(town["fortLevel"])] += 1
            town_hero_sources[str(town.get("defendingHeroSource", "missing"))] += 1
            town_has_fortifications[str(bool(row.get("hasFortifications"))).lower()] += 1
        if int(row.get("schema", 1)) >= 6:
            schema6_battle_start["rows"] += 1
            stacks = row.get("battleStartStacks")
            obstacles = row.get("battleStartObstacles")
            wall_state = row.get("battleStartWallState")
            initial_wall_state = row.get("initialWallState")
            if isinstance(stacks, list):
                schema6_battle_start["stacks_array"] += 1
                schema6_battle_start["stack_count"] += len(stacks)
                schema6_battle_start["nonempty_stacks"] += int(bool(stacks))
                schema6_battle_start["attacker_stack_rows"] += int(has_stack_side(stacks, 0))
                schema6_battle_start["defender_stack_rows"] += int(has_stack_side(stacks, 1))
                schema6_battle_start["turret_rows"] += int(any(isinstance(stack, dict) and stack.get("turret") for stack in stacks))
            if isinstance(obstacles, list):
                schema6_battle_start["obstacles_array"] += 1
                schema6_battle_start["obstacle_count"] += len(obstacles)
                schema6_battle_start["nonempty_obstacles"] += int(bool(obstacles))
            if isinstance(wall_state, dict):
                schema6_battle_start["wall_state_rows"] += 1
                wall_parts = ["keep", "bottomTower", "bottomWall", "belowGate", "overGate", "upperWall", "upperTower", "gate", "gateState"]
                schema6_battle_start["wall_changed_rows"] += int(
                    any(wall_part(initial_wall_state, part) != wall_part(wall_state, part) for part in wall_parts)
                )
                schema6_battle_start["gate_changed_rows"] += int(
                    wall_part(initial_wall_state, "gate") != wall_part(wall_state, "gate")
                    or wall_part(initial_wall_state, "gateState") != wall_part(wall_state, "gateState")
                )

    active_groups = shard_groups if group_key == "shard" else setup_groups

    return {
        "group_key": group_key,
        "rows": rows,
        "schemas": dict(sorted(schemas.items())),
        "battle_types": dict(sorted(battle_types.items())),
        "shards": len(shard_rows),
        "shard_rows_min": min(shard_rows.values()) if shard_rows else 0,
        "shard_rows_max": max(shard_rows.values()) if shard_rows else 0,
        "shard_rows": dict(sorted(shard_rows.items())),
        "groups": len(active_groups),
        "group_rows_min": min(active_groups.values()) if active_groups else 0,
        "group_rows_max": max(active_groups.values()) if active_groups else 0,
        "setup_groups": len(setup_groups),
        "setup_group_rows_min": min(setup_groups.values()) if setup_groups else 0,
        "setup_group_rows_max": max(setup_groups.values()) if setup_groups else 0,
        "shard_groups": len(shard_groups),
        "shard_group_rows_min": min(shard_groups.values()) if shard_groups else 0,
        "shard_group_rows_max": max(shard_groups.values()) if shard_groups else 0,
        "town_fort_levels": dict(sorted(town_fort_levels.items())),
        "town_hero_sources": dict(sorted(town_hero_sources.items())),
        "town_has_fortifications": dict(sorted(town_has_fortifications.items())),
        "schema6_battle_start": dict(sorted(schema6_battle_start.items())),
    }


def validate(args: argparse.Namespace, summary: dict[str, Any], logs: LogScan, rich_fields: dict[str, Any] | None) -> list[str]:
    errors = []
    required_types = parse_required_types(args.require_battle_types)
    min_schema6_counters, counter_errors = parse_min_counters(args.min_schema6_battle_start_counter)
    errors.extend(counter_errors)

    if args.expected_rows is not None and summary["rows"] != args.expected_rows:
        errors.append(f"expected {args.expected_rows} rows, got {summary['rows']}")

    if args.expected_schema is not None and summary["schemas"] != {args.expected_schema: summary["rows"]}:
        errors.append(f"expected only schema {args.expected_schema}, got {summary['schemas']}")

    if args.expected_shards is not None and summary["shards"] != args.expected_shards:
        errors.append(f"expected {args.expected_shards} shards, got {summary['shards']}")

    if args.expected_shard_size is not None:
        if summary["shard_rows_min"] != args.expected_shard_size or summary["shard_rows_max"] != args.expected_shard_size:
            errors.append(
                f"expected shard size {args.expected_shard_size}, got min={summary['shard_rows_min']} max={summary['shard_rows_max']}"
            )

    if args.expected_groups is not None and summary["groups"] != args.expected_groups:
        errors.append(f"expected {args.expected_groups} {summary['group_key']} groups, got {summary['groups']}")

    if args.min_groups is not None and summary["groups"] < args.min_groups:
        errors.append(f"expected at least {args.min_groups} {summary['group_key']} groups, got {summary['groups']}")

    if args.require_complete_shards and args.expected_shard_size is None:
        errors.append("--require-complete-shards needs --expected-shard-size")

    if args.min_setup_groups is not None and summary["setup_groups"] < args.min_setup_groups:
        errors.append(f"expected at least {args.min_setup_groups} setup groups, got {summary['setup_groups']}")

    missing_types = required_types - set(summary["battle_types"])
    if missing_types:
        errors.append(f"missing battle types: {sorted(missing_types)}")

    if args.require_no_mmai_fallback and logs.fallback_lines:
        errors.append(f"found {len(logs.fallback_lines)} MMAI fallback/config-error log lines")

    if args.require_mmai_initialized:
        if logs.files == 0:
            errors.append("no shard logs found for MMAI initialization check")
        elif logs.files_with_mmai_init != logs.files:
            errors.append(f"MMAI initialized in {logs.files_with_mmai_init}/{logs.files} shard logs")

    if args.require_schema3_rich_fields and rich_fields and rich_fields["invalid_rows"] > 0:
        errors.append(f"schema3 rich fields invalid in {rich_fields['invalid_rows']}/{rich_fields['rows_checked']} rows")

    schema6_counters = summary["schema6_battle_start"]
    for name, minimum in sorted(min_schema6_counters.items()):
        actual = int(schema6_counters.get(name, 0))
        if actual < minimum:
            errors.append(f"schema6_battle_start.{name} expected at least {minimum}, got {actual}")

    return errors


def main() -> int:
    args = parse_args()
    summary = load_summary(args.dataset, args.group_key)
    logs = scan_logs(args.dataset)
    rich_fields = validate_schema3_rich_fields(args.dataset) if args.require_schema3_rich_fields else None
    errors = validate(args, summary, logs, rich_fields)

    output = {
        **summary,
        "log_files": logs.files,
        "log_files_with_mmai_init": logs.files_with_mmai_init,
        "mmai_fallback_lines": len(logs.fallback_lines),
        "schema3_rich_fields": rich_fields,
        "ok": not errors,
        "errors": errors,
    }

    if args.json:
        print(json.dumps(output, sort_keys=True))
    else:
        print(
            f"rows={summary['rows']} schemas={summary['schemas']} "
            f"battle_types={summary['battle_types']} shards={summary['shards']} "
            f"shard_rows={summary['shard_rows_min']}..{summary['shard_rows_max']} "
            f"group_key={summary['group_key']} groups={summary['groups']} "
            f"group_rows={summary['group_rows_min']}..{summary['group_rows_max']} "
            f"setup_groups={summary['setup_groups']} shard_groups={summary['shard_groups']} "
            f"logs={logs.files} "
            f"mmai_init_logs={logs.files_with_mmai_init} mmai_fallback_lines={len(logs.fallback_lines)}"
        )
        if summary["town_fort_levels"]:
            print(
                f"town_fort_levels={summary['town_fort_levels']} "
                f"town_hero_sources={summary['town_hero_sources']} "
                f"town_has_fortifications={summary['town_has_fortifications']}"
            )
        if summary["schema6_battle_start"]:
            print(f"schema6_battle_start={summary['schema6_battle_start']}")
        if rich_fields:
            print(
                f"schema3_rich_fields rows_checked={rich_fields['rows_checked']} "
                f"invalid_rows={rich_fields['invalid_rows']}"
            )
            if rich_fields["examples"]:
                print("schema3 rich-field examples:")
                for example in rich_fields["examples"]:
                    print(f"  {example}")
        if logs.fallback_lines and args.print_fallback_lines > 0:
            print("fallback/config-error lines:")
            for line in logs.fallback_lines[: args.print_fallback_lines]:
                print(f"  {line}")
        if errors:
            print("FAIL:")
            for error in errors:
                print(f"  {error}")
        else:
            print("OK")

    return 0 if not errors else 2


if __name__ == "__main__":
    raise SystemExit(main())
