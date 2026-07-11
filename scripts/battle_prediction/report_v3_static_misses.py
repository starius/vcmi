#!/usr/bin/env python3
"""Fast non-fitting diagnostics for current Nullkiller2 v3 static predictions."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from typing import Any

from evaluate_nullkiller_predictor import (
    SAFE_ATTACK_RATIO,
    V3_SAFE_PROBABILITY,
    army_rich_stats,
    battle_type,
    combat_spell_count,
    compact_group_summary,
    cxx_v3_probability,
    deployed_safe_prediction,
    filter_complete_shard_groups,
    load_groups,
    load_shard_manifest,
    matches_scope,
    primary,
    print_group_report,
    raw_mana,
    raw_mana_limit,
    secondary_skill_count,
    side_strength,
    split_groups,
    summarize_deployed_danger,
    summarize_predictions,
    town_bool,
    town_feature,
    town_fortification,
    town_pre_merge_not_in_battle_share,
    town_pre_merge_participating_share,
    worst_v3_groups,
    Group,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--group-key", choices=["setup", "shard"], default="shard")
    parser.add_argument("--complete-shards-only", action="store_true")
    parser.add_argument("--min-group-size", type=int, default=1)
    parser.add_argument("--scope", choices=["all", "deployed-static", "town"], default="town")
    parser.add_argument("--safe-ratio", type=float, default=SAFE_ATTACK_RATIO)
    parser.add_argument("--safe-probability", type=float, default=V3_SAFE_PROBABILITY)
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--limit", type=int, default=12)
    parser.add_argument("--segments", type=int, default=24)
    parser.add_argument("--close-even-min", type=float, default=0.25)
    parser.add_argument("--close-even-max", type=float, default=0.75)
    parser.add_argument("--close-even-min-error", type=float, default=0.25)
    parser.add_argument("--town-danger-factors", default="", help="Comma-separated deployed town danger factors to summarize")
    parser.add_argument("--probe-town-guards", action="store_true", help="Probe compact town safety guards derived from miss segments")
    parser.add_argument("--guard-results", type=int, default=12, help="Number of town guard candidates to print")
    parser.add_argument("--json", action="store_true", help="Print machine-readable output")
    return parser.parse_args()


def parse_float_list(value: str) -> list[float]:
    return [float(part.strip()) for part in value.split(",") if part.strip()]


def load_filtered_groups(args: argparse.Namespace) -> tuple[list[Group], Counter, int]:
    groups, schema_counts, row_count = load_groups(args.dataset, args.group_key)
    if args.complete_shards_only:
        if args.group_key != "shard":
            raise SystemExit("--complete-shards-only requires --group-key shard")
        manifest = load_shard_manifest(args.dataset)
        if not manifest:
            raise SystemExit("--complete-shards-only requires manifest.jsonl in the dataset")
        groups = filter_complete_shard_groups(groups, manifest)

    groups = [
        group
        for group in groups
        if group.count >= args.min_group_size and matches_scope(group.row, args.scope)
    ]
    return groups, schema_counts, row_count


def close_even_v3_misses(
    groups: list[Group],
    limit: int,
    actual_min: float,
    actual_max: float,
    min_error: float,
    safe_ratio: float,
) -> list[dict[str, Any]]:
    candidates = [
        group
        for group in groups
        if actual_min <= group.win_rate <= actual_max
        and abs(cxx_v3_probability(group.row) - group.win_rate) >= min_error
    ]
    candidates.sort(key=lambda group: (abs(cxx_v3_probability(group.row) - group.win_rate), group.count), reverse=True)

    result = []
    for group in candidates[:limit]:
        summary = compact_group_summary(group, None, safe_ratio)
        probability = cxx_v3_probability(group.row)
        summary["model_probability"] = probability
        summary["model_error"] = abs(probability - group.win_rate)
        result.append(summary)
    return result


def high_share(stats: dict[str, float], key: str, threshold: float) -> bool:
    return float(stats.get(key) or 0.0) >= threshold


def segment_names(row: dict[str, Any]) -> list[str]:
    attacker_hero = row.get("attackerHero")
    defender_hero = row.get("defenderHero")
    town = row.get("defendedTown") or {}
    attacker_rich = army_rich_stats(row, "attacker")
    defender_rich = army_rich_stats(row, "defender")

    tags: list[str] = [f"type={battle_type(row)}"]

    ratio = side_strength(row, "attacker") / max(side_strength(row, "defender"), 1.0)
    if ratio < 0.9:
        tags.append("strength_ratio<0.90")
    elif ratio < 1.1:
        tags.append("strength_ratio=0.90..1.10")
    elif ratio < 1.4:
        tags.append("strength_ratio=1.10..1.40")
    else:
        tags.append("strength_ratio>=1.40")

    if town:
        fort_level = int(town_feature(row, "fortLevel"))
        mage_level = int(town_feature(row, "mageGuildLevel"))
        buildings = town.get("buildings") or []
        tags.append(f"town.faction={int(town_feature(row, 'faction'))}")
        tags.append(f"town.fort={fort_level}")
        tags.append(f"town.mageGuild={mage_level}")
        tags.append(f"town.buildings={len(buildings)}")
        if fort_level >= 2:
            tags.append("town.has_citadel_or_castle")
        if fort_level >= 3:
            tags.append("town.has_castle")
        if town_fortification(row, "hasMoat"):
            tags.append("town.has_moat")
        if town_fortification(row, "wallsHealth") >= 3:
            tags.append("town.full_walls")
        if town_fortification(row, "upperTowerHealth") + town_fortification(row, "lowerTowerHealth") > 0:
            tags.append("town.side_towers_alive")
        if town_bool(row, "hasBuiltTavern"):
            tags.append("town.has_tavern")
        if town_bool(row, "hasBuiltGrail"):
            tags.append("town.has_grail")
        if town.get("defendingHeroSource"):
            tags.append(f"town.hero_source={town.get('defendingHeroSource')}")
        if town.get("hasVisitingHero"):
            tags.append("town.has_visiting_hero")
        if town.get("hasGarrisonHero"):
            tags.append("town.has_garrison_hero")

        not_in_battle = town_pre_merge_not_in_battle_share(row)
        participating = town_pre_merge_participating_share(row)
        if not_in_battle >= 0.25:
            tags.append("premerge.not_in_battle>=25%")
        elif not_in_battle >= 0.10:
            tags.append("premerge.not_in_battle=10..25%")
        if participating < 0.75:
            tags.append("premerge.participating<75%")

    mana_diff = raw_mana(attacker_hero) - raw_mana(defender_hero)
    mana_limit_diff = raw_mana_limit(attacker_hero) - raw_mana_limit(defender_hero)
    spell_count_diff = combat_spell_count(attacker_hero) - combat_spell_count(defender_hero)
    skill_count_diff = secondary_skill_count(attacker_hero) - secondary_skill_count(defender_hero)
    power_diff = primary(attacker_hero, 2) - primary(defender_hero, 2)
    knowledge_diff = primary(attacker_hero, 3) - primary(defender_hero, 3)

    if mana_diff <= -50:
        tags.append("defender.current_mana_adv>=50")
    elif mana_diff <= -20:
        tags.append("defender.current_mana_adv=20..50")
    elif mana_diff >= 20:
        tags.append("attacker.current_mana_adv>=20")
    if mana_limit_diff <= -75:
        tags.append("defender.mana_limit_adv>=75")
    elif mana_limit_diff >= 75:
        tags.append("attacker.mana_limit_adv>=75")
    if spell_count_diff <= -4:
        tags.append("defender.combat_spells_adv>=4")
    elif spell_count_diff <= -2:
        tags.append("defender.combat_spells_adv=2..4")
    elif spell_count_diff >= 2:
        tags.append("attacker.combat_spells_adv>=2")
    if skill_count_diff <= -2:
        tags.append("defender.secondary_adv>=2")
    elif skill_count_diff >= 2:
        tags.append("attacker.secondary_adv>=2")
    if power_diff <= -5:
        tags.append("defender.spell_power_adv>=5")
    elif power_diff >= 5:
        tags.append("attacker.spell_power_adv>=5")
    if knowledge_diff <= -5:
        tags.append("defender.knowledge_adv>=5")
    elif knowledge_diff >= 5:
        tags.append("attacker.knowledge_adv>=5")
    if attacker_hero and attacker_hero.get("hasSpellbook"):
        tags.append("attacker.has_spellbook")
    if defender_hero and defender_hero.get("hasSpellbook"):
        tags.append("defender.has_spellbook")

    for side, stats in (("attacker", attacker_rich), ("defender", defender_rich)):
        if high_share(stats, "shooter_share", 0.35):
            tags.append(f"{side}.shooter_share>=35%")
        if high_share(stats, "flying_share", 0.35):
            tags.append(f"{side}.flying_share>=35%")
        if high_share(stats, "spellcaster_share", 0.20):
            tags.append(f"{side}.spellcaster_share>=20%")
        if high_share(stats, "blocksRetaliation_share", 0.25):
            tags.append(f"{side}.no_retaliation_share>=25%")
        if float(stats.get("max_speed") or 0.0) >= 12:
            tags.append(f"{side}.max_speed>=12")

    return tags


def summarize_segments(groups: list[Group], limit: int) -> list[dict[str, Any]]:
    counters: dict[str, dict[str, Any]] = defaultdict(
        lambda: {
            "groups": 0,
            "rows": 0,
            "win_sum": 0.0,
            "pred_sum": 0.0,
            "error_sum": 0.0,
        }
    )
    for group in groups:
        prediction = cxx_v3_probability(group.row)
        for tag in segment_names(group.row):
            item = counters[tag]
            item["groups"] += 1
            item["rows"] += group.count
            item["win_sum"] += group.count * group.win_rate
            item["pred_sum"] += group.count * prediction
            item["error_sum"] += group.count * (prediction - group.win_rate)

    result = []
    for tag, item in counters.items():
        rows = item["rows"]
        result.append(
            {
                "segment": tag,
                "groups": item["groups"],
                "rows": rows,
                "avg_win_rate": item["win_sum"] / rows if rows else 0.0,
                "avg_cxx_v3": item["pred_sum"] / rows if rows else 0.0,
                "avg_signed_error": item["error_sum"] / rows if rows else 0.0,
            }
        )
    result.sort(key=lambda item: (abs(item["avg_signed_error"]) * item["rows"], item["rows"]), reverse=True)
    return result[:limit]


def false_safe_source_groups(groups: list[Group], safe_probability: float) -> list[Group]:
    return [
        group
        for group in groups
        if cxx_v3_probability(group.row) >= safe_probability and group.win_rate < 0.95
    ]


def false_unsafe_source_groups(groups: list[Group], safe_probability: float) -> list[Group]:
    return [
        group
        for group in groups
        if cxx_v3_probability(group.row) < safe_probability and group.win_rate >= 0.95
    ]


def hero_diff(row: dict[str, Any], function: Any) -> float:
    return float(function(row.get("attackerHero")) - function(row.get("defenderHero")))


def defender_combat_spell_advantage(row: dict[str, Any], value: int) -> bool:
    return hero_diff(row, combat_spell_count) <= -value


def attacker_combat_spell_advantage(row: dict[str, Any], value: int) -> bool:
    return hero_diff(row, combat_spell_count) >= value


def defender_mana_advantage(row: dict[str, Any], value: int) -> bool:
    return hero_diff(row, raw_mana) <= -value


def attacker_spell_power_advantage(row: dict[str, Any], value: int) -> bool:
    return primary(row.get("attackerHero"), 2) - primary(row.get("defenderHero"), 2) >= value


def town_fort_at_least(row: dict[str, Any], value: int) -> bool:
    return town_feature(row, "fortLevel") >= value


def town_mage_guild_at_least(row: dict[str, Any], value: int) -> bool:
    return town_feature(row, "mageGuildLevel") >= value


def town_has_moat(row: dict[str, Any]) -> bool:
    return bool(town_fortification(row, "hasMoat"))


def town_has_full_walls(row: dict[str, Any]) -> bool:
    return town_fortification(row, "wallsHealth") >= 3


def town_guard_candidates() -> dict[str, Any]:
    return {
        "none": lambda row: False,
        "def_spells4": lambda row: defender_combat_spell_advantage(row, 4),
        "def_spells4_or_fort2_moat": lambda row: defender_combat_spell_advantage(row, 4)
        or (town_fort_at_least(row, 2) and town_has_moat(row)),
        "def_spells4_or_castle": lambda row: defender_combat_spell_advantage(row, 4) or town_fort_at_least(row, 3),
        "def_spells2_mage2_or_fort2_moat": lambda row: (
            defender_combat_spell_advantage(row, 2)
            and town_mage_guild_at_least(row, 2)
        )
        or (town_fort_at_least(row, 2) and town_has_moat(row)),
        "def_spells4_or_fullwalls": lambda row: defender_combat_spell_advantage(row, 4) or town_has_full_walls(row),
        "def_spells4_or_premerge10": lambda row: defender_combat_spell_advantage(row, 4)
        or town_pre_merge_not_in_battle_share(row) >= 0.10,
        "def_spells4_or_mana50_castle": lambda row: defender_combat_spell_advantage(row, 4)
        or (defender_mana_advantage(row, 50) and town_fort_at_least(row, 3)),
    }


def town_override_candidates() -> dict[str, Any]:
    return {
        "none": lambda row: False,
        "att_spells2_power5": lambda row: attacker_combat_spell_advantage(row, 2)
        and attacker_spell_power_advantage(row, 5),
        "att_spells2": lambda row: attacker_combat_spell_advantage(row, 2),
        "att_spells2_not_castle": lambda row: attacker_combat_spell_advantage(row, 2)
        and not town_fort_at_least(row, 3),
    }


def summarize_guard_prediction(
    label: str,
    groups: list[Group],
    factor: float,
    guard_name: str,
    override_name: str,
) -> dict[str, Any]:
    guard = town_guard_candidates()[guard_name]
    override = town_override_candidates()[override_name]
    rows = sum(group.count for group in groups)
    correct = 0.0
    brier = 0.0
    false_safe = 0
    false_unsafe = 0
    safe_groups = 0

    for group in groups:
        actual_class = group.win_rate >= 0.5
        safe = deployed_safe_prediction(group.row, SAFE_ATTACK_RATIO, factor)
        if guard(group.row):
            safe = False
        if override(group.row):
            safe = True
        probability = 1.0 if safe else 0.0
        correct += group.count * (safe == actual_class)
        brier += group.count * (probability - group.win_rate) ** 2
        false_safe += int(safe and group.win_rate < 0.95)
        false_unsafe += int((not safe) and group.win_rate >= 0.95)
        safe_groups += int(safe)

    return {
        "label": label,
        "factor": factor,
        "guard": guard_name,
        "override": override_name,
        "rows": rows,
        "groups": len(groups),
        "accuracy": correct / rows if rows else 0.0,
        "brier": brier / rows if rows else 0.0,
        "false_safe": false_safe,
        "false_unsafe": false_unsafe,
        "safe_groups": safe_groups,
    }


def probe_town_guard_rules(
    all_groups: list[Group],
    train: list[Group],
    test: list[Group],
    factors: list[float],
    limit: int,
) -> list[dict[str, Any]]:
    guards = town_guard_candidates()
    overrides = town_override_candidates()
    results = []

    for factor in factors:
        for guard_name in guards:
            for override_name in overrides:
                all_summary = summarize_guard_prediction("all", all_groups, factor, guard_name, override_name)
                train_summary = summarize_guard_prediction("train", train, factor, guard_name, override_name)
                test_summary = summarize_guard_prediction("test", test, factor, guard_name, override_name)
                score = (
                    test_summary["false_safe"] == 0,
                    all_summary["false_safe"] == 0,
                    test_summary["accuracy"],
                    all_summary["accuracy"],
                    -test_summary["false_unsafe"],
                    -all_summary["false_unsafe"],
                    test_summary["safe_groups"],
                )
                results.append(
                    {
                        "score": score,
                        "all": all_summary,
                        "train": train_summary,
                        "test": test_summary,
                    }
                )

    results.sort(key=lambda item: item["score"], reverse=True)
    for item in results:
        del item["score"]
    return results[:limit]


def v3_false_safe_report_groups(groups: list[Group], limit: int, safe_ratio: float, safe_probability: float) -> list[dict[str, Any]]:
    candidates = false_safe_source_groups(groups, safe_probability)
    candidates.sort(key=lambda group: (cxx_v3_probability(group.row) - group.win_rate, group.count), reverse=True)

    result = []
    for group in candidates[:limit]:
        summary = compact_group_summary(group, None, safe_ratio)
        probability = cxx_v3_probability(group.row)
        summary["model_probability"] = probability
        summary["model_error"] = probability - group.win_rate
        result.append(summary)
    return result


def v3_false_unsafe_report_groups(groups: list[Group], limit: int, safe_ratio: float, safe_probability: float) -> list[dict[str, Any]]:
    candidates = false_unsafe_source_groups(groups, safe_probability)
    candidates.sort(key=lambda group: (group.win_rate - cxx_v3_probability(group.row), group.count), reverse=True)

    result = []
    for group in candidates[:limit]:
        summary = compact_group_summary(group, None, safe_ratio)
        probability = cxx_v3_probability(group.row)
        summary["model_probability"] = probability
        summary["model_error"] = group.win_rate - probability
        result.append(summary)
    return result


def metrics_for_groups(groups: list[Group], safe_ratio: float, town_danger_factors: list[float]) -> dict[str, Any]:
    metrics = summarize_predictions(groups, safe_ratio, None)
    if town_danger_factors:
        metrics["town_danger_factors"] = [
            summarize_deployed_danger(groups, safe_ratio, factor)
            for factor in town_danger_factors
        ]
    return metrics


def print_summary(name: str, metrics: dict[str, Any]) -> None:
    print(f"{name}: rows={metrics['rows']} groups={metrics['groups']}")
    print(
        "  cxx-v3 "
        f"accuracy={metrics['v3_accuracy']:.4f} "
        f"brier={metrics['v3_brier']:.4f} "
        f"false_safe={metrics['v3_false_safe_groups']} "
        f"false_unsafe={metrics['v3_false_unsafe_groups']} "
        f"safe_groups={metrics['v3_safe_groups']}"
    )
    deployed = metrics["deployed_danger"]
    print(
        "  deployed-danger "
        f"accuracy={deployed['accuracy']:.4f} "
        f"brier={deployed['brier']:.4f} "
        f"false_safe={deployed['false_safe_groups']} "
        f"false_unsafe={deployed['false_unsafe_groups']} "
        f"safe_groups={deployed['safe_groups']}"
    )
    for item in metrics.get("town_danger_factors", []):
        print(
            "  deployed-danger "
            f"factor={item['factor']:.4f} "
            f"accuracy={item['accuracy']:.4f} "
            f"brier={item['brier']:.4f} "
            f"false_safe={item['false_safe_groups']} "
            f"false_unsafe={item['false_unsafe_groups']} "
            f"safe_groups={item['safe_groups']}"
        )


def main() -> int:
    args = parse_args()
    town_danger_factors = parse_float_list(args.town_danger_factors)
    guard_factors = town_danger_factors or [1.0, 1.1, 1.25, 1.4, 1.5, 1.75, 2.0]
    groups, schema_counts, row_count = load_filtered_groups(args)
    train, test = split_groups(groups, args.test_fraction)
    false_safe = false_safe_source_groups(groups, args.safe_probability)
    false_unsafe = false_unsafe_source_groups(groups, args.safe_probability)
    close_even = close_even_v3_misses(
        groups,
        args.limit,
        args.close_even_min,
        args.close_even_max,
        args.close_even_min_error,
        args.safe_ratio,
    )

    metrics = {
        "dataset": args.dataset,
        "scope": args.scope,
        "group_key": args.group_key,
        "complete_shards_only": args.complete_shards_only,
        "input_rows": row_count,
        "schema_counts": dict(schema_counts),
        "rows_after_filter": sum(group.count for group in groups),
        "groups_after_filter": len(groups),
        "all": metrics_for_groups(groups, args.safe_ratio, town_danger_factors),
        "train": metrics_for_groups(train, args.safe_ratio, town_danger_factors),
        "test": metrics_for_groups(test, args.safe_ratio, town_danger_factors),
        "close_even_v3_misses": close_even,
        "worst_v3_groups": worst_v3_groups(groups, args.limit, args.safe_ratio),
        "v3_false_safe_groups": v3_false_safe_report_groups(groups, args.limit, args.safe_ratio, args.safe_probability),
        "v3_false_unsafe_groups": v3_false_unsafe_report_groups(groups, args.limit, args.safe_ratio, args.safe_probability),
        "false_safe_segments": summarize_segments(false_safe, args.segments),
        "false_unsafe_segments": summarize_segments(false_unsafe, args.segments),
    }
    if args.probe_town_guards:
        metrics["town_guard_probe"] = probe_town_guard_rules(groups, train, test, guard_factors, args.guard_results)

    if args.json:
        print(json.dumps(metrics, indent=2, sort_keys=True))
        return 0

    print(f"rows: {row_count}")
    print(f"scope: {args.scope}")
    print(f"group key: {args.group_key}")
    print(f"complete shards only: {args.complete_shards_only}")
    print(f"rows after filter: {metrics['rows_after_filter']}")
    print(f"schemas: {dict(schema_counts)}")
    print(f"groups after filter: {len(groups)}")
    print_summary("all", metrics["all"])
    print_summary("train", metrics["train"])
    print_summary("test", metrics["test"])

    print_group_report(
        f"close-even cxx-v3 misses actual=[{args.close_even_min:.2f},{args.close_even_max:.2f}] min_error={args.close_even_min_error:.2f}",
        metrics["close_even_v3_misses"],
    )
    print_group_report("worst cxx-v3 groups", metrics["worst_v3_groups"])
    print_group_report("cxx-v3 false-safe groups", metrics["v3_false_safe_groups"])
    print_group_report("cxx-v3 false-unsafe groups", metrics["v3_false_unsafe_groups"])

    for title, rows in (
        ("false-safe segments", metrics["false_safe_segments"]),
        ("false-unsafe segments", metrics["false_unsafe_segments"]),
    ):
        print(title + ":")
        for index, item in enumerate(rows, start=1):
            print(
                f"  {index}. {item['segment']} "
                f"groups={item['groups']} rows={item['rows']} "
                f"avg_win={item['avg_win_rate']:.4f} "
                f"avg_cxx_v3={item['avg_cxx_v3']:.4f} "
                f"avg_signed_error={item['avg_signed_error']:.4f}"
            )

    if args.probe_town_guards:
        print("town guard prototype candidates:")
        for index, item in enumerate(metrics["town_guard_probe"], start=1):
            all_summary = item["all"]
            print(
                f"  {index}. factor={all_summary['factor']:.2f} "
                f"guard={all_summary['guard']} override={all_summary['override']}"
            )
            for name in ("all", "train", "test"):
                summary = item[name]
                print(
                    f"     {name}: accuracy={summary['accuracy']:.4f} "
                    f"brier={summary['brier']:.4f} "
                    f"false_safe={summary['false_safe']} "
                    f"false_unsafe={summary['false_unsafe']} "
                    f"safe_groups={summary['safe_groups']}/{summary['groups']}"
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
