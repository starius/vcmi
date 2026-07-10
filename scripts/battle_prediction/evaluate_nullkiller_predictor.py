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
    parser.add_argument("--min-group-size", type=int, default=1)
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--epochs", type=int, default=2500)
    parser.add_argument("--learning-rate", type=float, default=0.05)
    parser.add_argument("--l2", type=float, default=0.001)
    parser.add_argument("--json", action="store_true", help="Print machine-readable metrics")
    return parser.parse_args()


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


def mana_ratio(hero: dict[str, Any] | None) -> float:
    if not hero:
        return 0.0
    limit = float(hero.get("manaLimit") or 0)
    if limit <= 0:
        return 0.0
    return float(hero.get("mana") or 0) / limit


def primary(hero: dict[str, Any] | None, index: int) -> float:
    if not hero:
        return 0.0
    values = hero.get("primary") or [0, 0, 0, 0]
    return float(values[index])


def feature_vector(row: dict[str, Any]) -> list[float]:
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    attacker_army = max(float(row.get("attackerArmyStrength") or 0), 1.0)
    defender_army = max(float(row.get("defenderArmyStrength") or 0), 1.0)
    attacker_stats = army_stats(row, "attacker")
    defender_stats = army_stats(row, "defender")
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
        1.0 if attacker_hero and attacker_hero.get("hasSpellbook") else 0.0,
        1.0 if defender_hero and defender_hero.get("hasSpellbook") else 0.0,
    ]


def ratio_feature_vector(row: dict[str, Any]) -> list[float]:
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    return [math.log(attacker / defender)]


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
    "attacker_spellbook",
    "defender_spellbook",
]

RATIO_FEATURE_NAMES = ["log_strength_ratio"]


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
        }

    stable = {
        "terrain": row.get("terrain"),
        "battlefield": row.get("battlefield"),
        "attackerHero": clean_hero(row.get("attackerHero")),
        "defenderHero": clean_hero(row.get("defenderHero")),
        "attackerArmyStrength": row.get("attackerArmyStrength"),
        "defenderArmyStrength": row.get("defenderArmyStrength"),
        "attackerArmy": row.get("attackerArmy"),
        "defenderArmy": row.get("defenderArmy"),
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


def load_groups(path: str) -> tuple[list[Group], Counter, int]:
    groups: dict[str, Group] = {}
    schema_counts: Counter = Counter()
    rows = 0
    for row in iter_json_lines(path):
        rows += 1
        schema_counts[row.get("schema", 1)] += 1
        key = setup_key(row)
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


def current_loss_prediction(row: dict[str, Any]) -> float:
    attacker = max(side_strength(row, "attacker"), EPSILON)
    defender = max(side_strength(row, "defender"), EPSILON)
    return max(0.0, min(1.0, (defender / attacker) ** 2))


def summarize_predictions(groups: list[Group], safe_ratio: float, model: LogisticModel | None = None) -> dict[str, Any]:
    rows = sum(group.count for group in groups)
    baseline_correct = 0.0
    baseline_brier = 0.0
    model_correct = 0.0
    model_brier = 0.0
    loss_abs = []
    loss_squared = []
    false_safe = 0
    false_unsafe = 0
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

        loss_prediction = current_loss_prediction(row)
        loss_abs.append(abs(loss_prediction - group.attacker_loss_ratio))
        loss_squared.append((loss_prediction - group.attacker_loss_ratio) ** 2)
        battle_type = "hero-v-hero" if row.get("defenderHero") else "hero-v-monster"
        by_type[(battle_type, "safe" if safe else "unsafe")] += 1

    result = {
        "rows": rows,
        "groups": len(groups),
        "baseline_accuracy": baseline_correct / rows if rows else 0.0,
        "baseline_brier": baseline_brier / rows if rows else 0.0,
        "baseline_false_safe_groups": false_safe,
        "baseline_false_unsafe_groups": false_unsafe,
        "baseline_loss_mae": statistics.fmean(loss_abs) if loss_abs else 0.0,
        "baseline_loss_rmse": math.sqrt(statistics.fmean(loss_squared)) if loss_squared else 0.0,
        "baseline_by_type": {f"{key[0]}:{key[1]}": value for key, value in sorted(by_type.items())},
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
        battle_type = "hero-v-hero" if row.get("defenderHero") else "hero-v-monster"
        samples.append((defender / attacker, group.attacker_loss_ratio, battle_type))

    def evaluate_formula(exponent: float, factors: dict[str, float]) -> dict[str, Any]:
        squared = []
        absolute = []
        for ratio, expected, battle_type in samples:
            predicted = max(0.0, min(1.0, factors[battle_type] * (ratio ** exponent)))
            squared.append((predicted - expected) ** 2)
            absolute.append(abs(predicted - expected))
        return {
            "rmse": math.sqrt(statistics.fmean(squared)) if squared else 0.0,
            "mae": statistics.fmean(absolute) if absolute else 0.0,
            "exponent": exponent,
            "factors": dict(factors),
        }

    best = evaluate_formula(2.0, {"hero-v-monster": 1.0, "hero-v-hero": 1.0})
    for exponent_i in range(50, 351):
        exponent = exponent_i / 100.0
        factors = {}
        for battle_type in ["hero-v-monster", "hero-v-hero"]:
            numerator = 0.0
            denominator = 0.0
            for ratio, expected, sample_type in samples:
                if sample_type != battle_type:
                    continue
                value = ratio ** exponent
                numerator += expected * value
                denominator += value * value
            factors[battle_type] = max(0.05, min(3.0, numerator / denominator if denominator > EPSILON else 1.0))

        for candidate_factors in (factors, {"hero-v-monster": 1.0, "hero-v-hero": 1.0}):
            candidate = evaluate_formula(exponent, candidate_factors)
            if candidate["rmse"] < best["rmse"]:
                best = candidate
    return best


def main() -> int:
    args = parse_args()
    groups, schema_counts, row_count = load_groups(args.dataset)
    groups = [group for group in groups if group.count >= args.min_group_size]
    train, test = split_groups(groups, args.test_fraction)
    full_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, feature_vector) if train else None
    ratio_model = fit_logistic(train, args.epochs, args.learning_rate, args.l2, ratio_feature_vector) if train else None

    metrics: dict[str, Any] = {
        "dataset": args.dataset,
        "input_rows": row_count,
        "schema_counts": dict(schema_counts),
        "groups_after_filter": len(groups),
        "train": summarize_predictions(train, args.safe_ratio, full_model),
        "test": summarize_predictions(test, args.safe_ratio, full_model),
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

    if full_model and ratio_model:
        ratio_train = summarize_predictions(train, args.safe_ratio, ratio_model)
        ratio_test = summarize_predictions(test, args.safe_ratio, ratio_model)
        metrics["ratio_train"] = {
            "model_accuracy": ratio_train["model_accuracy"],
            "model_brier": ratio_train["model_brier"],
        }
        metrics["ratio_test"] = {
            "model_accuracy": ratio_test["model_accuracy"],
            "model_brier": ratio_test["model_brier"],
        }
        metrics["logistic_model"] = serialize_model(full_model, FEATURE_NAMES)
        metrics["ratio_logistic_model"] = serialize_model(ratio_model, RATIO_FEATURE_NAMES)
        metrics["thresholds"] = {
            "full_train": threshold_summary(train, full_model),
            "full_test": threshold_summary(test, full_model),
            "ratio_train": threshold_summary(train, ratio_model),
            "ratio_test": threshold_summary(test, ratio_model),
        }
        metrics["loss_model"] = fit_loss_grid(train)

    if args.json:
        print(json.dumps(metrics, indent=2, sort_keys=True))
    else:
        print(f"rows: {row_count}")
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
            print(
                "  false_safe_groups="
                f"{summary['baseline_false_safe_groups']} "
                "false_unsafe_groups="
                f"{summary['baseline_false_unsafe_groups']}"
            )
        if full_model:
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
            print("loss model:")
            print(
                f"  exponent={loss_model['exponent']:.4f} "
                f"hero-v-monster factor={loss_model['factors']['hero-v-monster']:.4f} "
                f"hero-v-hero factor={loss_model['factors']['hero-v-hero']:.4f} "
                f"mae={loss_model['mae']:.4f} rmse={loss_model['rmse']:.4f}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
