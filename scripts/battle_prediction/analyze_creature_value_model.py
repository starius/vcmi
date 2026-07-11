#!/usr/bin/env python3
"""Fit per-creature strength multipliers for battle-outcome prediction.

This is an offline diagnostic tool. It answers whether v3's remaining error is
mostly explained by creature AI-value miscalibration, without changing global
creature values or running battles.
"""

from __future__ import annotations

import argparse
import math
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Any

from evaluate_nullkiller_predictor import (
    EPSILON,
    V3_FEATURES,
    V3_INTERCEPT,
    V3_LOG_STRENGTH_RATIO,
    Group,
    army_summary,
    battle_type,
    cxx_v3_feature_values,
    cxx_v3_probability,
    hero_strength,
    hero_summary,
    load_groups,
    scaled_cpp_feature,
    sigmoid,
    split_groups,
)


@dataclass
class PreparedGroup:
    group: Group
    base_score: float
    attacker_power: dict[int, float]
    defender_power: dict[int, float]
    pair_key: tuple[str, int, int]


@dataclass
class CreatureValueModel:
    ratio_bias: float
    ratio_coefficient: float
    multipliers: dict[int, float] = field(default_factory=dict)
    pair_biases: dict[tuple[str, int, int], float] = field(default_factory=dict)


@dataclass
class AdamState:
    first: float = 0.0
    second: float = 0.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Directory with battle-simulation JSONL shards")
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--epochs", type=int, default=600)
    parser.add_argument("--learning-rate", type=float, default=0.05)
    parser.add_argument("--l2", type=float, default=0.01)
    parser.add_argument("--pair-l2", type=float, default=0.05)
    parser.add_argument("--min-weight", type=int, default=20, help="Minimum grouped row weight for a creature to be trainable")
    parser.add_argument("--pair-min-weight", type=int, default=0, help="Minimum grouped row weight for a dominant creature pair bias")
    parser.add_argument("--safe-probability", type=float, default=0.60)
    parser.add_argument("--print-worst", type=int, default=8)
    parser.add_argument("--top-creatures", type=int, default=30)
    return parser.parse_args()


def army_power(row: dict[str, Any], side: str) -> dict[int, float]:
    result: dict[int, float] = defaultdict(float)
    for stack in row.get(f"{side}Army") or []:
        result[int(stack["creature"])] += float(stack.get("power") or 0.0)
    return dict(result)


def base_score_without_strength_ratio(row: dict[str, Any]) -> float:
    _, values = cxx_v3_feature_values(row)
    score = V3_INTERCEPT
    for value, feature in zip(values, V3_FEATURES):
        score += scaled_cpp_feature(value, feature)
    return score


def prepare_groups(groups: list[Group]) -> list[PreparedGroup]:
    result: list[PreparedGroup] = []
    for group in groups:
        attacker_power = army_power(group.row, "attacker")
        defender_power = army_power(group.row, "defender")
        result.append(
            PreparedGroup(
                group=group,
                base_score=base_score_without_strength_ratio(group.row),
                attacker_power=attacker_power,
                defender_power=defender_power,
                pair_key=dominant_pair_key(group.row, attacker_power, defender_power),
            )
        )
    return result


def collect_creature_ids(groups: list[Group], min_weight: int) -> set[int]:
    creature_weights: Counter[int] = Counter()
    for group in groups:
        ids = {
            int(stack["creature"])
            for side in ("attacker", "defender")
            for stack in group.row.get(f"{side}Army") or []
        }
        for creature_id in ids:
            creature_weights[creature_id] += group.count
    return {creature_id for creature_id, weight in creature_weights.items() if weight >= min_weight}


def dominant_creature(power_by_creature: dict[int, float]) -> int:
    if not power_by_creature:
        return -1
    return max(power_by_creature.items(), key=lambda item: (item[1], item[0]))[0]


def dominant_pair_key(row: dict[str, Any], attacker_power: dict[int, float], defender_power: dict[int, float]) -> tuple[str, int, int]:
    return (
        battle_type(row),
        dominant_creature(attacker_power),
        dominant_creature(defender_power),
    )


def collect_pair_keys(prepared_groups: list[PreparedGroup], min_weight: int) -> set[tuple[str, int, int]]:
    if min_weight <= 0:
        return set()

    pair_weights: Counter[tuple[str, int, int]] = Counter()
    for prepared in prepared_groups:
        pair_weights[prepared.pair_key] += prepared.group.count
    return {pair_key for pair_key, weight in pair_weights.items() if weight >= min_weight}


def adjusted_strength(power_by_creature: dict[int, float], model: CreatureValueModel) -> float:
    total = 0.0
    for creature_id, power in power_by_creature.items():
        total += power * math.exp(model.multipliers.get(creature_id, 0.0))
    return max(total, EPSILON)


def predict_prepared(prepared: PreparedGroup, model: CreatureValueModel) -> float:
    attacker = adjusted_strength(prepared.attacker_power, model) * max(hero_strength(prepared.group.row.get("attackerHero")), EPSILON)
    defender = adjusted_strength(prepared.defender_power, model) * max(hero_strength(prepared.group.row.get("defenderHero")), EPSILON)
    log_ratio = math.log(attacker / defender)
    pair_bias = model.pair_biases.get(prepared.pair_key, 0.0)
    return sigmoid(prepared.base_score + model.ratio_bias + model.ratio_coefficient * log_ratio + pair_bias)


def evaluate(name: str, prepared_groups: list[PreparedGroup], model: CreatureValueModel, safe_probability: float) -> dict[str, float]:
    rows = 0.0
    correct_50 = 0.0
    correct_safe = 0.0
    brier = 0.0
    false_safe = 0
    false_unsafe = 0
    by_type: Counter[str] = Counter()
    for prepared in prepared_groups:
        group = prepared.group
        weight = group.count
        probability = predict_prepared(prepared, model)
        actual_safe = group.win_rate >= 0.95
        predicted_safe = probability >= safe_probability
        rows += weight
        correct_50 += weight * ((probability >= 0.5) == (group.win_rate >= 0.5))
        correct_safe += weight * (predicted_safe == actual_safe)
        brier += weight * (probability - group.win_rate) ** 2
        if predicted_safe and not actual_safe:
            false_safe += 1
        if not predicted_safe and actual_safe:
            false_unsafe += 1
        by_type[f"{battle_type(group.row)}:{'safe' if predicted_safe else 'unsafe'}"] += 1

    return {
        "name": name,
        "rows": rows,
        "accuracy_50": correct_50 / rows if rows else 0.0,
        "safety_accuracy": correct_safe / rows if rows else 0.0,
        "brier": brier / rows if rows else 0.0,
        "false_safe_groups": false_safe,
        "false_unsafe_groups": false_unsafe,
        "by_type": dict(sorted(by_type.items())),
    }


def print_metrics(metrics: dict[str, Any]) -> None:
    print(
        f"{metrics['name']}: rows={metrics['rows']:.0f} "
        f"accuracy50={metrics['accuracy_50']:.4f} "
        f"safety_accuracy={metrics['safety_accuracy']:.4f} "
        f"brier={metrics['brier']:.5f} "
        f"false_safe={metrics['false_safe_groups']} "
        f"false_unsafe={metrics['false_unsafe_groups']}"
    )
    print(f"  by_type={metrics['by_type']}")


def update_adam(value: float, gradient: float, state: AdamState, step: int, learning_rate: float) -> float:
    beta1 = 0.9
    beta2 = 0.999
    state.first = beta1 * state.first + (1.0 - beta1) * gradient
    state.second = beta2 * state.second + (1.0 - beta2) * gradient * gradient
    corrected_first = state.first / (1.0 - beta1**step)
    corrected_second = state.second / (1.0 - beta2**step)
    return value - learning_rate * corrected_first / (math.sqrt(corrected_second) + 1e-8)


def train_model(
    prepared_groups: list[PreparedGroup],
    creature_ids: set[int],
    pair_keys: set[tuple[str, int, int]],
    epochs: int,
    learning_rate: float,
    l2: float,
    pair_l2: float,
) -> CreatureValueModel:
    ratio_coefficient = V3_LOG_STRENGTH_RATIO[0] / V3_LOG_STRENGTH_RATIO[2]
    ratio_bias = -ratio_coefficient * V3_LOG_STRENGTH_RATIO[1]
    model = CreatureValueModel(
        ratio_bias=ratio_bias,
        ratio_coefficient=ratio_coefficient,
        multipliers={creature_id: 0.0 for creature_id in sorted(creature_ids)},
        pair_biases={pair_key: 0.0 for pair_key in sorted(pair_keys)},
    )

    ratio_bias_state = AdamState()
    ratio_coefficient_state = AdamState()
    multiplier_states = {creature_id: AdamState() for creature_id in model.multipliers}
    pair_bias_states = {pair_key: AdamState() for pair_key in model.pair_biases}
    total_weight = sum(prepared.group.count for prepared in prepared_groups) or 1.0

    for epoch in range(1, epochs + 1):
        ratio_bias_gradient = 0.0
        ratio_coefficient_gradient = 0.0
        multiplier_gradients = {creature_id: 0.0 for creature_id in model.multipliers}
        pair_bias_gradients = {pair_key: 0.0 for pair_key in model.pair_biases}

        for prepared in prepared_groups:
            group = prepared.group
            attacker_army_total = adjusted_strength(prepared.attacker_power, model)
            defender_army_total = adjusted_strength(prepared.defender_power, model)
            attacker_total = attacker_army_total * max(hero_strength(prepared.group.row.get("attackerHero")), EPSILON)
            defender_total = defender_army_total * max(hero_strength(prepared.group.row.get("defenderHero")), EPSILON)
            log_ratio = math.log(attacker_total / defender_total)
            pair_bias = model.pair_biases.get(prepared.pair_key, 0.0)
            probability = sigmoid(prepared.base_score + model.ratio_bias + model.ratio_coefficient * log_ratio + pair_bias)
            error = (probability - group.win_rate) * group.count

            ratio_bias_gradient += error
            ratio_coefficient_gradient += error * log_ratio
            if prepared.pair_key in pair_bias_gradients:
                pair_bias_gradients[prepared.pair_key] += error

            for creature_id in (set(prepared.attacker_power) | set(prepared.defender_power)) & set(model.multipliers):
                attacker_share = prepared.attacker_power.get(creature_id, 0.0) * math.exp(model.multipliers[creature_id]) / attacker_army_total
                defender_share = prepared.defender_power.get(creature_id, 0.0) * math.exp(model.multipliers[creature_id]) / defender_army_total
                multiplier_gradients[creature_id] += error * model.ratio_coefficient * (attacker_share - defender_share)

        ratio_bias_gradient /= total_weight
        ratio_coefficient_gradient /= total_weight
        model.ratio_bias = update_adam(model.ratio_bias, ratio_bias_gradient, ratio_bias_state, epoch, learning_rate)
        model.ratio_coefficient = update_adam(
            model.ratio_coefficient,
            ratio_coefficient_gradient,
            ratio_coefficient_state,
            epoch,
            learning_rate,
        )

        for creature_id, gradient in multiplier_gradients.items():
            gradient = gradient / total_weight + l2 * model.multipliers[creature_id]
            updated = update_adam(model.multipliers[creature_id], gradient, multiplier_states[creature_id], epoch, learning_rate)
            model.multipliers[creature_id] = max(-1.5, min(1.5, updated))

        for pair_key, gradient in pair_bias_gradients.items():
            gradient = gradient / total_weight + pair_l2 * model.pair_biases[pair_key]
            updated = update_adam(model.pair_biases[pair_key], gradient, pair_bias_states[pair_key], epoch, learning_rate)
            model.pair_biases[pair_key] = max(-2.0, min(2.0, updated))

        if epoch in {1, epochs} or (epoch <= 50 and epoch % 10 == 0) or epoch % 100 == 0:
            metrics = evaluate(f"epoch {epoch}", prepared_groups, model, 0.60)
            print(
                f"epoch={epoch} train_brier={metrics['brier']:.5f} "
                f"accuracy50={metrics['accuracy_50']:.4f} safety_accuracy={metrics['safety_accuracy']:.4f}"
            )

    return model


def print_top_multipliers(model: CreatureValueModel, limit: int) -> None:
    print("top creature multipliers:")
    for creature_id, log_multiplier in sorted(model.multipliers.items(), key=lambda item: abs(item[1]), reverse=True)[:limit]:
        print(f"  creature={creature_id} log_multiplier={log_multiplier:.6f} multiplier={math.exp(log_multiplier):.4f}")


def print_top_pair_biases(model: CreatureValueModel, limit: int) -> None:
    if not model.pair_biases:
        return

    print("top dominant-pair biases:")
    for (sample_type, attacker, defender), bias in sorted(model.pair_biases.items(), key=lambda item: abs(item[1]), reverse=True)[:limit]:
        print(f"  type={sample_type} attacker={attacker} defender={defender} bias={bias:.6f}")


def print_worst(prepared_groups: list[PreparedGroup], model: CreatureValueModel, limit: int) -> None:
    if limit <= 0:
        return

    candidates = sorted(
        prepared_groups,
        key=lambda prepared: abs(predict_prepared(prepared, model) - prepared.group.win_rate),
        reverse=True,
    )
    print("worst adjusted-model errors:")
    for index, prepared in enumerate(candidates[:limit], start=1):
        group = prepared.group
        probability = predict_prepared(prepared, model)
        cxx_probability = cxx_v3_probability(group.row)
        print(
            f"  {index}. type={battle_type(group.row)} count={group.count} "
            f"win_rate={group.win_rate:.4f} adjusted={probability:.4f} "
            f"cxx_v3={cxx_probability:.4f} error={abs(probability - group.win_rate):.4f}"
        )
        print(f"     attacker: {hero_summary(group.row.get('attackerHero'))} army={army_summary(group.row, 'attacker')}")
        print(f"     defender: {hero_summary(group.row.get('defenderHero'))} army={army_summary(group.row, 'defender')}")


def main() -> int:
    args = parse_args()
    groups, schema_counts, rows = load_groups(args.dataset)
    train, test = split_groups(groups, args.test_fraction)
    creature_ids = collect_creature_ids(train, args.min_weight)
    train_prepared = prepare_groups(train)
    test_prepared = prepare_groups(test)
    pair_keys = collect_pair_keys(train_prepared, args.pair_min_weight)

    print(f"dataset={args.dataset} rows={rows} groups={len(groups)} schema_counts={schema_counts}")
    print(
        f"train_groups={len(train)} test_groups={len(test)} "
        f"trainable_creatures={len(creature_ids)} trainable_pairs={len(pair_keys)}"
    )

    baseline = CreatureValueModel(
        ratio_bias=-(V3_LOG_STRENGTH_RATIO[0] / V3_LOG_STRENGTH_RATIO[2]) * V3_LOG_STRENGTH_RATIO[1],
        ratio_coefficient=V3_LOG_STRENGTH_RATIO[0] / V3_LOG_STRENGTH_RATIO[2],
        multipliers={creature_id: 0.0 for creature_id in creature_ids},
        pair_biases={pair_key: 0.0 for pair_key in pair_keys},
    )
    print_metrics(evaluate("baseline-train", train_prepared, baseline, args.safe_probability))
    print_metrics(evaluate("baseline-test", test_prepared, baseline, args.safe_probability))

    model = train_model(train_prepared, creature_ids, pair_keys, args.epochs, args.learning_rate, args.l2, args.pair_l2)
    print(
        f"fitted ratio_bias={model.ratio_bias:.12g} "
        f"ratio_coefficient={model.ratio_coefficient:.12g}"
    )
    print_metrics(evaluate("adjusted-train", train_prepared, model, args.safe_probability))
    print_metrics(evaluate("adjusted-test", test_prepared, model, args.safe_probability))
    print_top_multipliers(model, args.top_creatures)
    print_top_pair_biases(model, args.top_creatures)
    print_worst(test_prepared, model, args.print_worst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
