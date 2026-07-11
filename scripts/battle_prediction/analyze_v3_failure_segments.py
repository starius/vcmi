#!/usr/bin/env python3
"""Segment battle predictor errors by interpretable battle features."""

from __future__ import annotations

import argparse
import hashlib
import math
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Any

from evaluate_nullkiller_predictor import (
    SAFE_ATTACK_RATIO,
    V3_SAFE_PROBABILITY,
    army_rich_stats,
    battle_type,
    cxx_v3_probability,
    cxx_v3_static_calibration_applies,
    deployed_safe_prediction,
    hero_strength,
    load_groups,
)


@dataclass
class SegmentStats:
    groups: int = 0
    rows: int = 0
    weighted_abs_error: float = 0.0
    weighted_brier: float = 0.0
    correct50: int = 0
    false_safe: int = 0
    false_unsafe: int = 0
    actual_sum: float = 0.0
    predicted_sum: float = 0.0

    def add(self, count: int, actual: float, predicted: float, safe_probability: float, actual_safe_probability: float) -> None:
        self.groups += 1
        self.rows += count
        self.weighted_abs_error += count * abs(predicted - actual)
        self.weighted_brier += count * (predicted - actual) ** 2
        self.correct50 += count * int((predicted >= 0.5) == (actual >= 0.5))
        self.false_safe += int(predicted >= safe_probability and actual < actual_safe_probability)
        self.false_unsafe += int(predicted < safe_probability and actual >= actual_safe_probability)
        self.actual_sum += count * actual
        self.predicted_sum += count * predicted

    def score(self) -> float:
        return self.weighted_abs_error / self.rows if self.rows else 0.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--min-group-size", type=int, default=1)
    parser.add_argument("--min-segment-groups", type=int, default=3)
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--predictor", choices=["cxx-v3", "deployed-danger"], default="cxx-v3")
    parser.add_argument("--safe-probability", type=float, default=V3_SAFE_PROBABILITY)
    parser.add_argument("--safe-ratio", type=float, default=SAFE_ATTACK_RATIO)
    parser.add_argument("--town-danger-factor", type=float, default=1.0)
    parser.add_argument("--actual-safe-probability", type=float, default=0.95)
    parser.add_argument("--actual-min", type=float, default=0.0, help="Only include groups with empirical win rate at least this value")
    parser.add_argument("--actual-max", type=float, default=1.0, help="Only include groups with empirical win rate at most this value")
    parser.add_argument("--prediction-min", type=float, default=0.0, help="Only include groups with predictor score at least this value")
    parser.add_argument("--prediction-max", type=float, default=1.0, help="Only include groups with predictor score at most this value")
    parser.add_argument("--error-min", type=float, default=0.0, help="Only include groups where abs(predictor score - empirical win rate) is at least this value")
    parser.add_argument("--print-groups", type=int, default=0, help="Print N included groups with largest absolute predictor error")
    parser.add_argument(
        "--sort",
        choices=["mae", "brier", "false-safe", "false-unsafe", "accuracy"],
        default="mae",
        help="Segment ranking metric",
    )
    parser.add_argument(
        "--scope",
        choices=["all", "deployed-static", "town"],
        default="all",
        help="Battle scope to segment. deployed-static matches current Nullkiller2 static v3 usage.",
    )
    return parser.parse_args()


def bucket(value: float, edges: list[float]) -> str:
    previous = "-inf"
    for edge in edges:
        if value < edge:
            return f"[{previous},{edge:g})"
        previous = f"{edge:g}"
    return f"[{previous},inf)"


def hero_field(row: dict[str, Any], side: str, field: str, default: float = 0.0) -> float:
    hero = row.get(f"{side}Hero") or {}
    return float(hero.get(field) if hero.get(field) is not None else default)


def has_spellbook(row: dict[str, Any], side: str) -> bool:
    hero = row.get(f"{side}Hero") or {}
    return bool(hero.get("hasSpellbook"))


def army_strength(row: dict[str, Any], side: str) -> float:
    return max(float(row.get(f"{side}ArmyStrength") or 0.0), 1.0) * hero_strength(row.get(f"{side}Hero"))


def town_feature(row: dict[str, Any], key: str, default: Any = None) -> Any:
    town = row.get("defendedTown") or {}
    return town.get(key, default)


def town_fortification(row: dict[str, Any], key: str, default: Any = None) -> Any:
    town = row.get("defendedTown") or {}
    fortifications = town.get("fortifications") or {}
    return fortifications.get(key, default)


def fort_level(row: dict[str, Any]) -> int:
    town = row.get("defendedTown") or {}
    if town.get("fortLevel") is not None:
        return int(town["fortLevel"])
    fortifications = town.get("fortifications") or {}
    walls = int(fortifications.get("wallsHealth") or 0)
    keep = int(fortifications.get("citadelHealth") or 0)
    towers = int(fortifications.get("upperTowerHealth") or 0) + int(fortifications.get("lowerTowerHealth") or 0)
    if keep > 0 and towers > 0:
        return 3
    if keep > 0:
        return 2
    if walls > 0:
        return 1
    return 0


def rich_diff(row: dict[str, Any], key: str) -> float:
    attacker = army_rich_stats(row, "attacker")
    defender = army_rich_stats(row, "defender")
    return float(attacker.get(key, 0.0) - defender.get(key, 0.0))


def rich_log_ratio(row: dict[str, Any], key: str) -> float:
    attacker = army_rich_stats(row, "attacker")
    defender = army_rich_stats(row, "defender")
    return math.log((float(attacker.get(key, 0.0)) + 1.0) / (float(defender.get(key, 0.0)) + 1.0))


def segments_for(row: dict[str, Any], actual: float, predicted: float) -> list[str]:
    attacker_strength = army_strength(row, "attacker")
    defender_strength = army_strength(row, "defender")
    strength_ratio = attacker_strength / defender_strength if defender_strength > 0 else 999.0
    mana_diff = hero_field(row, "attacker", "mana") - hero_field(row, "defender", "mana")
    combat_spell_diff = hero_field(row, "attacker", "combatSpellCount") - hero_field(row, "defender", "combatSpellCount")
    type_name = battle_type(row)

    result = [
        f"type={type_name}",
        f"actual={bucket(actual, [0.05, 0.25, 0.45, 0.55, 0.75, 0.95])}",
        f"predicted={bucket(predicted, [0.05, 0.25, 0.45, 0.55, 0.75, 0.95])}",
        f"strength_ratio={bucket(strength_ratio, [0.5, 0.8, 1.0, 1.25, 1.75, 2.5, 4.0])}",
        f"battlefield={row.get('battlefield')}",
        f"terrain={row.get('terrain')}",
        f"spellbooks={int(has_spellbook(row, 'attacker'))}:{int(has_spellbook(row, 'defender'))}",
        f"mana_diff={bucket(mana_diff, [-100, -25, 0, 25, 100])}",
        f"combat_spell_diff={bucket(combat_spell_diff, [-5, -1, 1, 5])}",
        f"shooter_share_diff={bucket(rich_diff(row, 'shooter_share'), [-0.5, -0.1, 0.1, 0.5])}",
        f"flying_share_diff={bucket(rich_diff(row, 'flying_share'), [-0.5, -0.1, 0.1, 0.5])}",
        f"no_retaliation_share_diff={bucket(rich_diff(row, 'blocksRetaliation_share'), [-0.5, -0.1, 0.1, 0.5])}",
        f"area_attack_share_diff={bucket(rich_diff(row, 'area_attack_share'), [-0.5, -0.1, 0.1, 0.5])}",
        f"spell_immunity_share_diff={bucket(rich_diff(row, 'spell_immunity_share'), [-0.5, -0.1, 0.1, 0.5])}",
        f"speed_diff={bucket(rich_diff(row, 'speed_avg'), [-5, -2, 0, 2, 5])}",
        f"hp_log_ratio={bucket(rich_log_ratio(row, 'hp'), [-2, -1, 0, 1, 2])}",
        f"damage_log_ratio={bucket(rich_log_ratio(row, 'damage'), [-2, -1, 0, 1, 2])}",
    ]

    if type_name.startswith("town"):
        result.extend(
            [
                f"town_faction={town_feature(row, 'faction')}",
                f"town_fort={fort_level(row)}",
                f"town_mage={town_feature(row, 'mageGuildLevel', 0)}",
                f"town_tavern={int(bool(town_feature(row, 'hasBuiltTavern', False)))}",
                f"town_grail={int(bool(town_feature(row, 'hasBuiltGrail', False)))}",
                f"town_moat={int(bool(town_fortification(row, 'hasMoat', False)))}",
            ]
        )

    return result


def matches_scope(row: dict[str, Any], scope: str) -> bool:
    if scope == "all":
        return True
    if scope == "deployed-static":
        return cxx_v3_static_calibration_applies(row)
    if scope == "town":
        return battle_type(row).startswith("town")
    raise ValueError(f"Unknown scope: {scope}")


def matches_error_window(group: Any, predicted: float, args: argparse.Namespace) -> bool:
    actual = group.win_rate
    return (
        args.actual_min <= actual <= args.actual_max
        and args.prediction_min <= predicted <= args.prediction_max
        and abs(predicted - actual) >= args.error_min
    )


def predict_group(row: dict[str, Any], args: argparse.Namespace) -> float:
    if args.predictor == "cxx-v3":
        return cxx_v3_probability(row)
    if args.predictor == "deployed-danger":
        return 1.0 if deployed_safe_prediction(row, args.safe_ratio, args.town_danger_factor) else 0.0
    raise ValueError(f"Unknown predictor: {args.predictor}")


def hero_summary(hero: dict[str, Any] | None) -> str:
    if not hero:
        return "none"
    secondary = hero.get("secondary") or []
    skills = []
    for entry in secondary[:8]:
        if isinstance(entry, dict):
            skills.append(f"{entry.get('skill')}:{entry.get('level')}")
    combat_spells = hero.get("combatSpells") or []
    primary = hero.get("primary") or []
    return (
        f"type={hero.get('type')} level={hero.get('level')} primary={primary} "
        f"mana={hero.get('mana')}/{hero.get('manaLimit')} "
        f"spells={len(combat_spells)} combat={combat_spells[:8]} "
        f"skills={skills}"
    )


def stack_summary(stack: dict[str, Any]) -> str:
    stats = stack.get("stats") or {}
    flags = []
    for key, label in [
        ("shooter", "shot"),
        ("flying", "fly"),
        ("blocksRetaliation", "no-ret"),
        ("attacksAllAdjacent", "all-adj"),
        ("twoHexAttackBreath", "breath"),
        ("spellcaster", "cast"),
        ("spellAfterAttack", "spell-hit"),
        ("mindImmune", "mind-immune"),
        ("undead", "undead"),
        ("nonLiving", "nonliving"),
        ("blockAllMagic", "no-magic"),
        ("spellSchoolImmunity", "school-immune"),
    ]:
        if stats.get(key):
            flags.append(label)
    return (
        f"slot={stack.get('slot')} cr={stack.get('creature')} count={stack.get('count')} "
        f"power={float(stack.get('power') or 0.0):.1f} "
        f"a/d={stats.get('attack')}/{stats.get('defense')} "
        f"dmg={stats.get('damageMin')}-{stats.get('damageMax')} "
        f"hp={stats.get('hitPoints')} spd={stats.get('speed')} "
        f"flags={','.join(flags) if flags else '-'}"
    )


def army_summary(row: dict[str, Any], side: str, limit: int = 4) -> list[str]:
    army = row.get(f"{side}Army") or []
    sorted_army = sorted(army, key=lambda stack: float(stack.get("power") or 0.0), reverse=True)
    return [stack_summary(stack) for stack in sorted_army[:limit]]


def town_summary(row: dict[str, Any]) -> str:
    town = row.get("defendedTown")
    if not isinstance(town, dict):
        return "none"
    fortifications = town.get("fortifications") or {}
    return (
        f"faction={town.get('faction')} fort={town.get('fortLevel')} "
        f"mage={town.get('mageGuildLevel')} tavern={town.get('hasBuiltTavern')} "
        f"grail={town.get('hasBuiltGrail')} source={town.get('defendingHeroSource')} "
        f"visiting={town.get('hasVisitingHero')} garrison={town.get('hasGarrisonHero')} "
        f"walls={fortifications.get('wallsHealth')} keep={fortifications.get('citadelHealth')} "
        f"towers={fortifications.get('upperTowerHealth')}/{fortifications.get('lowerTowerHealth')} "
        f"moat={fortifications.get('hasMoat')}"
    )


def print_group_detail(index: int, group: Any, predicted: float) -> None:
    row = group.row
    attacker_strength = army_strength(row, "attacker")
    defender_strength = army_strength(row, "defender")
    strength_ratio = attacker_strength / defender_strength if defender_strength > 0 else 999.0
    setup_hash = hashlib.sha256(group.key.encode("utf-8")).hexdigest()[:12]
    print(
        f"group {index}: type={battle_type(row)} count={group.count} "
        f"actual={group.win_rate:.4f} predicted={predicted:.4f} "
        f"error={abs(predicted - group.win_rate):.4f} ratio={strength_ratio:.4f} "
        f"battlefield={row.get('battlefield')} terrain={row.get('terrain')} setup={setup_hash}"
    )
    print(f"  attacker hero: {hero_summary(row.get('attackerHero'))}")
    print(f"  defender hero: {hero_summary(row.get('defenderHero'))}")
    if battle_type(row).startswith("town"):
        print(f"  town: {town_summary(row)}")
    for side in ("attacker", "defender"):
        print(f"  {side} army:")
        for line in army_summary(row, side):
            print(f"    {line}")


def print_segment(name: str, stats: SegmentStats) -> None:
    rows = max(stats.rows, 1)
    print(
        f"{name} groups={stats.groups} rows={stats.rows} "
        f"mae={stats.weighted_abs_error / rows:.4f} "
        f"brier={stats.weighted_brier / rows:.4f} "
        f"accuracy50={stats.correct50 / rows:.4f} "
        f"actual_avg={stats.actual_sum / rows:.4f} "
        f"predicted_avg={stats.predicted_sum / rows:.4f} "
        f"false_safe={stats.false_safe} false_unsafe={stats.false_unsafe}"
    )


def segment_sort_key(stats: SegmentStats, sort: str) -> tuple[float, int]:
    rows = max(stats.rows, 1)
    if sort == "mae":
        return (stats.weighted_abs_error / rows, stats.groups)
    if sort == "brier":
        return (stats.weighted_brier / rows, stats.groups)
    if sort == "false-safe":
        return (stats.false_safe / stats.groups if stats.groups else 0.0, stats.false_safe)
    if sort == "false-unsafe":
        return (stats.false_unsafe / stats.groups if stats.groups else 0.0, stats.false_unsafe)
    if sort == "accuracy":
        return (1.0 - stats.correct50 / rows, stats.groups)
    raise ValueError(f"Unknown sort mode: {sort}")


def main() -> int:
    args = parse_args()
    groups, schema_counts, rows = load_groups(args.dataset)
    groups = [
        group
        for group in groups
        if group.count >= args.min_group_size and matches_scope(group.row, args.scope)
    ]
    segment_stats: dict[str, SegmentStats] = defaultdict(SegmentStats)
    error_direction: Counter[str] = Counter()
    included_group_details: list[tuple[float, Any, float]] = []
    included_groups = 0
    included_rows = 0

    for group in groups:
        actual = group.win_rate
        predicted = predict_group(group.row, args)
        if not matches_error_window(group, predicted, args):
            continue

        included_groups += 1
        included_rows += group.count
        included_group_details.append((abs(predicted - actual), group, predicted))

        if predicted >= args.safe_probability and actual < args.actual_safe_probability:
            error_direction["false_safe"] += 1
        if predicted < args.safe_probability and actual >= args.actual_safe_probability:
            error_direction["false_unsafe"] += 1

        for segment in segments_for(group.row, actual, predicted):
            segment_stats[segment].add(group.count, actual, predicted, args.safe_probability, args.actual_safe_probability)

    candidates = [
        (segment, stats)
        for segment, stats in segment_stats.items()
        if stats.groups >= args.min_segment_groups
    ]
    candidates.sort(key=lambda item: segment_sort_key(item[1], args.sort), reverse=True)

    scoped_rows = sum(group.count for group in groups)
    print(f"rows={rows} scoped_rows={scoped_rows} schemas={dict(sorted(schema_counts.items()))} groups={len(groups)} scope={args.scope}")
    print(
        f"predictor={args.predictor} safe_probability={args.safe_probability:.4f} "
        f"safe_ratio={args.safe_ratio:.4f} town_danger_factor={args.town_danger_factor:.4f}"
    )
    print(
        "filter "
        f"actual=[{args.actual_min:.4f},{args.actual_max:.4f}] "
        f"prediction=[{args.prediction_min:.4f},{args.prediction_max:.4f}] "
        f"error_min={args.error_min:.4f} "
        f"included_groups={included_groups} included_rows={included_rows}"
    )
    print(f"{args.predictor} false_safe={error_direction['false_safe']} false_unsafe={error_direction['false_unsafe']}")
    print(f"worst segments by {args.sort}:")
    for segment, stats in candidates[: args.top]:
        print_segment(segment, stats)
    if args.print_groups > 0:
        included_group_details.sort(key=lambda item: (item[0], item[1].count), reverse=True)
        print("worst included groups:")
        for index, (_, group, predicted) in enumerate(included_group_details[: args.print_groups], start=1):
            print_group_detail(index, group, predicted)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
