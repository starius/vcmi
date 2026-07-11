#!/usr/bin/env python3
"""Evaluate and fit Nullkiller2 battle outcome heuristics from JSONL battle runs."""

from __future__ import annotations

import argparse
import glob
import gzip
import hashlib
import json
import math
import os
import statistics
import tarfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Any, Callable, Iterable


SAFE_ATTACK_RATIO = 1.1
EPSILON = 1e-12
V3_SAFE_PROBABILITY = 0.60
V3_LOG_STRENGTH_RATIO = (2.62135643331, 0.446922572487, 0.815917259918)
V3_INTERCEPT = 1.19022780253
V3_FEATURES = [
    (0.468810332299, 0.494545454545, 0.499970247049),
    (1.45543395361, 10.192502792, 0.772118513706),
    (-1.44810294879, 10.08671394, 0.744063816548),
    (-1.13950296625, 0.424568267977, 0.700102370093),
    (-0.403380522643, -0.273682343684, 0.397677129925),
    (0.336767948616, 5.80727272727, 8.74461610042),
    (0.244887696405, 5.01454545455, 9.47416426023),
    (0.318006419965, 5.30181818182, 9.13095615643),
    (0.0688971253683, 4.37454545455, 9.22061362939),
    (-0.468810332299, 0.505454545455, 0.499970247049),
    (0.442212589111, 0.505317851301, 0.496692983083),
    (0.211822037923, 44.56, 93.8397039444),
    (-0.288005257378, 44.5636363636, 94.0816209733),
    (-0.163787797423, 0.581818181818, 0.493260362409),
    (-0.042127370793, 0.258181818182, 0.43763451297),
    (-0.39778987901, 0.323636363636, 0.682765402639),
    (0.0305726479297, 0.341133720491, 0.450081992853),
]


@dataclass
class Group:
    key: str
    row: dict[str, Any]
    count: int = 0
    attacker_wins: int = 0
    no_winner: int = 0
    attacker_loss_ratio_sum: float = 0.0

    @property
    def win_rate(self) -> float:
        return self.attacker_wins / self.count if self.count else 0.0

    @property
    def attacker_loss_ratio(self) -> float:
        return self.attacker_loss_ratio_sum / self.count if self.count else 0.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--safe-ratio", type=float, default=SAFE_ATTACK_RATIO)
    parser.add_argument(
        "--group-key",
        choices=["setup", "shard"],
        default="setup",
        help="Group repeated rows by full setup features or by generated shard metadata. Use shard for generated repeated-simulation datasets.",
    )
    parser.add_argument("--min-group-size", type=int, default=1)
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--epochs", type=int, default=2500)
    parser.add_argument("--learning-rate", type=float, default=0.05)
    parser.add_argument("--l2", type=float, default=0.001)
    parser.add_argument("--composition-min-weight", type=int, default=1, help="Minimum weighted rows for a creature to enter the composition model")
    parser.add_argument("--skill-spell-min-weight", type=int, default=1, help="Minimum weighted rows for a skill or spell to enter the skill/spell model")
    parser.add_argument("--print-near-even", type=int, default=0, help="Print N grouped setups with empirical win rate closest to 50%%")
    parser.add_argument("--print-worst", type=int, default=0, help="Print N grouped setups with largest fitted-model probability error")
    parser.add_argument(
        "--scope",
        choices=["all", "deployed-static", "town"],
        default="all",
        help="Battle scope to evaluate. deployed-static matches current Nullkiller2 static v3 usage.",
    )
    parser.add_argument("--print-v3-false-safe", type=int, default=0, help="Print N cxx-v3 groups predicted safe with empirical win rate below 95%%")
    parser.add_argument("--print-v3-false-unsafe", type=int, default=0, help="Print N cxx-v3 groups predicted unsafe with empirical win rate at least 95%%")
    parser.add_argument("--print-town-deployable-false-safe", type=int, default=0, help="Print N town-deployable groups predicted safe with empirical win rate below 95%%")
    parser.add_argument("--print-town-deployable-false-unsafe", type=int, default=0, help="Print N town-deployable groups predicted unsafe with empirical win rate at least 95%%")
    parser.add_argument("--town-deployable-safe-probability", type=float, default=V3_SAFE_PROBABILITY, help="Safety probability threshold for town-deployable false-safe diagnostics")
    parser.add_argument("--summary-only", action="store_true", help="Skip fitted coefficient, threshold, and group detail output")
    parser.add_argument(
        "--town-danger-factors",
        default="",
        help="Comma-separated extra multipliers for deployed town danger safety diagnostics",
    )
    parser.add_argument("--deployed-danger-only", action="store_true", help="Skip fitted models and print only baseline/deployed danger summaries")
    parser.add_argument("--json", action="store_true", help="Print machine-readable metrics")
    return parser.parse_args()


def parse_float_list(value: str) -> list[float]:
    result = []
    for part in value.split(","):
        if not part.strip():
            continue
        result.append(float(part))
    return result


def iter_json_lines(path: str) -> Iterable[dict[str, Any]]:
    if os.path.isdir(path):
        files = sorted(glob.glob(os.path.join(path, "shard-*.jsonl")))
        for file_name in files:
            with open(file_name, encoding="utf-8") as handle:
                for line in handle:
                    if line.strip():
                        yield json.loads(line)
        return

    if path.endswith(".tar.gz") or path.endswith(".tgz"):
        with tarfile.open(path, "r:gz") as archive:
            members = sorted(
                (member for member in archive.getmembers() if os.path.basename(member.name).startswith("shard-") and member.name.endswith(".jsonl")),
                key=lambda member: member.name,
            )
            for member in members:
                extracted = archive.extractfile(member)
                if extracted is None:
                    continue
                for raw in extracted:
                    line = raw.decode("utf-8")
                    if line.strip():
                        yield json.loads(line)
        return

    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)


def hero_fighting_strength(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 1.0
    if "fightingStrength" in hero:
        return float(hero["fightingStrength"])
    primary = hero.get("primary") or [0, 0, 0, 0]
    return math.sqrt((1.0 + 0.05 * primary[0]) * (1.0 + 0.05 * primary[1]))


def hero_strength(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 1.0
    if "heroStrength" in hero:
        return float(hero["heroStrength"])
    return hero_fighting_strength(hero)


def battle_type(row: dict[str, Any]) -> str:
    if row.get("battleType"):
        return str(row["battleType"])
    if row.get("defendedTown"):
        return "town-hero" if row.get("defenderHero") else "town"
    return "hero-hero" if row.get("defenderHero") else "hero-monster"


def side_strength(row: dict[str, Any], side: str) -> float:
    return float(row[f"{side}ArmyStrength"]) * hero_strength(row.get(f"{side}Hero"))


def army_stats(row: dict[str, Any], side: str) -> dict[str, float]:
    army = row.get(f"{side}Army") or []
    total = max(float(row.get(f"{side}ArmyStrength") or 0), 1.0)
    powers = [float(stack.get("power") or 0) for stack in army]
    max_share = max(powers) / total if powers else 0.0
    return {
        "stacks": float(len(army)),
        "max_share": max_share,
    }


def army_rich_stats(row: dict[str, Any], side: str) -> dict[str, float]:
    army = row.get(f"{side}Army") or []
    total_power = max(float(row.get(f"{side}ArmyStrength") or 0), 1.0)
    result = defaultdict(float)

    for stack in army:
        stats = stack.get("stats") or {}
        if not stats:
            continue

        result["available"] = 1.0
        count = float(stack.get("count") or 0.0)
        power = float(stack.get("power") or 0.0)
        hit_points = float(stats.get("hitPoints") or 0.0)
        damage_min = float(stats.get("damageMin") or 0.0)
        damage_max = float(stats.get("damageMax") or 0.0)
        average_damage = (damage_min + damage_max) / 2.0

        result["hp"] += count * hit_points
        result["damage"] += count * average_damage
        result["attack_weighted"] += power * float(stats.get("attack") or 0.0)
        result["defense_weighted"] += power * float(stats.get("defense") or 0.0)
        result["speed_weighted"] += power * float(stats.get("speed") or 0.0)
        result["max_speed"] = max(result["max_speed"], float(stats.get("speed") or 0.0))
        result["magic_resistance_weighted"] += power * float(stats.get("magicResistance") or 0.0)
        result["level_spell_immunity_weighted"] += power * float(stats.get("levelSpellImmunity") or 0.0)
        result["spell_damage_reduction_weighted"] += power * float(stats.get("spellDamageReduction") or 0.0)

        for key in [
            "shooter",
            "flying",
            "blocksRetaliation",
            "unlimitedRetaliations",
            "returnAfterStrike",
            "twoHexAttackBreath",
            "attacksAllAdjacent",
            "threeHeadedAttack",
            "spellAfterAttack",
            "spellcaster",
            "mindImmune",
            "undead",
            "nonLiving",
            "blockAllMagic",
            "spellSchoolImmunity",
        ]:
            if stats.get(key):
                result[f"{key}_power"] += power

        if float(stats.get("additionalAttack") or 0.0) > 0.0:
            result["additionalAttack_power"] += power
        if float(stats.get("additionalRetaliation") or 0.0) > 0.0:
            result["additionalRetaliation_power"] += power

    result["attack_avg"] = result["attack_weighted"] / total_power
    result["defense_avg"] = result["defense_weighted"] / total_power
    result["speed_avg"] = result["speed_weighted"] / total_power
    result["magic_resistance_avg"] = result["magic_resistance_weighted"] / total_power
    result["level_spell_immunity_avg"] = result["level_spell_immunity_weighted"] / total_power
    result["spell_damage_reduction_avg"] = result["spell_damage_reduction_weighted"] / total_power

    for key in [
        "shooter",
        "flying",
        "blocksRetaliation",
        "unlimitedRetaliations",
        "returnAfterStrike",
        "twoHexAttackBreath",
        "attacksAllAdjacent",
        "threeHeadedAttack",
        "spellAfterAttack",
        "spellcaster",
        "mindImmune",
        "undead",
        "nonLiving",
        "blockAllMagic",
        "spellSchoolImmunity",
        "additionalAttack",
        "additionalRetaliation",
    ]:
        result[f"{key}_share"] = result[f"{key}_power"] / total_power

    result["area_attack_share"] = max(
        result["twoHexAttackBreath_share"],
        result["attacksAllAdjacent_share"],
        result["threeHeadedAttack_share"],
    )
    result["spell_immunity_share"] = max(
        result["mindImmune_share"],
        result["blockAllMagic_share"],
        result["spellSchoolImmunity_share"],
    )
    result["non_living_or_undead_share"] = max(result["undead_share"], result["nonLiving_share"])
    return result


def mana_ratio(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    limit = float(hero.get("manaLimit") or 0)
    if limit <= 0:
        return 0.0
    return float(hero.get("mana") or 0) / limit


def raw_mana(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    return float(hero.get("mana") or 0)


def raw_mana_limit(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    return float(hero.get("manaLimit") or 0)


def combat_spell_count(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    if "combatSpells" in hero:
        return float(len(hero.get("combatSpells") or []))
    return float(hero.get("combatSpellCount") or 0)


def secondary_skill_count(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    return float(len(hero.get("secondary") or []))


def hero_level(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    return float(hero.get("level") or 0)


def primary(hero: dict[str, Any] | None, index: int) -> float:
    if not hero:
        return 0.0
    values = hero.get("primary") or [0, 0, 0, 0]
    return float(values[index])


def town_feature(row: dict[str, Any], key: str, default: float = 0.0) -> float:
    town = row.get("defendedTown") or {}
    return float(town.get(key) if town.get(key) is not None else default)


def town_bool(row: dict[str, Any], key: str) -> float:
    town = row.get("defendedTown") or {}
    return 1.0 if town.get(key) else 0.0


def town_fortification(row: dict[str, Any], key: str) -> float:
    town = row.get("defendedTown") or {}
    fortifications = town.get("fortifications") or {}
    return float(fortifications.get(key) or 0.0)


def town_damage_midpoint(row: dict[str, Any], key: str) -> float:
    town = row.get("defendedTown") or {}
    damage = town.get(key) or {}
    return (float(damage.get("min") or 0.0) + float(damage.get("max") or 0.0)) / 2.0


def town_initial_wall_state(row: dict[str, Any], key: str) -> float:
    wall_state = row.get("initialWallState")
    if isinstance(wall_state, dict) and wall_state.get(key) is not None:
        return float(wall_state.get(key) or 0.0)

    # Schema 3 did not persist the pre-battle wall state. Derive the same coarse
    # planner-visible state from fortifications so older datasets remain usable.
    if key == "keep":
        return town_fortification(row, "citadelHealth")
    if key in ("bottomTower", "upperTower"):
        return town_fortification(row, "lowerTowerHealth" if key == "bottomTower" else "upperTowerHealth")
    if key == "gate":
        return 2.0 if town_feature(row, "fortLevel") > 0.0 else 0.0
    if key == "gateState":
        return 1.0 if town_feature(row, "fortLevel") > 0.0 else 0.0
    if key in ("bottomWall", "belowGate", "overGate", "upperWall"):
        return town_fortification(row, "wallsHealth")
    return 0.0


def town_initial_wall_total(row: dict[str, Any]) -> float:
    return sum(town_initial_wall_state(row, key) for key in ("bottomWall", "belowGate", "overGate", "upperWall"))


def town_pre_merge_state(row: dict[str, Any]) -> dict[str, Any] | None:
    pre_merge = row.get("townPreMergeState")
    return pre_merge if isinstance(pre_merge, dict) else None


def town_pre_merge_army(row: dict[str, Any], key: str) -> dict[str, Any]:
    pre_merge = town_pre_merge_state(row)
    if not pre_merge:
        return {}
    army = pre_merge.get(key)
    return army if isinstance(army, dict) else {}


def town_pre_merge_army_strength(row: dict[str, Any], key: str) -> float:
    return float(town_pre_merge_army(row, key).get("armyStrength") or 0.0)


def town_pre_merge_stack_count(row: dict[str, Any], key: str) -> float:
    stacks = town_pre_merge_army(row, key).get("stacks") or []
    return float(len(stacks)) if isinstance(stacks, list) else 0.0


def town_pre_merge_largest_share(row: dict[str, Any], key: str) -> float:
    army = town_pre_merge_army(row, key)
    total = max(float(army.get("armyStrength") or 0.0), 1.0)
    stacks = army.get("stacks") or []
    if not isinstance(stacks, list):
        return 0.0
    powers = [float(stack.get("power") or 0.0) for stack in stacks if isinstance(stack, dict)]
    return max(powers) / total if powers else 0.0


def feature_vector(row: dict[str, Any]) -> list[float]:
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    attacker_army = max(float(row.get("attackerArmyStrength") or 0), 1.0)
    defender_army = max(float(row.get("defenderArmyStrength") or 0), 1.0)
    attacker_stats = army_stats(row, "attacker")
    defender_stats = army_stats(row, "defender")
    attacker_rich = army_rich_stats(row, "attacker")
    defender_rich = army_rich_stats(row, "defender")
    attacker_hero = row.get("attackerHero")
    defender_hero = row.get("defenderHero")

    return [
        math.log(attacker / defender),
        1.0 if defender_hero else 0.0,
        math.log(attacker_army),
        math.log(defender_army),
        math.log((attacker_stats["stacks"] + 1.0) / (defender_stats["stacks"] + 1.0)),
        attacker_stats["max_share"] - defender_stats["max_share"],
        primary(attacker_hero, 0) - primary(defender_hero, 0),
        primary(attacker_hero, 1) - primary(defender_hero, 1),
        primary(attacker_hero, 2) - primary(defender_hero, 2),
        primary(attacker_hero, 3) - primary(defender_hero, 3),
        mana_ratio(attacker_hero) - mana_ratio(defender_hero),
        math.log1p(raw_mana(attacker_hero)) - math.log1p(raw_mana(defender_hero)),
        math.log1p(raw_mana_limit(attacker_hero)) - math.log1p(raw_mana_limit(defender_hero)),
        combat_spell_count(attacker_hero) - combat_spell_count(defender_hero),
        secondary_skill_count(attacker_hero) - secondary_skill_count(defender_hero),
        1.0 if attacker_hero and attacker_hero.get("hasSpellbook") else 0.0,
        1.0 if defender_hero and defender_hero.get("hasSpellbook") else 0.0,
        1.0 if row.get("defendedTown") else 0.0,
        town_feature(row, "fortLevel"),
        town_feature(row, "mageGuildLevel"),
        town_bool(row, "hasBuiltTavern"),
        town_bool(row, "hasBuiltGrail"),
        float(len((row.get("defendedTown") or {}).get("buildings") or [])),
        town_fortification(row, "wallsHealth"),
        town_fortification(row, "citadelHealth"),
        town_fortification(row, "upperTowerHealth") + town_fortification(row, "lowerTowerHealth"),
        town_fortification(row, "hasMoat"),
        math.log1p(town_damage_midpoint(row, "keepDamage")),
        math.log1p(town_damage_midpoint(row, "towerDamage")),
        min(attacker_rich["available"], defender_rich["available"]),
        math.log1p(attacker_rich["hp"]) - math.log1p(defender_rich["hp"]),
        math.log1p(attacker_rich["damage"]) - math.log1p(defender_rich["damage"]),
        math.log((attacker_rich["damage"] + 1.0) / (attacker_rich["hp"] + 1.0))
        - math.log((defender_rich["damage"] + 1.0) / (defender_rich["hp"] + 1.0)),
        attacker_rich["attack_avg"] - defender_rich["attack_avg"],
        attacker_rich["defense_avg"] - defender_rich["defense_avg"],
        attacker_rich["speed_avg"] - defender_rich["speed_avg"],
        attacker_rich["max_speed"] - defender_rich["max_speed"],
        attacker_rich["shooter_share"] - defender_rich["shooter_share"],
        attacker_rich["flying_share"] - defender_rich["flying_share"],
        attacker_rich["blocksRetaliation_share"] - defender_rich["blocksRetaliation_share"],
        attacker_rich["unlimitedRetaliations_share"] - defender_rich["unlimitedRetaliations_share"],
        attacker_rich["additionalAttack_share"] - defender_rich["additionalAttack_share"],
        attacker_rich["returnAfterStrike_share"] - defender_rich["returnAfterStrike_share"],
        attacker_rich["area_attack_share"] - defender_rich["area_attack_share"],
        attacker_rich["spellAfterAttack_share"] - defender_rich["spellAfterAttack_share"],
        attacker_rich["spellcaster_share"] - defender_rich["spellcaster_share"],
        attacker_rich["magic_resistance_avg"] - defender_rich["magic_resistance_avg"],
        attacker_rich["level_spell_immunity_avg"] - defender_rich["level_spell_immunity_avg"],
        attacker_rich["spell_damage_reduction_avg"] - defender_rich["spell_damage_reduction_avg"],
        attacker_rich["spell_immunity_share"] - defender_rich["spell_immunity_share"],
        attacker_rich["non_living_or_undead_share"] - defender_rich["non_living_or_undead_share"],
    ]


def ratio_feature_vector(row: dict[str, Any]) -> list[float]:
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    return [math.log(attacker / defender)]


def scaled_cpp_feature(value: float, feature: tuple[float, float, float]) -> float:
    coefficient, mean, scale = feature
    return coefficient * ((value - mean) / scale)


def cxx_v3_feature_values(row: dict[str, Any]) -> tuple[float, list[float]]:
    attacker_hero = row.get("attackerHero")
    defender_hero = row.get("defenderHero")
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    attacker_army = max(float(row.get("attackerArmyStrength") or 0), 1.0)
    defender_army = max(float(row.get("defenderArmyStrength") or 0), 1.0)
    attacker_stats = army_stats(row, "attacker")
    defender_stats = army_stats(row, "defender")

    values = [
        1.0 if defender_hero else 0.0,
        math.log(attacker_army),
        math.log(defender_army),
        math.log((attacker_stats["stacks"] + 1.0) / (defender_stats["stacks"] + 1.0)),
        attacker_stats["max_share"] - defender_stats["max_share"],
        primary(attacker_hero, 0) - primary(defender_hero, 0),
        primary(attacker_hero, 1) - primary(defender_hero, 1),
        primary(attacker_hero, 2) - primary(defender_hero, 2),
        primary(attacker_hero, 3) - primary(defender_hero, 3),
        hero_level(attacker_hero) - hero_level(defender_hero),
        mana_ratio(attacker_hero) - mana_ratio(defender_hero),
        raw_mana(attacker_hero) - raw_mana(defender_hero),
        raw_mana_limit(attacker_hero) - raw_mana_limit(defender_hero),
        1.0 if attacker_hero and attacker_hero.get("hasSpellbook") else 0.0,
        1.0 if defender_hero and defender_hero.get("hasSpellbook") else 0.0,
        combat_spell_count(attacker_hero) - combat_spell_count(defender_hero),
        math.log(max(hero_strength(attacker_hero), EPSILON) / max(hero_strength(defender_hero), EPSILON)),
    ]
    return math.log(attacker / defender), values


def cxx_v3_probability(row: dict[str, Any]) -> float:
    log_ratio, values = cxx_v3_feature_values(row)
    score = V3_INTERCEPT
    score += scaled_cpp_feature(log_ratio, V3_LOG_STRENGTH_RATIO)
    for value, feature in zip(values, V3_FEATURES):
        score += scaled_cpp_feature(value, feature)
    return sigmoid(score)


def cxx_v3_static_calibration_applies(row: dict[str, Any]) -> bool:
    return battle_type(row) in ("hero-hero", "hero-monster")


def matches_scope(row: dict[str, Any], scope: str) -> bool:
    if scope == "all":
        return True
    if scope == "deployed-static":
        return cxx_v3_static_calibration_applies(row)
    if scope == "town":
        return battle_type(row).startswith("town")
    raise ValueError(f"Unknown scope: {scope}")


def v3_compatible_feature_vector(row: dict[str, Any]) -> list[float]:
    log_ratio, values = cxx_v3_feature_values(row)
    return [log_ratio] + values


TOWN_FACTION_BUCKETS = 9
TOWN_TERRAIN_BUCKETS = 10
TOWN_BATTLEFIELD_BUCKETS = 24


def town_deployable_feature_vector(row: dict[str, Any]) -> list[float]:
    attacker_strength = max(side_strength(row, "attacker"), EPSILON)
    defender_strength = max(side_strength(row, "defender"), EPSILON)
    deployed_strength = max(deployed_danger(row), EPSILON)
    attacker_army = max(float(row.get("attackerArmyStrength") or 0), 1.0)
    defender_army = max(float(row.get("defenderArmyStrength") or 0), 1.0)
    attacker_stats = army_stats(row, "attacker")
    defender_stats = army_stats(row, "defender")
    attacker_hero = row.get("attackerHero")
    defender_hero = row.get("defenderHero")
    town = row.get("defendedTown") or {}
    source = str(town.get("defendingHeroSource") or "")
    faction = int(town.get("faction") if town.get("faction") is not None else -1)
    terrain = int(row.get("terrain") if row.get("terrain") is not None else -1)
    battlefield = int(row.get("battlefield") if row.get("battlefield") is not None else -1)
    pre_merge_town_army = town_pre_merge_army_strength(row, "townArmy")
    pre_merge_hero_army = town_pre_merge_army_strength(row, "defendingHeroArmy")
    defender_army_denominator = max(defender_army, 1.0)

    values = [
        math.log(attacker_strength / deployed_strength),
        math.log(attacker_strength / defender_strength),
        1.0 if defender_hero else 0.0,
        1.0 if source == "visiting" else 0.0,
        1.0 if source == "garrison" else 0.0,
        math.log(attacker_army),
        math.log(defender_army),
        math.log(max(town_feature(row, "armyStrength"), 0.0) + 1.0),
        1.0 if town_pre_merge_state(row) else 0.0,
        math.log1p(pre_merge_town_army),
        math.log1p(pre_merge_hero_army),
        pre_merge_town_army / defender_army_denominator,
        pre_merge_hero_army / defender_army_denominator,
        town_pre_merge_stack_count(row, "townArmy"),
        town_pre_merge_stack_count(row, "defendingHeroArmy"),
        town_pre_merge_largest_share(row, "townArmy"),
        town_pre_merge_largest_share(row, "defendingHeroArmy"),
        math.log((attacker_stats["stacks"] + 1.0) / (defender_stats["stacks"] + 1.0)),
        attacker_stats["max_share"] - defender_stats["max_share"],
        primary(attacker_hero, 0) - primary(defender_hero, 0),
        primary(attacker_hero, 1) - primary(defender_hero, 1),
        primary(attacker_hero, 2) - primary(defender_hero, 2),
        primary(attacker_hero, 3) - primary(defender_hero, 3),
        hero_level(attacker_hero) - hero_level(defender_hero),
        mana_ratio(attacker_hero) - mana_ratio(defender_hero),
        raw_mana(attacker_hero) - raw_mana(defender_hero),
        raw_mana_limit(attacker_hero) - raw_mana_limit(defender_hero),
        1.0 if attacker_hero and attacker_hero.get("hasSpellbook") else 0.0,
        1.0 if defender_hero and defender_hero.get("hasSpellbook") else 0.0,
        combat_spell_count(attacker_hero) - combat_spell_count(defender_hero),
        secondary_skill_count(attacker_hero) - secondary_skill_count(defender_hero),
        math.log(max(hero_strength(attacker_hero), EPSILON) / max(hero_strength(defender_hero), EPSILON)),
        town_feature(row, "fortLevel"),
        town_feature(row, "mageGuildLevel"),
        town_bool(row, "hasBuiltTavern"),
        town_bool(row, "hasBuiltGrail"),
        float(len(town.get("buildings") or [])),
        town_fortification(row, "wallsHealth"),
        town_fortification(row, "citadelHealth"),
        town_fortification(row, "upperTowerHealth") + town_fortification(row, "lowerTowerHealth"),
        town_fortification(row, "hasMoat"),
        math.log1p(town_damage_midpoint(row, "keepDamage")),
        math.log1p(town_damage_midpoint(row, "towerDamage")),
    ]
    values.extend(1.0 if faction == index else 0.0 for index in range(TOWN_FACTION_BUCKETS))
    values.extend(1.0 if terrain == index else 0.0 for index in range(TOWN_TERRAIN_BUCKETS))
    values.extend(1.0 if battlefield == index else 0.0 for index in range(TOWN_BATTLEFIELD_BUCKETS))
    return values


def town_rich_deployable_feature_vector(row: dict[str, Any]) -> list[float]:
    values = town_deployable_feature_vector(row)
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    attacker_rich = army_rich_stats(row, "attacker")
    defender_rich = army_rich_stats(row, "defender")
    attacker_hero = row.get("attackerHero")
    defender_hero = row.get("defenderHero")

    fort_level = town_feature(row, "fortLevel")
    mage_guild_level = town_feature(row, "mageGuildLevel")
    has_moat = town_fortification(row, "hasMoat")
    wall_total = town_initial_wall_total(row)
    tower_total = town_initial_wall_state(row, "bottomTower") + town_initial_wall_state(row, "upperTower")
    keep_health = town_initial_wall_state(row, "keep")
    gate_health = town_initial_wall_state(row, "gate")
    gate_state = town_initial_wall_state(row, "gateState")
    attacker_flying = attacker_rich["flying_share"]
    attacker_shooter = attacker_rich["shooter_share"]
    attacker_spellcaster = attacker_rich["spellcaster_share"]
    attacker_no_retaliation = attacker_rich["blocksRetaliation_share"]
    defender_shooter = defender_rich["shooter_share"]
    defender_spellcaster = defender_rich["spellcaster_share"]
    defender_magic_resistance = defender_rich["magic_resistance_avg"]
    spell_power_diff = primary(attacker_hero, 2) - primary(defender_hero, 2)
    current_mana_diff = raw_mana(attacker_hero) - raw_mana(defender_hero)
    spell_count_diff = combat_spell_count(attacker_hero) - combat_spell_count(defender_hero)
    log_strength_ratio = math.log(attacker / defender)

    values.extend([
        min(attacker_rich["available"], defender_rich["available"]),
        math.log1p(attacker_rich["hp"]) - math.log1p(defender_rich["hp"]),
        math.log1p(attacker_rich["damage"]) - math.log1p(defender_rich["damage"]),
        math.log((attacker_rich["damage"] + 1.0) / (attacker_rich["hp"] + 1.0))
        - math.log((defender_rich["damage"] + 1.0) / (defender_rich["hp"] + 1.0)),
        attacker_rich["attack_avg"] - defender_rich["attack_avg"],
        attacker_rich["defense_avg"] - defender_rich["defense_avg"],
        attacker_rich["speed_avg"] - defender_rich["speed_avg"],
        attacker_rich["max_speed"] - defender_rich["max_speed"],
        attacker_rich["shooter_share"] - defender_rich["shooter_share"],
        attacker_rich["flying_share"] - defender_rich["flying_share"],
        attacker_rich["blocksRetaliation_share"] - defender_rich["blocksRetaliation_share"],
        attacker_rich["unlimitedRetaliations_share"] - defender_rich["unlimitedRetaliations_share"],
        attacker_rich["additionalAttack_share"] - defender_rich["additionalAttack_share"],
        attacker_rich["returnAfterStrike_share"] - defender_rich["returnAfterStrike_share"],
        attacker_rich["area_attack_share"] - defender_rich["area_attack_share"],
        attacker_rich["spellAfterAttack_share"] - defender_rich["spellAfterAttack_share"],
        attacker_rich["spellcaster_share"] - defender_rich["spellcaster_share"],
        attacker_rich["magic_resistance_avg"] - defender_rich["magic_resistance_avg"],
        attacker_rich["level_spell_immunity_avg"] - defender_rich["level_spell_immunity_avg"],
        attacker_rich["spell_damage_reduction_avg"] - defender_rich["spell_damage_reduction_avg"],
        attacker_rich["spell_immunity_share"] - defender_rich["spell_immunity_share"],
        attacker_rich["non_living_or_undead_share"] - defender_rich["non_living_or_undead_share"],
        wall_total,
        keep_health,
        tower_total,
        gate_health,
        gate_state,
        log_strength_ratio * fort_level,
        log_strength_ratio * has_moat,
        attacker_flying * fort_level,
        attacker_flying * has_moat,
        attacker_flying * wall_total,
        attacker_shooter * fort_level,
        attacker_shooter * tower_total,
        attacker_spellcaster * mage_guild_level,
        attacker_spellcaster * current_mana_diff,
        attacker_no_retaliation * fort_level,
        defender_shooter * tower_total,
        defender_shooter * keep_health,
        defender_spellcaster * mage_guild_level,
        defender_magic_resistance * spell_power_diff,
        spell_power_diff * mage_guild_level,
        current_mana_diff * mage_guild_level,
        spell_count_diff * mage_guild_level,
    ])
    return values


FEATURE_NAMES = [
    "log_strength_ratio",
    "hero_vs_hero",
    "log_attacker_army",
    "log_defender_army",
    "log_stack_count_ratio",
    "max_stack_share_diff",
    "attack_diff",
    "defense_diff",
    "spell_power_diff",
    "knowledge_diff",
    "mana_ratio_diff",
    "raw_mana_log_diff",
    "mana_limit_log_diff",
    "combat_spell_count_diff",
    "secondary_skill_count_diff",
    "attacker_spellbook",
    "defender_spellbook",
    "town_defender",
    "town_fort_level",
    "town_mage_guild_level",
    "town_has_tavern",
    "town_has_grail",
    "town_building_count",
    "town_walls_health",
    "town_keep_health",
    "town_tower_health",
    "town_has_moat",
    "town_keep_damage_log",
    "town_tower_damage_log",
    "creature_stats_available",
    "total_hp_log_diff",
    "total_damage_log_diff",
    "damage_per_hp_log_diff",
    "base_attack_weighted_diff",
    "base_defense_weighted_diff",
    "speed_weighted_diff",
    "max_speed_diff",
    "shooter_power_share_diff",
    "flying_power_share_diff",
    "blocks_retaliation_share_diff",
    "unlimited_retaliations_share_diff",
    "additional_attack_share_diff",
    "return_after_strike_share_diff",
    "area_attack_share_diff",
    "spell_after_attack_share_diff",
    "spellcaster_share_diff",
    "magic_resistance_avg_diff",
    "level_spell_immunity_avg_diff",
    "spell_damage_reduction_avg_diff",
    "spell_immunity_share_diff",
    "non_living_or_undead_share_diff",
]

RATIO_FEATURE_NAMES = ["log_strength_ratio"]

V3_COMPATIBLE_FEATURE_NAMES = [
    "log_strength_ratio",
    "hero_vs_hero",
    "log_attacker_army",
    "log_defender_army",
    "log_stack_count_ratio",
    "max_stack_share_diff",
    "attack_diff",
    "defense_diff",
    "spell_power_diff",
    "knowledge_diff",
    "level_diff",
    "mana_ratio_diff",
    "current_mana_diff",
    "mana_limit_diff",
    "attacker_spellbook",
    "defender_spellbook",
    "combat_spell_count_diff",
    "log_hero_strength_ratio",
]

TOWN_DEPLOYABLE_FEATURE_NAMES = [
    "log_attacker_to_deployed_town_danger",
    "log_strength_ratio",
    "hero_vs_hero",
    "defending_hero_visiting",
    "defending_hero_garrison",
    "log_attacker_army",
    "log_defender_army",
    "log_town_army",
    "town_pre_merge_available",
    "log_pre_merge_town_army",
    "log_pre_merge_defending_hero_army",
    "pre_merge_town_army_share",
    "pre_merge_defending_hero_army_share",
    "pre_merge_town_stack_count",
    "pre_merge_defending_hero_stack_count",
    "pre_merge_town_largest_stack_share",
    "pre_merge_defending_hero_largest_stack_share",
    "log_stack_count_ratio",
    "max_stack_share_diff",
    "attack_diff",
    "defense_diff",
    "spell_power_diff",
    "knowledge_diff",
    "level_diff",
    "mana_ratio_diff",
    "current_mana_diff",
    "mana_limit_diff",
    "attacker_spellbook",
    "defender_spellbook",
    "combat_spell_count_diff",
    "secondary_skill_count_diff",
    "log_hero_strength_ratio",
    "town_fort_level",
    "town_mage_guild_level",
    "town_has_tavern",
    "town_has_grail",
    "town_building_count",
    "town_walls_health",
    "town_keep_health",
    "town_tower_health",
    "town_has_moat",
    "town_keep_damage_log",
    "town_tower_damage_log",
] + [f"town_faction_{index}" for index in range(TOWN_FACTION_BUCKETS)]
TOWN_DEPLOYABLE_FEATURE_NAMES += [f"terrain_{index}" for index in range(TOWN_TERRAIN_BUCKETS)]
TOWN_DEPLOYABLE_FEATURE_NAMES += [f"battlefield_{index}" for index in range(TOWN_BATTLEFIELD_BUCKETS)]

TOWN_RICH_DEPLOYABLE_EXTRA_NAMES = [
    "creature_stats_available",
    "total_hp_log_diff",
    "total_damage_log_diff",
    "damage_per_hp_log_diff",
    "base_attack_weighted_diff",
    "base_defense_weighted_diff",
    "speed_weighted_diff",
    "max_speed_diff",
    "shooter_power_share_diff",
    "flying_power_share_diff",
    "blocks_retaliation_share_diff",
    "unlimited_retaliations_share_diff",
    "additional_attack_share_diff",
    "return_after_strike_share_diff",
    "area_attack_share_diff",
    "spell_after_attack_share_diff",
    "spellcaster_share_diff",
    "magic_resistance_avg_diff",
    "level_spell_immunity_avg_diff",
    "spell_damage_reduction_avg_diff",
    "spell_immunity_share_diff",
    "non_living_or_undead_share_diff",
    "initial_wall_total",
    "initial_keep_health",
    "initial_tower_health",
    "initial_gate_health",
    "initial_gate_state",
    "log_strength_ratio_x_fort_level",
    "log_strength_ratio_x_moat",
    "attacker_flying_share_x_fort_level",
    "attacker_flying_share_x_moat",
    "attacker_flying_share_x_initial_wall_total",
    "attacker_shooter_share_x_fort_level",
    "attacker_shooter_share_x_initial_tower_health",
    "attacker_spellcaster_share_x_mage_guild_level",
    "attacker_spellcaster_share_x_current_mana_diff",
    "attacker_no_retaliation_share_x_fort_level",
    "defender_shooter_share_x_initial_tower_health",
    "defender_shooter_share_x_initial_keep_health",
    "defender_spellcaster_share_x_mage_guild_level",
    "defender_magic_resistance_avg_x_spell_power_diff",
    "spell_power_diff_x_mage_guild_level",
    "current_mana_diff_x_mage_guild_level",
    "combat_spell_count_diff_x_mage_guild_level",
]

TOWN_RICH_DEPLOYABLE_FEATURE_NAMES = TOWN_DEPLOYABLE_FEATURE_NAMES + TOWN_RICH_DEPLOYABLE_EXTRA_NAMES


def army_power_by_creature(row: dict[str, Any], side: str) -> dict[int, float]:
    result: dict[int, float] = defaultdict(float)
    for stack in row.get(f"{side}Army") or []:
        result[int(stack["creature"])] += float(stack.get("power") or 0.0)
    return result


def hero_secondary_levels(hero: dict[str, Any] | None) -> dict[int, float]:
    result: dict[int, float] = defaultdict(float)
    if not hero:
        return result

    for entry in hero.get("secondary") or []:
        if isinstance(entry, dict) and entry.get("skill") is not None:
            result[int(entry["skill"])] = float(entry.get("level") or 0.0)
    return result


def hero_combat_spells(hero: dict[str, Any] | None) -> set[int]:
    if not hero:
        return set()
    return {int(spell_id) for spell_id in hero.get("combatSpells") or []}


def make_skill_spell_feature_function(groups: list[Group], min_weight: int = 1) -> tuple[FeatureFunction, list[str]]:
    skill_weights: Counter = Counter()
    spell_weights: Counter = Counter()
    for group in groups:
        skill_ids = {
            int(entry["skill"])
            for side in ("attacker", "defender")
            for entry in (group.row.get(f"{side}Hero") or {}).get("secondary") or []
            if isinstance(entry, dict) and entry.get("skill") is not None
        }
        spell_ids = {
            int(spell_id)
            for side in ("attacker", "defender")
            for spell_id in (group.row.get(f"{side}Hero") or {}).get("combatSpells") or []
        }
        for skill_id in skill_ids:
            skill_weights[skill_id] += group.count
        for spell_id in spell_ids:
            spell_weights[spell_id] += group.count

    skill_ids = sorted(skill_id for skill_id, weight in skill_weights.items() if weight >= min_weight)
    spell_ids = sorted(spell_id for spell_id, weight in spell_weights.items() if weight >= min_weight)
    names = (
        FEATURE_NAMES
        + [f"secondary_{skill_id}_level_diff" for skill_id in skill_ids]
        + [f"combat_spell_{spell_id}_presence_diff" for spell_id in spell_ids]
    )

    def features(row: dict[str, Any]) -> list[float]:
        attacker_skills = hero_secondary_levels(row.get("attackerHero"))
        defender_skills = hero_secondary_levels(row.get("defenderHero"))
        attacker_spells = hero_combat_spells(row.get("attackerHero"))
        defender_spells = hero_combat_spells(row.get("defenderHero"))
        skill_diffs = [attacker_skills.get(skill_id, 0.0) - defender_skills.get(skill_id, 0.0) for skill_id in skill_ids]
        spell_diffs = [
            float(spell_id in attacker_spells) - float(spell_id in defender_spells)
            for spell_id in spell_ids
        ]
        return feature_vector(row) + skill_diffs + spell_diffs

    return features, names


def make_composition_feature_function(groups: list[Group], min_weight: int = 1) -> tuple[FeatureFunction, list[str]]:
    creature_weights: Counter = Counter()
    for group in groups:
        creature_ids = {
            int(stack["creature"])
            for side in ("attacker", "defender")
            for stack in group.row.get(f"{side}Army") or []
        }
        for creature_id in creature_ids:
            creature_weights[creature_id] += group.count

    creature_ids = sorted(creature_id for creature_id, weight in creature_weights.items() if weight >= min_weight)
    names = (
        V3_COMPATIBLE_FEATURE_NAMES
        + [f"creature_{creature_id}_power_share_diff" for creature_id in creature_ids]
    )

    def features(row: dict[str, Any]) -> list[float]:
        attacker_total = max(float(row.get("attackerArmyStrength") or 0.0), 1.0)
        defender_total = max(float(row.get("defenderArmyStrength") or 0.0), 1.0)
        attacker_power = army_power_by_creature(row, "attacker")
        defender_power = army_power_by_creature(row, "defender")
        attacker_shares = [attacker_power.get(creature_id, 0.0) / attacker_total for creature_id in creature_ids]
        defender_shares = [defender_power.get(creature_id, 0.0) / defender_total for creature_id in creature_ids]
        share_diffs = [attacker - defender for attacker, defender in zip(attacker_shares, defender_shares)]
        return v3_compatible_feature_vector(row) + share_diffs

    return features, names


def setup_key(row: dict[str, Any]) -> str:
    def clean_hero(hero: dict[str, Any] | None) -> Any:
        if not hero:
            return None
        return {
            "type": hero.get("type"),
            "level": hero.get("level"),
            "mana": hero.get("mana") if row.get("schema", 1) >= 2 else None,
            "manaLimit": hero.get("manaLimit"),
            "hasSpellbook": hero.get("hasSpellbook"),
            "combatSpellCount": hero.get("combatSpellCount"),
            "primary": hero.get("primary"),
            "heroStrength": hero.get("heroStrength"),
            "secondary": hero.get("secondary") if row.get("schema", 1) >= 3 else None,
            "combatSpells": hero.get("combatSpells") if row.get("schema", 1) >= 3 else None,
        }

    def clean_town(town: dict[str, Any] | None) -> Any:
        if not town:
            return None
        return {
            "faction": town.get("faction"),
            "fortLevel": town.get("fortLevel"),
            "hallLevel": town.get("hallLevel"),
            "mageGuildLevel": town.get("mageGuildLevel"),
            "hasFort": town.get("hasFort"),
            "hasBuiltTavern": town.get("hasBuiltTavern"),
            "hasBuiltGrail": town.get("hasBuiltGrail"),
            "battleTerrain": town.get("battleTerrain"),
            "armyStrength": town.get("armyStrength"),
            "buildings": town.get("buildings"),
            "fortifications": town.get("fortifications"),
            "towerDamage": town.get("towerDamage"),
            "keepDamage": town.get("keepDamage"),
        }

    def clean_army_snapshot(army: dict[str, Any] | None) -> Any:
        if not army:
            return None
        return {
            "objectId": army.get("objectId"),
            "armyStrength": army.get("armyStrength"),
            "stacks": army.get("stacks"),
        }

    def clean_town_pre_merge(pre_merge: dict[str, Any] | None) -> Any:
        if not pre_merge:
            return None
        return {
            "townId": pre_merge.get("townId"),
            "defendingHeroId": pre_merge.get("defendingHeroId"),
            "townArmy": clean_army_snapshot(pre_merge.get("townArmy")),
            "defendingHeroArmy": clean_army_snapshot(pre_merge.get("defendingHeroArmy")),
        }

    stable = {
        "battleType": battle_type(row),
        "terrain": row.get("terrain"),
        "battlefield": row.get("battlefield"),
        "defendedTown": clean_town(row.get("defendedTown")),
        "townPreMergeState": clean_town_pre_merge(row.get("townPreMergeState")) if row.get("schema", 1) >= 5 else None,
        "initialWallState": row.get("initialWallState") if row.get("schema", 1) >= 4 else None,
        "attackerHero": clean_hero(row.get("attackerHero")),
        "defenderHero": clean_hero(row.get("defenderHero")),
        "attackerArmyStrength": row.get("attackerArmyStrength"),
        "defenderArmyStrength": row.get("defenderArmyStrength"),
        "attackerArmy": row.get("attackerArmy"),
        "defenderArmy": row.get("defenderArmy"),
    }
    return json.dumps(stable, sort_keys=True, separators=(",", ":"))


def shard_setup_key(row: dict[str, Any]) -> str:
    if row.get("shardIndex") is None or row.get("globalSeed") is None:
        return setup_key(row)

    stable = {
        "globalSeed": row.get("globalSeed"),
        "shardIndex": row.get("shardIndex"),
        "shardSeed": row.get("shardSeed"),
        "battleType": battle_type(row),
    }
    return json.dumps(stable, sort_keys=True, separators=(",", ":"))


def casualty_power(row: dict[str, Any], side: str) -> float:
    by_creature: dict[int, list[float]] = defaultdict(lambda: [0.0, 0.0])
    for stack in row.get(f"{side}Army") or []:
        count = float(stack.get("count") or 0)
        if count <= 0:
            continue
        entry = by_creature[int(stack["creature"])]
        entry[0] += float(stack.get("power") or 0)
        entry[1] += count

    result = 0.0
    for casualty in row.get(f"{side}Casualties") or []:
        power, count = by_creature[int(casualty["creature"])]
        if count > 0:
            result += float(casualty.get("killed") or 0) * power / count
    return result


def load_groups(path: str, group_key: str = "setup") -> tuple[list[Group], Counter, int]:
    groups: dict[str, Group] = {}
    schema_counts: Counter = Counter()
    rows = 0
    for row in iter_json_lines(path):
        rows += 1
        schema_counts[row.get("schema", 1)] += 1
        key = shard_setup_key(row) if group_key == "shard" else setup_key(row)
        group = groups.setdefault(key, Group(key=key, row=row))
        group.count += 1
        group.attacker_wins += row.get("winner") == "attacker"
        group.no_winner += row.get("winner") == "none"
        army_strength = max(float(row.get("attackerArmyStrength") or 0), 1.0)
        group.attacker_loss_ratio_sum += casualty_power(row, "attacker") / army_strength
    return list(groups.values()), schema_counts, rows


def split_groups(groups: list[Group], test_fraction: float) -> tuple[list[Group], list[Group]]:
    test_cutoff = int(max(0.0, min(1.0, test_fraction)) * 10000)
    train: list[Group] = []
    test: list[Group] = []
    for group in groups:
        digest = hashlib.sha256(group.key.encode("utf-8")).digest()
        bucket = int.from_bytes(digest[:4], "big") % 10000
        (test if bucket < test_cutoff else train).append(group)
    return train, test


def sigmoid(value: float) -> float:
    if value >= 0:
        z = math.exp(-value)
        return 1.0 / (1.0 + z)
    z = math.exp(value)
    return z / (1.0 + z)


@dataclass
class Scaler:
    mean: list[float]
    scale: list[float]

    def transform(self, values: list[float]) -> list[float]:
        return [(value - mean) / scale for value, mean, scale in zip(values, self.mean, self.scale)]


FeatureFunction = Callable[[dict[str, Any]], list[float]]


def make_scaler(groups: list[Group], features: FeatureFunction) -> Scaler:
    columns = list(zip(*(features(group.row) for group in groups)))
    mean = [statistics.fmean(column) for column in columns]
    scale = []
    for column, avg in zip(columns, mean):
        variance = statistics.fmean((value - avg) ** 2 for value in column)
        scale.append(math.sqrt(variance) if variance > EPSILON else 1.0)
    return Scaler(mean=mean, scale=scale)


@dataclass
class LogisticModel:
    intercept: float
    coefficients: list[float]
    scaler: Scaler
    features: FeatureFunction

    def predict(self, row: dict[str, Any]) -> float:
        values = self.scaler.transform(self.features(row))
        score = self.intercept + sum(coefficient * value for coefficient, value in zip(self.coefficients, values))
        return sigmoid(score)


def fit_logistic(groups: list[Group], epochs: int, learning_rate: float, l2: float, features: FeatureFunction) -> LogisticModel:
    scaler = make_scaler(groups, features)
    width = len(features(groups[0].row))
    intercept = 0.0
    coefficients = [0.0] * width
    total_weight = sum(group.count for group in groups)

    m = [0.0] * (width + 1)
    v = [0.0] * (width + 1)
    beta1 = 0.9
    beta2 = 0.999

    for epoch in range(1, epochs + 1):
        gradient = [0.0] * (width + 1)
        for group in groups:
            values = scaler.transform(features(group.row))
            score = intercept + sum(coefficient * value for coefficient, value in zip(coefficients, values))
            error = sigmoid(score) - group.win_rate
            weight = group.count / total_weight
            gradient[0] += weight * error
            for index, value in enumerate(values, start=1):
                gradient[index] += weight * error * value

        for index, coefficient in enumerate(coefficients, start=1):
            gradient[index] += l2 * coefficient

        step_size = learning_rate * (0.1 + 0.9 * (1.0 - epoch / (epochs + 1)))
        parameters = [intercept] + coefficients
        for index, grad in enumerate(gradient):
            m[index] = beta1 * m[index] + (1.0 - beta1) * grad
            v[index] = beta2 * v[index] + (1.0 - beta2) * grad * grad
            m_hat = m[index] / (1.0 - beta1 ** epoch)
            v_hat = v[index] / (1.0 - beta2 ** epoch)
            parameters[index] -= step_size * m_hat / (math.sqrt(v_hat) + 1e-8)
        intercept = parameters[0]
        coefficients = parameters[1:]

    return LogisticModel(intercept=intercept, coefficients=coefficients, scaler=scaler, features=features)


def current_safe_prediction(row: dict[str, Any], safe_ratio: float) -> bool:
    attacker = side_strength(row, "attacker")
    defender = side_strength(row, "defender")
    if defender <= 0:
        return True
    return attacker > defender * safe_ratio


def town_fort_danger_bonus(row: dict[str, Any]) -> float:
    town = row.get("defendedTown") or {}
    fort_level = int(town.get("fortLevel") or 0)
    if fort_level >= 3:
        return 10000.0
    if fort_level == 2:
        return 4000.0
    return 0.0


def deployed_danger(row: dict[str, Any], town_danger_factor: float = 1.0) -> float:
    defender_army = max(float(row.get("defenderArmyStrength") or 0.0), 0.0)
    defender_hero = row.get("defenderHero")
    type_name = battle_type(row)

    if not type_name.startswith("town"):
        return side_strength(row, "defender")

    town = row.get("defendedTown") or {}
    source = town.get("defendingHeroSource")
    has_visiting_hero = bool(town.get("hasVisitingHero"))
    has_garrison_hero = bool(town.get("hasGarrisonHero"))
    town_army = max(float(town.get("armyStrength") or 0.0), 0.0)

    if type_name == "town-hero" and source == "visiting":
        if has_garrison_hero:
            danger = defender_army
        else:
            danger = town_army
            if danger > 0.0 or has_visiting_hero:
                danger += town_fort_danger_bonus(row)
            danger += defender_army
    elif type_name == "town-hero" and source == "garrison":
        danger = defender_army
        if danger > 0.0:
            danger += town_fort_danger_bonus(row)
    else:
        danger = town_army
        if danger > 0.0:
            danger += town_fort_danger_bonus(row)

    if defender_hero and type_name == "town-hero":
        danger *= hero_strength(defender_hero)

    return danger * town_danger_factor


def deployed_safe_prediction(row: dict[str, Any], safe_ratio: float, town_danger_factor: float = 1.0) -> bool:
    attacker = side_strength(row, "attacker")
    defender = deployed_danger(row, town_danger_factor)
    if defender <= 0:
        return True
    return attacker > defender * safe_ratio


def current_loss_prediction(row: dict[str, Any]) -> float:
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    return max(0.0, min(1.0, (defender / attacker) ** 2))


def summarize_deployed_danger(groups: list[Group], safe_ratio: float, town_danger_factor: float = 1.0) -> dict[str, Any]:
    rows = sum(group.count for group in groups)
    correct = 0.0
    brier = 0.0
    false_safe = 0
    false_unsafe = 0
    safe_groups = 0
    by_type: Counter = Counter()

    for group in groups:
        row = group.row
        expected_win = group.win_rate
        actual_class = expected_win >= 0.5
        safe = deployed_safe_prediction(row, safe_ratio, town_danger_factor)
        probability = 1.0 if safe else 0.0

        correct += group.count * (safe == actual_class)
        brier += group.count * (probability - expected_win) ** 2
        false_safe += safe and expected_win < 0.95
        false_unsafe += (not safe) and expected_win >= 0.95
        safe_groups += safe
        by_type[f"{battle_type(row)}:{'safe' if safe else 'unsafe'}"] += group.count

    return {
        "factor": town_danger_factor,
        "rows": rows,
        "groups": len(groups),
        "accuracy": correct / rows if rows else 0.0,
        "brier": brier / rows if rows else 0.0,
        "false_safe_groups": false_safe,
        "false_unsafe_groups": false_unsafe,
        "safe_groups": safe_groups,
        "by_type": dict(sorted(by_type.items())),
    }


def summarize_predictions(groups: list[Group], safe_ratio: float, model: LogisticModel | None = None) -> dict[str, Any]:
    rows = sum(group.count for group in groups)
    baseline_correct = 0.0
    baseline_brier = 0.0
    model_correct = 0.0
    model_brier = 0.0
    v3_correct = 0.0
    v3_brier = 0.0
    v3_deployed_rows = 0
    v3_deployed_groups = 0
    v3_deployed_correct = 0.0
    v3_deployed_brier = 0.0
    v3_deployed_false_safe = 0
    v3_deployed_false_unsafe = 0
    loss_abs = []
    loss_squared = []
    false_safe = 0
    false_unsafe = 0
    v3_false_safe = 0
    v3_false_unsafe = 0
    by_type: Counter = Counter()

    for group in groups:
        row = group.row
        weight = group.count
        expected_win = group.win_rate
        actual_class = expected_win >= 0.5
        safe = current_safe_prediction(row, safe_ratio)
        baseline_probability = 1.0 if safe else 0.0

        baseline_correct += weight * (safe == actual_class)
        baseline_brier += weight * (baseline_probability - expected_win) ** 2
        if safe and expected_win < 0.95:
            false_safe += 1
        if not safe and expected_win >= 0.95:
            false_unsafe += 1

        if model:
            probability = model.predict(row)
            model_correct += weight * ((probability >= 0.5) == actual_class)
            model_brier += weight * (probability - expected_win) ** 2

        v3_probability = cxx_v3_probability(row)
        v3_safe = v3_probability >= V3_SAFE_PROBABILITY
        v3_correct += weight * (v3_safe == actual_class)
        v3_brier += weight * (v3_probability - expected_win) ** 2
        if v3_safe and expected_win < 0.95:
            v3_false_safe += 1
        if not v3_safe and expected_win >= 0.95:
            v3_false_unsafe += 1
        if cxx_v3_static_calibration_applies(row):
            v3_deployed_rows += weight
            v3_deployed_groups += 1
            v3_deployed_correct += weight * (v3_safe == actual_class)
            v3_deployed_brier += weight * (v3_probability - expected_win) ** 2
            if v3_safe and expected_win < 0.95:
                v3_deployed_false_safe += 1
            if not v3_safe and expected_win >= 0.95:
                v3_deployed_false_unsafe += 1

        loss_prediction = current_loss_prediction(row)
        loss_abs.append(abs(loss_prediction - group.attacker_loss_ratio))
        loss_squared.append((loss_prediction - group.attacker_loss_ratio) ** 2)
        by_type[(battle_type(row), "safe" if safe else "unsafe")] += 1

    result = {
        "rows": rows,
        "groups": len(groups),
        "baseline_accuracy": baseline_correct / rows if rows else 0.0,
        "baseline_brier": baseline_brier / rows if rows else 0.0,
        "baseline_false_safe_groups": false_safe,
        "baseline_false_unsafe_groups": false_unsafe,
        "v3_accuracy": v3_correct / rows if rows else 0.0,
        "v3_brier": v3_brier / rows if rows else 0.0,
        "v3_false_safe_groups": v3_false_safe,
        "v3_false_unsafe_groups": v3_false_unsafe,
        "v3_deployed_rows": v3_deployed_rows,
        "v3_deployed_groups": v3_deployed_groups,
        "v3_deployed_accuracy": v3_deployed_correct / v3_deployed_rows if v3_deployed_rows else 0.0,
        "v3_deployed_brier": v3_deployed_brier / v3_deployed_rows if v3_deployed_rows else 0.0,
        "v3_deployed_false_safe_groups": v3_deployed_false_safe,
        "v3_deployed_false_unsafe_groups": v3_deployed_false_unsafe,
        "baseline_loss_mae": statistics.fmean(loss_abs) if loss_abs else 0.0,
        "baseline_loss_rmse": math.sqrt(statistics.fmean(loss_squared)) if loss_squared else 0.0,
        "baseline_by_type": {f"{key[0]}:{key[1]}": value for key, value in sorted(by_type.items())},
        "deployed_danger": summarize_deployed_danger(groups, safe_ratio),
    }
    if model:
        result["model_accuracy"] = model_correct / rows if rows else 0.0
        result["model_brier"] = model_brier / rows if rows else 0.0
    return result


def threshold_summary(groups: list[Group], model: LogisticModel) -> dict[str, Any]:
    rows = sum(group.count for group in groups)

    def evaluate(threshold: float) -> dict[str, Any]:
        correct = 0.0
        false_safe = 0
        false_unsafe = 0
        safe = 0
        for group in groups:
            prediction = model.predict(group.row) >= threshold
            expected_win = group.win_rate
            correct += group.count * (prediction == (expected_win >= 0.5))
            false_safe += prediction and expected_win < 0.95
            false_unsafe += (not prediction) and expected_win >= 0.95
            safe += prediction
        return {
            "threshold": threshold,
            "accuracy": correct / rows if rows else 0.0,
            "false_safe_groups": false_safe,
            "false_unsafe_groups": false_unsafe,
            "safe_groups": safe,
            "safety_cost": false_safe * 5 + false_unsafe,
        }

    candidates = [evaluate(index / 100.0) for index in range(1, 100)]
    best_accuracy = max(candidates, key=lambda item: (item["accuracy"], -item["false_safe_groups"]))
    best_safety = min(candidates, key=lambda item: (item["safety_cost"], -item["accuracy"]))
    return {
        "best_accuracy": best_accuracy,
        "best_safety": best_safety,
    }


def fit_loss_grid(groups: list[Group]) -> dict[str, Any]:
    samples = []
    for group in groups:
        row = group.row
        attacker = max(side_strength(row, "attacker"), EPSILON)
        defender = max(side_strength(row, "defender"), EPSILON)
        samples.append((defender / attacker, group.attacker_loss_ratio, battle_type(row)))

    battle_types = sorted({sample_type for _, _, sample_type in samples}) or ["hero-monster"]
    default_factors = {battle_type_name: 1.0 for battle_type_name in battle_types}

    def evaluate_formula(exponent: float, factors: dict[str, float]) -> dict[str, Any]:
        squared = []
        absolute = []
        for ratio, expected, sample_type in samples:
            predicted = max(0.0, min(1.0, factors[sample_type] * (ratio ** exponent)))
            squared.append((predicted - expected) ** 2)
            absolute.append(abs(predicted - expected))
        return {
            "rmse": math.sqrt(statistics.fmean(squared)) if squared else 0.0,
            "mae": statistics.fmean(absolute) if absolute else 0.0,
            "exponent": exponent,
            "factors": dict(factors),
        }

    best = evaluate_formula(2.0, default_factors)
    for exponent_i in range(50, 351):
        exponent = exponent_i / 100.0
        factors = {}
        for battle_type_name in battle_types:
            numerator = 0.0
            denominator = 0.0
            for ratio, expected, sample_type in samples:
                if sample_type != battle_type_name:
                    continue
                value = ratio ** exponent
                numerator += expected * value
                denominator += value * value
            factors[battle_type_name] = max(0.05, min(3.0, numerator / denominator if denominator > EPSILON else 1.0))

        for candidate_factors in (factors, default_factors):
            candidate = evaluate_formula(exponent, candidate_factors)
            if candidate["rmse"] < best["rmse"]:
                best = candidate
    return best


def hero_summary(hero: dict[str, Any] | None) -> str:
    if not hero:
        return "none"
    primary_values = hero.get("primary") or [0, 0, 0, 0]
    return (
        f"type={hero.get('type')} lvl={hero.get('level')} "
        f"prim={primary_values} mana={hero.get('mana')}/{hero.get('manaLimit')} "
        f"spells={len(hero.get('combatSpells') or [])} skills={len(hero.get('secondary') or [])}"
    )


def army_summary(row: dict[str, Any], side: str) -> str:
    stacks = row.get(f"{side}Army") or []
    parts = [f"{stack.get('creature')}x{stack.get('count')}" for stack in stacks[:7]]
    return "[" + ", ".join(parts) + "]"


def town_summary(row: dict[str, Any]) -> str:
    town = row.get("defendedTown")
    if not town:
        return "none"
    fortifications = town.get("fortifications") or {}
    return (
        f"faction={town.get('faction')} fort={town.get('fortLevel')} "
        f"mage={town.get('mageGuildLevel')} tavern={town.get('hasBuiltTavern')} "
        f"grail={town.get('hasBuiltGrail')} walls={fortifications.get('wallsHealth')} "
        f"keep={fortifications.get('citadelHealth')} towers="
        f"{fortifications.get('upperTowerHealth')}/{fortifications.get('lowerTowerHealth')} "
        f"moat={fortifications.get('hasMoat')}"
    )


def town_pre_merge_summary(row: dict[str, Any]) -> str:
    pre_merge = town_pre_merge_state(row)
    if not pre_merge:
        return "none"

    town_army = town_pre_merge_army_strength(row, "townArmy")
    hero_army = town_pre_merge_army_strength(row, "defendingHeroArmy")
    defender_army = max(float(row.get("defenderArmyStrength") or 0.0), 1.0)
    pre_merge_total = town_army + hero_army
    return (
        f"town_army={town_army:.0f} "
        f"hero_army={hero_army:.0f} "
        f"battle_defender_army={defender_army:.0f} "
        f"not_in_battle={max(0.0, pre_merge_total - defender_army):.0f} "
        f"town_share={town_army / defender_army:.3f} "
        f"hero_share={hero_army / defender_army:.3f} "
        f"town_stacks={town_pre_merge_stack_count(row, 'townArmy'):.0f} "
        f"hero_stacks={town_pre_merge_stack_count(row, 'defendingHeroArmy'):.0f} "
        f"town_largest={town_pre_merge_largest_share(row, 'townArmy'):.3f} "
        f"hero_largest={town_pre_merge_largest_share(row, 'defendingHeroArmy'):.3f} "
        f"post_town_army={town_feature(row, 'armyStrength'):.0f}"
    )


def compact_group_summary(group: Group, model: LogisticModel | None, safe_ratio: float) -> dict[str, Any]:
    row = group.row
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    probability = model.predict(row) if model else None
    v3_probability = cxx_v3_probability(row)
    return {
        "count": group.count,
        "win_rate": group.win_rate,
        "model_probability": probability,
        "model_error": abs(probability - group.win_rate) if probability is not None else None,
        "cxx_v3_probability": v3_probability,
        "cxx_v3_error": abs(v3_probability - group.win_rate),
        "safe_baseline": current_safe_prediction(row, safe_ratio),
        "strength_ratio": attacker / defender,
        "battle_type": battle_type(row),
        "terrain": row.get("terrain"),
        "battlefield": row.get("battlefield"),
        "attacker_hero": hero_summary(row.get("attackerHero")),
        "defender_hero": hero_summary(row.get("defenderHero")),
        "town": town_summary(row),
        "town_pre_merge": town_pre_merge_summary(row),
        "attacker_army": army_summary(row, "attacker"),
        "defender_army": army_summary(row, "defender"),
    }


def near_even_groups(groups: list[Group], limit: int, model: LogisticModel | None, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = sorted(groups, key=lambda group: (abs(group.win_rate - 0.5), -group.count, group.key))
    return [compact_group_summary(group, model, safe_ratio) for group in candidates[:limit]]


def worst_model_groups(groups: list[Group], limit: int, model: LogisticModel, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = sorted(groups, key=lambda group: abs(model.predict(group.row) - group.win_rate), reverse=True)
    return [compact_group_summary(group, model, safe_ratio) for group in candidates[:limit]]


def worst_v3_groups(groups: list[Group], limit: int, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = sorted(groups, key=lambda group: abs(cxx_v3_probability(group.row) - group.win_rate), reverse=True)
    result = []
    for group in candidates[:limit]:
        summary = compact_group_summary(group, None, safe_ratio)
        probability = cxx_v3_probability(group.row)
        summary["model_probability"] = probability
        summary["model_error"] = abs(probability - group.win_rate)
        result.append(summary)
    return result


def v3_false_safe_groups(groups: list[Group], limit: int, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = [
        group
        for group in groups
        if cxx_v3_probability(group.row) >= V3_SAFE_PROBABILITY and group.win_rate < 0.95
    ]
    candidates.sort(key=lambda group: (cxx_v3_probability(group.row) - group.win_rate, group.count), reverse=True)
    result = []
    for group in candidates[:limit]:
        summary = compact_group_summary(group, None, safe_ratio)
        probability = cxx_v3_probability(group.row)
        summary["model_probability"] = probability
        summary["model_error"] = probability - group.win_rate
        result.append(summary)
    return result


def v3_false_unsafe_groups(groups: list[Group], limit: int, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = [
        group
        for group in groups
        if cxx_v3_probability(group.row) < V3_SAFE_PROBABILITY and group.win_rate >= 0.95
    ]
    candidates.sort(key=lambda group: (group.win_rate - cxx_v3_probability(group.row), group.count), reverse=True)
    result = []
    for group in candidates[:limit]:
        summary = compact_group_summary(group, None, safe_ratio)
        probability = cxx_v3_probability(group.row)
        summary["model_probability"] = probability
        summary["model_error"] = group.win_rate - probability
        result.append(summary)
    return result


def model_false_safe_groups(groups: list[Group], limit: int, model: LogisticModel, threshold: float, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = []
    for group in groups:
        probability = model.predict(group.row)
        if probability >= threshold and group.win_rate < 0.95:
            candidates.append((group, probability))
    candidates.sort(key=lambda item: (item[1] - item[0].win_rate, item[0].count), reverse=True)
    result = []
    for group, probability in candidates[:limit]:
        summary = compact_group_summary(group, model, safe_ratio)
        summary["model_probability"] = probability
        summary["model_error"] = probability - group.win_rate
        result.append(summary)
    return result


def model_false_unsafe_groups(groups: list[Group], limit: int, model: LogisticModel, threshold: float, safe_ratio: float) -> list[dict[str, Any]]:
    candidates = []
    for group in groups:
        probability = model.predict(group.row)
        if probability < threshold and group.win_rate >= 0.95:
            candidates.append((group, probability))
    candidates.sort(key=lambda item: (item[0].win_rate - item[1], item[0].count), reverse=True)
    result = []
    for group, probability in candidates[:limit]:
        summary = compact_group_summary(group, model, safe_ratio)
        summary["model_probability"] = probability
        summary["model_error"] = group.win_rate - probability
        result.append(summary)
    return result


def print_group_report(title: str, groups: list[dict[str, Any]]) -> None:
    print(title + ":")
    for index, group in enumerate(groups, start=1):
        probability = group["model_probability"]
        probability_text = "n/a" if probability is None else f"{probability:.4f}"
        error = group["model_error"]
        error_text = "n/a" if error is None else f"{error:.4f}"
        print(
            f"  {index}. type={group['battle_type']} count={group['count']} "
            f"win_rate={group['win_rate']:.4f} model={probability_text} "
            f"error={error_text} cxx_v3={group['cxx_v3_probability']:.4f} "
            f"cxx_v3_error={group['cxx_v3_error']:.4f} "
            f"baseline_safe={group['safe_baseline']} "
            f"strength_ratio={group['strength_ratio']:.4f} "
            f"terrain={group['terrain']} battlefield={group['battlefield']}"
        )
        print(f"     attacker: {group['attacker_hero']} army={group['attacker_army']}")
        print(f"     defender: {group['defender_hero']} army={group['defender_army']}")
        if group["town"] != "none":
            print(f"     town: {group['town']}")
        if group["town_pre_merge"] != "none":
            print(f"     pre-merge: {group['town_pre_merge']}")


def top_model_features(model: LogisticModel, names: list[str], limit: int) -> list[dict[str, float | str]]:
    items = sorted(
        zip(names, model.coefficients, model.scaler.mean, model.scaler.scale),
        key=lambda item: abs(item[1]),
        reverse=True,
    )
    return [
        {
            "name": name,
            "coefficient": coefficient,
            "mean": mean,
            "scale": scale,
        }
        for name, coefficient, mean, scale in items[:limit]
    ]


def main() -> int:
    args = parse_args()
    town_danger_factors = parse_float_list(args.town_danger_factors)
    groups, schema_counts, row_count = load_groups(args.dataset, args.group_key)
    groups = [
        group
        for group in groups
        if group.count >= args.min_group_size and matches_scope(group.row, args.scope)
    ]
    train, test = split_groups(groups, args.test_fraction)
    fit_models = not args.deployed_danger_only
    full_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, feature_vector) if train and fit_models else None
    ratio_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, ratio_feature_vector) if train and fit_models else None
    v3_compatible_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, v3_compatible_feature_vector) if train and fit_models else None
    town_deployable_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, town_deployable_feature_vector) if train and fit_models else None
    town_rich_deployable_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, town_rich_deployable_feature_vector) if train and fit_models else None
    composition_features: FeatureFunction | None = None
    composition_names: list[str] = []
    composition_model: LogisticModel | None = None
    skill_spell_features: FeatureFunction | None = None
    skill_spell_names: list[str] = []
    skill_spell_model: LogisticModel | None = None
    if train and fit_models:
        skill_spell_features, skill_spell_names = make_skill_spell_feature_function(train, args.skill_spell_min_weight)
        skill_spell_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, skill_spell_features)
        composition_features, composition_names = make_composition_feature_function(train, args.composition_min_weight)
        composition_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, composition_features)

    metrics: dict[str, Any] = {
        "dataset": args.dataset,
        "scope": args.scope,
        "group_key": args.group_key,
        "input_rows": row_count,
        "rows_after_filter": sum(group.count for group in groups),
        "schema_counts": dict(schema_counts),
        "groups_after_filter": len(groups),
        "train": summarize_predictions(train, args.safe_ratio, full_model),
        "test": summarize_predictions(test, args.safe_ratio, full_model),
    }
    if town_danger_factors:
        metrics["town_danger_factors"] = {
            "train": [summarize_deployed_danger(train, args.safe_ratio, factor) for factor in town_danger_factors],
            "test": [summarize_deployed_danger(test, args.safe_ratio, factor) for factor in town_danger_factors],
        }

    def serialize_model(model: LogisticModel, names: list[str]) -> dict[str, Any]:
        return {
            "intercept": model.intercept,
            "features": [
                {
                    "name": name,
                    "coefficient": coefficient,
                    "mean": mean,
                    "scale": scale,
                }
                for name, coefficient, mean, scale in zip(
                    names,
                    model.coefficients,
                    model.scaler.mean,
                    model.scaler.scale,
                )
            ],
        }

    if full_model and ratio_model and v3_compatible_model and town_deployable_model and town_rich_deployable_model:
        ratio_train = summarize_predictions(train, args.safe_ratio, ratio_model)
        ratio_test = summarize_predictions(test, args.safe_ratio, ratio_model)
        v3_compatible_train = summarize_predictions(train, args.safe_ratio, v3_compatible_model)
        v3_compatible_test = summarize_predictions(test, args.safe_ratio, v3_compatible_model)
        town_deployable_train = summarize_predictions(train, args.safe_ratio, town_deployable_model)
        town_deployable_test = summarize_predictions(test, args.safe_ratio, town_deployable_model)
        town_rich_deployable_train = summarize_predictions(train, args.safe_ratio, town_rich_deployable_model)
        town_rich_deployable_test = summarize_predictions(test, args.safe_ratio, town_rich_deployable_model)
        skill_spell_train = summarize_predictions(train, args.safe_ratio, skill_spell_model) if skill_spell_model else None
        skill_spell_test = summarize_predictions(test, args.safe_ratio, skill_spell_model) if skill_spell_model else None
        composition_train = summarize_predictions(train, args.safe_ratio, composition_model) if composition_model else None
        composition_test = summarize_predictions(test, args.safe_ratio, composition_model) if composition_model else None
        metrics["ratio_train"] = {
            "model_accuracy": ratio_train["model_accuracy"],
            "model_brier": ratio_train["model_brier"],
        }
        metrics["ratio_test"] = {
            "model_accuracy": ratio_test["model_accuracy"],
            "model_brier": ratio_test["model_brier"],
        }
        metrics["v3_compatible_train"] = {
            "model_accuracy": v3_compatible_train["model_accuracy"],
            "model_brier": v3_compatible_train["model_brier"],
        }
        metrics["v3_compatible_test"] = {
            "model_accuracy": v3_compatible_test["model_accuracy"],
            "model_brier": v3_compatible_test["model_brier"],
        }
        metrics["town_deployable_train"] = {
            "model_accuracy": town_deployable_train["model_accuracy"],
            "model_brier": town_deployable_train["model_brier"],
        }
        metrics["town_deployable_test"] = {
            "model_accuracy": town_deployable_test["model_accuracy"],
            "model_brier": town_deployable_test["model_brier"],
        }
        metrics["town_rich_deployable_train"] = {
            "model_accuracy": town_rich_deployable_train["model_accuracy"],
            "model_brier": town_rich_deployable_train["model_brier"],
        }
        metrics["town_rich_deployable_test"] = {
            "model_accuracy": town_rich_deployable_test["model_accuracy"],
            "model_brier": town_rich_deployable_test["model_brier"],
        }
        if skill_spell_train and skill_spell_test and skill_spell_model:
            metrics["skill_spell_train"] = {
                "model_accuracy": skill_spell_train["model_accuracy"],
                "model_brier": skill_spell_train["model_brier"],
            }
            metrics["skill_spell_test"] = {
                "model_accuracy": skill_spell_test["model_accuracy"],
                "model_brier": skill_spell_test["model_brier"],
            }
            metrics["skill_spell_feature_count"] = len(skill_spell_names)
            metrics["skill_spell_top_features"] = top_model_features(skill_spell_model, skill_spell_names, 25)
        if composition_train and composition_test and composition_model:
            metrics["composition_train"] = {
                "model_accuracy": composition_train["model_accuracy"],
                "model_brier": composition_train["model_brier"],
            }
            metrics["composition_test"] = {
                "model_accuracy": composition_test["model_accuracy"],
                "model_brier": composition_test["model_brier"],
            }
            metrics["composition_feature_count"] = len(composition_names)
            metrics["composition_top_features"] = top_model_features(composition_model, composition_names, 25)
        metrics["logistic_model"] = serialize_model(full_model, FEATURE_NAMES)
        metrics["ratio_logistic_model"] = serialize_model(ratio_model, RATIO_FEATURE_NAMES)
        metrics["v3_compatible_model"] = serialize_model(v3_compatible_model, V3_COMPATIBLE_FEATURE_NAMES)
        metrics["town_deployable_model"] = serialize_model(town_deployable_model, TOWN_DEPLOYABLE_FEATURE_NAMES)
        metrics["town_rich_deployable_model"] = serialize_model(town_rich_deployable_model, TOWN_RICH_DEPLOYABLE_FEATURE_NAMES)
        if skill_spell_model:
            metrics["skill_spell_model"] = serialize_model(skill_spell_model, skill_spell_names)
        metrics["thresholds"] = {
            "full_train": threshold_summary(train, full_model),
            "full_test": threshold_summary(test, full_model),
            "ratio_train": threshold_summary(train, ratio_model),
            "ratio_test": threshold_summary(test, ratio_model),
            "town_deployable_train": threshold_summary(train, town_deployable_model),
            "town_deployable_test": threshold_summary(test, town_deployable_model),
            "town_rich_deployable_train": threshold_summary(train, town_rich_deployable_model),
            "town_rich_deployable_test": threshold_summary(test, town_rich_deployable_model),
        }
        if skill_spell_model:
            metrics["thresholds"]["skill_spell_train"] = threshold_summary(train, skill_spell_model)
            metrics["thresholds"]["skill_spell_test"] = threshold_summary(test, skill_spell_model)
        if composition_model:
            metrics["thresholds"]["composition_train"] = threshold_summary(train, composition_model)
            metrics["thresholds"]["composition_test"] = threshold_summary(test, composition_model)
        metrics["loss_model"] = fit_loss_grid(train)
        if args.print_near_even > 0:
            metrics["near_even"] = near_even_groups(groups, args.print_near_even, full_model, args.safe_ratio)
        if args.print_worst > 0:
            metrics["worst_model_errors"] = worst_model_groups(groups, args.print_worst, full_model, args.safe_ratio)
            metrics["worst_town_deployable_errors"] = worst_model_groups(groups, args.print_worst, town_deployable_model, args.safe_ratio)
            metrics["worst_town_rich_deployable_errors"] = worst_model_groups(groups, args.print_worst, town_rich_deployable_model, args.safe_ratio)
            metrics["worst_v3_errors"] = worst_v3_groups(groups, args.print_worst, args.safe_ratio)
        if args.print_v3_false_safe > 0:
            metrics["v3_false_safe_groups"] = v3_false_safe_groups(groups, args.print_v3_false_safe, args.safe_ratio)
        if args.print_v3_false_unsafe > 0:
            metrics["v3_false_unsafe_groups"] = v3_false_unsafe_groups(groups, args.print_v3_false_unsafe, args.safe_ratio)
        if args.print_town_deployable_false_safe > 0:
            metrics["town_deployable_false_safe_groups"] = model_false_safe_groups(
                groups,
                args.print_town_deployable_false_safe,
                town_deployable_model,
                args.town_deployable_safe_probability,
                args.safe_ratio,
            )
        if args.print_town_deployable_false_unsafe > 0:
            metrics["town_deployable_false_unsafe_groups"] = model_false_unsafe_groups(
                groups,
                args.print_town_deployable_false_unsafe,
                town_deployable_model,
                args.town_deployable_safe_probability,
                args.safe_ratio,
            )

    if args.json:
        print(json.dumps(metrics, indent=2, sort_keys=True))
    else:
        print(f"rows: {row_count}")
        print(f"scope: {args.scope}")
        print(f"group key: {args.group_key}")
        print(f"rows after filter: {metrics['rows_after_filter']}")
        print(f"schemas: {dict(schema_counts)}")
        print(f"groups after filter: {len(groups)}")
        for name in ["train", "test"]:
            summary = metrics[name]
            print(f"{name}: rows={summary['rows']} groups={summary['groups']}")
            print(
                "  baseline "
                f"accuracy={summary['baseline_accuracy']:.4f} "
                f"brier={summary['baseline_brier']:.4f} "
                f"loss_mae={summary['baseline_loss_mae']:.4f} "
                f"loss_rmse={summary['baseline_loss_rmse']:.4f}"
            )
            deployed_summary = summary["deployed_danger"]
            print(
                "  deployed-danger "
                f"accuracy={deployed_summary['accuracy']:.4f} "
                f"brier={deployed_summary['brier']:.4f} "
                f"false_safe={deployed_summary['false_safe_groups']} "
                f"false_unsafe={deployed_summary['false_unsafe_groups']} "
                f"safe_groups={deployed_summary['safe_groups']}"
            )
            if full_model:
                print(
                    "  fitted "
                    f"accuracy={summary['model_accuracy']:.4f} "
                    f"brier={summary['model_brier']:.4f}"
                )
                ratio_summary = metrics[f"ratio_{name}"]
                print(
                    "  ratio-fitted "
                    f"accuracy={ratio_summary['model_accuracy']:.4f} "
                    f"brier={ratio_summary['model_brier']:.4f}"
                )
                compatible_summary = metrics[f"v3_compatible_{name}"]
                print(
                    "  v3-compatible-fitted "
                    f"accuracy={compatible_summary['model_accuracy']:.4f} "
                    f"brier={compatible_summary['model_brier']:.4f}"
                )
                town_deployable_summary = metrics[f"town_deployable_{name}"]
                print(
                    "  town-deployable-fitted "
                    f"accuracy={town_deployable_summary['model_accuracy']:.4f} "
                    f"brier={town_deployable_summary['model_brier']:.4f}"
                )
                town_rich_deployable_summary = metrics[f"town_rich_deployable_{name}"]
                print(
                    "  town-rich-deployable-fitted "
                    f"accuracy={town_rich_deployable_summary['model_accuracy']:.4f} "
                    f"brier={town_rich_deployable_summary['model_brier']:.4f}"
                )
                if "skill_spell_train" in metrics:
                    skill_spell_summary = metrics[f"skill_spell_{name}"]
                    print(
                        "  skill-spell-fitted "
                        f"accuracy={skill_spell_summary['model_accuracy']:.4f} "
                        f"brier={skill_spell_summary['model_brier']:.4f}"
                    )
                if "composition_train" in metrics:
                    composition_summary = metrics[f"composition_{name}"]
                    print(
                        "  composition-fitted "
                        f"accuracy={composition_summary['model_accuracy']:.4f} "
                        f"brier={composition_summary['model_brier']:.4f}"
                    )
            print(
                "  cxx-v3 "
                f"accuracy={summary['v3_accuracy']:.4f} "
                f"brier={summary['v3_brier']:.4f} "
                f"false_safe={summary['v3_false_safe_groups']} "
                f"false_unsafe={summary['v3_false_unsafe_groups']}"
            )
            print(
                "  cxx-v3 deployed-scope "
                f"rows={summary['v3_deployed_rows']} groups={summary['v3_deployed_groups']} "
                f"accuracy={summary['v3_deployed_accuracy']:.4f} "
                f"brier={summary['v3_deployed_brier']:.4f} "
                f"false_safe={summary['v3_deployed_false_safe_groups']} "
                f"false_unsafe={summary['v3_deployed_false_unsafe_groups']}"
            )
            print(
                "  false_safe_groups="
                f"{summary['baseline_false_safe_groups']} "
                "false_unsafe_groups="
                f"{summary['baseline_false_unsafe_groups']}"
            )
        if town_danger_factors:
            print("town danger factor diagnostics:")
            for name in ["train", "test"]:
                print(f"  {name}:")
                for item in metrics["town_danger_factors"][name]:
                    print(
                        f"    factor={item['factor']:.4f} "
                        f"accuracy={item['accuracy']:.4f} "
                        f"brier={item['brier']:.4f} "
                        f"false_safe={item['false_safe_groups']} "
                        f"false_unsafe={item['false_unsafe_groups']} "
                        f"safe_groups={item['safe_groups']}"
                    )
        if full_model and not args.summary_only:
            print("logistic model:")
            print(f"  intercept={full_model.intercept:.12g}")
            for item in metrics["logistic_model"]["features"]:
                print(
                    "  "
                    f"{item['name']}: coefficient={item['coefficient']:.12g} "
                    f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                )
            print("ratio logistic model:")
            print(f"  intercept={ratio_model.intercept:.12g}")
            for item in metrics["ratio_logistic_model"]["features"]:
                print(
                    "  "
                    f"{item['name']}: coefficient={item['coefficient']:.12g} "
                    f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                )
            print("v3-compatible model:")
            print(f"  intercept={v3_compatible_model.intercept:.12g}")
            for item in metrics["v3_compatible_model"]["features"]:
                print(
                    "  "
                    f"{item['name']}: coefficient={item['coefficient']:.12g} "
                    f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                )
            print("town-deployable model:")
            print(f"  intercept={town_deployable_model.intercept:.12g}")
            for item in metrics["town_deployable_model"]["features"]:
                print(
                    "  "
                    f"{item['name']}: coefficient={item['coefficient']:.12g} "
                    f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                )
            print("town-rich-deployable model:")
            print(f"  intercept={town_rich_deployable_model.intercept:.12g}")
            for item in metrics["town_rich_deployable_model"]["features"]:
                print(
                    "  "
                    f"{item['name']}: coefficient={item['coefficient']:.12g} "
                    f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                )
            if "skill_spell_feature_count" in metrics:
                print(f"skill/spell model: features={metrics['skill_spell_feature_count']}")
                print("skill/spell top features:")
                for item in metrics["skill_spell_top_features"]:
                    print(
                        "  "
                        f"{item['name']}: coefficient={item['coefficient']:.12g} "
                        f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                    )
            if "composition_feature_count" in metrics:
                print(f"composition model: features={metrics['composition_feature_count']}")
                print("composition top features:")
                for item in metrics["composition_top_features"]:
                    print(
                        "  "
                        f"{item['name']}: coefficient={item['coefficient']:.12g} "
                        f"mean={item['mean']:.12g} scale={item['scale']:.12g}"
                    )
            print("thresholds:")
            for name, summary in metrics["thresholds"].items():
                accuracy = summary["best_accuracy"]
                safety = summary["best_safety"]
                print(
                    f"  {name} best_accuracy threshold={accuracy['threshold']:.2f} "
                    f"accuracy={accuracy['accuracy']:.4f} false_safe={accuracy['false_safe_groups']} "
                    f"false_unsafe={accuracy['false_unsafe_groups']}"
                )
                print(
                    f"  {name} best_safety threshold={safety['threshold']:.2f} "
                    f"accuracy={safety['accuracy']:.4f} false_safe={safety['false_safe_groups']} "
                    f"false_unsafe={safety['false_unsafe_groups']}"
                )
            loss_model = metrics["loss_model"]
            factor_text = " ".join(f"{name} factor={value:.4f}" for name, value in sorted(loss_model["factors"].items()))
            print("loss model:")
            print(
                f"  exponent={loss_model['exponent']:.4f} "
                f"{factor_text} "
                f"mae={loss_model['mae']:.4f} rmse={loss_model['rmse']:.4f}"
            )
            if args.print_near_even > 0:
                print_group_report("near-even groups", metrics["near_even"])
            if args.print_worst > 0:
                print_group_report("worst fitted-model errors", metrics["worst_model_errors"])
                print_group_report("worst town-deployable-model errors", metrics["worst_town_deployable_errors"])
                print_group_report("worst town-rich-deployable-model errors", metrics["worst_town_rich_deployable_errors"])
                print_group_report("worst cxx-v3 errors", metrics["worst_v3_errors"])
            if args.print_v3_false_safe > 0:
                print_group_report("cxx-v3 false-safe groups", metrics["v3_false_safe_groups"])
            if args.print_v3_false_unsafe > 0:
                print_group_report("cxx-v3 false-unsafe groups", metrics["v3_false_unsafe_groups"])
            if args.print_town_deployable_false_safe > 0:
                print_group_report(
                    f"town-deployable false-safe groups threshold={args.town_deployable_safe_probability:.2f}",
                    metrics["town_deployable_false_safe_groups"],
                )
            if args.print_town_deployable_false_unsafe > 0:
                print_group_report(
                    f"town-deployable false-unsafe groups threshold={args.town_deployable_safe_probability:.2f}",
                    metrics["town_deployable_false_unsafe_groups"],
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
