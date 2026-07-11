#!/usr/bin/env python3
"""Evaluate repeated-battle simulation as a predictor fallback.

The script uses existing repeated JSONL outcomes as a proxy for runtime
simulation. For each setup it treats the first K rows as simulated samples and
scores the estimated win probability against the remaining rows.
"""

from __future__ import annotations

import argparse
import math
from collections import Counter
from dataclasses import dataclass
from typing import Any, Callable

from evaluate_nullkiller_predictor import (
    Group,
    battle_type,
    cxx_v3_probability,
    cxx_v3_static_calibration_applies,
    feature_vector,
    fit_logistic,
    iter_json_lines,
    shard_setup_key,
    setup_key,
    split_groups,
)


@dataclass
class ReplayGroup:
    key: str
    row: dict[str, Any]
    rows: list[dict[str, Any]]

    @property
    def count(self) -> int:
        return len(self.rows)

    def to_group(self) -> Group:
        group = Group(key=self.key, row=self.row)
        group.count = len(self.rows)
        group.attacker_wins = sum(1 for row in self.rows if row.get("winner") == "attacker")
        group.no_winner = sum(1 for row in self.rows if row.get("winner") == "none")
        return group


@dataclass
class Metrics:
    name: str
    groups: int = 0
    holdout_rows: int = 0
    fallback_groups: int = 0
    fallback_rows: int = 0
    correct50: float = 0.0
    safety_correct: float = 0.0
    brier: float = 0.0
    false_safe_groups: int = 0
    false_safe_rows: int = 0
    false_unsafe_groups: int = 0
    false_unsafe_rows: int = 0
    by_type: Counter[str] | None = None

    def __post_init__(self) -> None:
        if self.by_type is None:
            self.by_type = Counter()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", help="Dataset directory, .jsonl, .jsonl.gz, or .tar.gz archive")
    parser.add_argument("--sample-counts", default="1,3,5,10", help="Comma-separated simulated sample counts")
    parser.add_argument(
        "--group-key",
        choices=("setup", "shard"),
        default="setup",
        help="Group repeated rows by full setup features or by generated shard metadata. Use shard for generated repeated-simulation datasets.",
    )
    parser.add_argument("--min-group-size", type=int, default=4)
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--epochs", type=int, default=1500)
    parser.add_argument("--learning-rate", type=float, default=0.05)
    parser.add_argument("--l2", type=float, default=0.01)
    parser.add_argument("--safe-probability", type=float, default=0.60)
    parser.add_argument("--actual-safe-probability", type=float, default=0.95)
    parser.add_argument(
        "--scope",
        choices=("all", "deployed-static", "town"),
        default="all",
        help="Battle scope to evaluate. deployed-static matches current Nullkiller2 static v3 usage.",
    )
    parser.add_argument(
        "--static-model",
        choices=("full-fitted", "cxx-v3"),
        default="full-fitted",
        help="Static predictor used when fallback is not used. cxx-v3 matches current Nullkiller2 v3 coefficients.",
    )
    parser.add_argument(
        "--safe-policy",
        choices=("probability", "all-wins", "wilson-lower"),
        default="probability",
        help="How simulated samples are converted into a safe-to-attack decision",
    )
    parser.add_argument(
        "--safe-wilson-z",
        type=float,
        default=1.2815515655446004,
        help="Z value used by --safe-policy wilson-lower; default is one-sided 90%%",
    )
    parser.add_argument(
        "--safe-wilson-threshold",
        type=float,
        default=None,
        help="Wilson lower-bound threshold; defaults to --safe-probability",
    )
    parser.add_argument(
        "--bands",
        default="0:1,0:0.95,0.1:0.95,0.2:0.95,0.25:0.85,0.4:0.75",
        help="Comma-separated low:high static-probability bands that trigger fallback",
    )
    parser.add_argument("--print-worst", type=int, default=8)
    parser.add_argument("--print-false-safe", type=int, default=8)
    return parser.parse_args()


def parse_sample_counts(value: str) -> list[int]:
    result = sorted({int(part) for part in value.split(",") if part.strip()})
    if not result or result[0] <= 0:
        raise ValueError("--sample-counts must contain positive integers")
    return result


def parse_bands(value: str) -> list[tuple[float, float]]:
    result = []
    for part in value.split(","):
        if not part.strip():
            continue
        low_text, high_text = part.split(":", 1)
        low = float(low_text)
        high = float(high_text)
        if low < 0.0 or high > 1.0 or low > high:
            raise ValueError(f"invalid band {part}")
        result.append((low, high))
    if not result:
        raise ValueError("--bands must contain at least one low:high range")
    return result


def load_replay_groups(path: str, group_key: str = "setup") -> tuple[list[ReplayGroup], Counter, int]:
    groups: dict[str, ReplayGroup] = {}
    schema_counts: Counter = Counter()
    rows = 0
    for row in iter_json_lines(path):
        rows += 1
        schema_counts[row.get("schema", 1)] += 1
        key = shard_setup_key(row) if group_key == "shard" else setup_key(row)
        group = groups.get(key)
        if group is None:
            group = ReplayGroup(key=key, row=row, rows=[])
            groups[key] = group
        group.rows.append(row)
    return list(groups.values()), schema_counts, rows


def matches_scope(row: dict[str, Any], scope: str) -> bool:
    if scope == "all":
        return True
    if scope == "deployed-static":
        return cxx_v3_static_calibration_applies(row)
    if scope == "town":
        return battle_type(row).startswith("town")
    raise ValueError(f"Unknown scope: {scope}")


def split_replay_groups(groups: list[ReplayGroup], test_fraction: float) -> tuple[list[ReplayGroup], list[ReplayGroup]]:
    keyed = {group.key: group for group in groups}
    train_groups, test_groups = split_groups([group.to_group() for group in groups], test_fraction)
    return [keyed[group.key] for group in train_groups], [keyed[group.key] for group in test_groups]


def win_rate(rows: list[dict[str, Any]]) -> float:
    return sum(1 for row in rows if row.get("winner") == "attacker") / len(rows) if rows else 0.0


def wilson_lower_bound(successes: int, total: int, z: float) -> float:
    if total <= 0:
        return 0.0
    probability = successes / total
    z2 = z * z
    denominator = 1.0 + z2 / total
    center = probability + z2 / (2.0 * total)
    margin = z * math.sqrt((probability * (1.0 - probability) + z2 / (4.0 * total)) / total)
    return max(0.0, (center - margin) / denominator)


def is_predicted_safe(
    probability: float,
    sample_wins: int,
    sample_count: int,
    used_fallback: bool,
    safe_probability: float,
    safe_policy: str,
    safe_wilson_z: float,
    safe_wilson_threshold: float,
) -> bool:
    if not used_fallback or safe_policy == "probability":
        return probability >= safe_probability
    if safe_policy == "all-wins":
        return sample_wins == sample_count
    if safe_policy == "wilson-lower":
        return wilson_lower_bound(sample_wins, sample_count, safe_wilson_z) >= safe_wilson_threshold
    raise ValueError(f"Unsupported safe policy: {safe_policy}")


def add_prediction(
    metrics: Metrics,
    group: ReplayGroup,
    probability: float,
    actual_probability: float,
    holdout_count: int,
    used_fallback: bool,
    predicted_safe: bool,
    actual_safe_probability: float,
) -> None:
    actual_class = actual_probability >= 0.5
    predicted_class = probability >= 0.5
    actual_safe = actual_probability >= actual_safe_probability
    false_safe = predicted_safe and not actual_safe
    false_unsafe = (not predicted_safe) and actual_safe

    metrics.groups += 1
    metrics.holdout_rows += holdout_count
    metrics.fallback_groups += int(used_fallback)
    metrics.fallback_rows += holdout_count if used_fallback else 0
    metrics.correct50 += holdout_count * (predicted_class == actual_class)
    metrics.safety_correct += holdout_count * (predicted_safe == actual_safe)
    metrics.brier += holdout_count * (probability - actual_probability) ** 2
    metrics.false_safe_groups += int(false_safe)
    metrics.false_safe_rows += holdout_count * int(false_safe)
    metrics.false_unsafe_groups += int(false_unsafe)
    metrics.false_unsafe_rows += holdout_count * int(false_unsafe)
    assert metrics.by_type is not None
    metrics.by_type[f"{battle_type(group.row)}:{'fallback' if used_fallback else 'static'}"] += holdout_count


def print_metrics(metrics: Metrics, sample_count: int) -> None:
    rows = metrics.holdout_rows or 1
    print(
        f"{metrics.name} sample_count={sample_count} groups={metrics.groups} "
        f"holdout_rows={metrics.holdout_rows} fallback_groups={metrics.fallback_groups} "
        f"avg_sims_per_decision={(sample_count * metrics.fallback_groups / metrics.groups) if metrics.groups else 0.0:.2f} "
        f"accuracy50={metrics.correct50 / rows:.4f} "
        f"safety_accuracy={metrics.safety_correct / rows:.4f} "
        f"brier={metrics.brier / rows:.5f} "
        f"false_safe={metrics.false_safe_groups}/{metrics.false_safe_rows} "
        f"false_unsafe={metrics.false_unsafe_groups}/{metrics.false_unsafe_rows} "
        f"by_type={dict(sorted((metrics.by_type or Counter()).items()))}"
    )


def static_probabilities(groups: list[ReplayGroup], predictor: Callable[[dict[str, Any]], float]) -> dict[str, float]:
    return {group.key: predictor(group.row) for group in groups}


def evaluate_policy(
    groups: list[ReplayGroup],
    sample_count: int,
    static_by_key: dict[str, float],
    safe_probability: float,
    actual_safe_probability: float,
    safe_policy: str,
    safe_wilson_z: float,
    safe_wilson_threshold: float,
    band: tuple[float, float] | None,
) -> tuple[Metrics, list[dict[str, Any]], list[dict[str, Any]]]:
    name = "fallback-only" if band is None else f"hybrid[{band[0]:.2f},{band[1]:.2f}]"
    name += f":safe={safe_policy}"
    metrics = Metrics(name=name)
    worst = []
    false_safe_cases = []

    for group in groups:
        if group.count <= sample_count:
            continue
        sample_rows = group.rows[:sample_count]
        holdout_rows = group.rows[sample_count:]
        sample_wins = sum(1 for row in sample_rows if row.get("winner") == "attacker")
        sample_losses = sample_count - sample_wins
        actual_wins = sum(1 for row in holdout_rows if row.get("winner") == "attacker")
        actual_losses = len(holdout_rows) - actual_wins
        sample_probability = win_rate(sample_rows)
        actual_probability = win_rate(holdout_rows)
        static_probability = static_by_key[group.key]
        use_fallback = band is None or (band[0] <= static_probability <= band[1])
        probability = sample_probability if use_fallback else static_probability
        predicted_safe = is_predicted_safe(
            probability,
            sample_wins,
            sample_count,
            use_fallback,
            safe_probability,
            safe_policy,
            safe_wilson_z,
            safe_wilson_threshold,
        )
        actual_safe = actual_probability >= actual_safe_probability
        add_prediction(
            metrics,
            group,
            probability,
            actual_probability,
            len(holdout_rows),
            use_fallback,
            predicted_safe,
            actual_safe_probability,
        )
        item = {
            "error": abs(probability - actual_probability),
            "type": battle_type(group.row),
            "count": group.count,
            "sample": sample_probability,
            "sample_wins": sample_wins,
            "sample_losses": sample_losses,
            "sample_wilson_lower": wilson_lower_bound(sample_wins, sample_count, safe_wilson_z),
            "static": static_probability,
            "predicted": probability,
            "actual": actual_probability,
            "actual_wins": actual_wins,
            "actual_losses": actual_losses,
            "predicted_safe": predicted_safe,
            "actual_safe": actual_safe,
            "fallback": use_fallback,
            "attacker": summarize_army(group.row, "attacker"),
            "defender": summarize_army(group.row, "defender"),
        }
        worst.append(item)
        if predicted_safe and not actual_safe:
            false_safe_cases.append(item)

    worst.sort(key=lambda item: item["error"], reverse=True)
    false_safe_cases.sort(key=lambda item: (item["actual"], -item["sample"], -item["count"]))
    return metrics, worst, false_safe_cases


def summarize_army(row: dict[str, Any], side: str) -> str:
    return "[" + ", ".join(f"{stack.get('creature')}x{stack.get('count')}" for stack in (row.get(f"{side}Army") or [])[:7]) + "]"


def print_worst(worst: list[dict[str, Any]], limit: int, title: str) -> None:
    if limit <= 0:
        return
    print(title + ":")
    for index, item in enumerate(worst[:limit], start=1):
        print(
            f"  {index}. type={item['type']} count={item['count']} "
            f"actual={item['actual']:.4f} predicted={item['predicted']:.4f} "
            f"sample={item['sample']:.4f} static={item['static']:.4f} "
            f"sample_wins={item['sample_wins']} sample_losses={item['sample_losses']} "
            f"actual_wins={item['actual_wins']} actual_losses={item['actual_losses']} "
            f"wilson_lower={item['sample_wilson_lower']:.4f} "
            f"predicted_safe={item['predicted_safe']} actual_safe={item['actual_safe']} "
            f"fallback={item['fallback']} error={item['error']:.4f}"
        )
        print(f"     attacker={item['attacker']}")
        print(f"     defender={item['defender']}")


def main() -> int:
    args = parse_args()
    sample_counts = parse_sample_counts(args.sample_counts)
    bands = parse_bands(args.bands)
    safe_wilson_threshold = args.safe_wilson_threshold
    if safe_wilson_threshold is None:
        safe_wilson_threshold = args.safe_probability
    replay_groups, schema_counts, rows = load_replay_groups(args.dataset, args.group_key)
    replay_groups = [
        group
        for group in replay_groups
        if group.count >= args.min_group_size and matches_scope(group.row, args.scope)
    ]
    train_replays, test_replays = split_replay_groups(replay_groups, args.test_fraction)
    train_groups = [group.to_group() for group in train_replays]

    print(
        f"dataset={args.dataset} rows={rows} schemas={dict(schema_counts)} "
        f"scope={args.scope} group_key={args.group_key} static_model={args.static_model} "
        f"groups={len(replay_groups)} train_groups={len(train_replays)} test_groups={len(test_replays)}"
    )

    full_model = None
    if args.static_model == "full-fitted" and train_groups:
        full_model = fit_logistic(train_groups, args.epochs, args.learning_rate, args.l2, feature_vector)
    static_predictor = full_model.predict if full_model else cxx_v3_probability
    static_by_key = static_probabilities(test_replays, static_predictor)

    static_metrics = Metrics(name=f"static-{args.static_model}")
    for group in test_replays:
        actual_probability = win_rate(group.rows)
        add_prediction(
            static_metrics,
            group,
            static_by_key[group.key],
            actual_probability,
            group.count,
            False,
            static_by_key[group.key] >= args.safe_probability,
            args.actual_safe_probability,
        )
    print_metrics(static_metrics, 0)

    final_worst: list[dict[str, Any]] = []
    final_false_safe: list[dict[str, Any]] = []
    final_title = ""
    final_false_safe_title = ""
    for sample_count in sample_counts:
        fallback_metrics, fallback_worst, fallback_false_safe = evaluate_policy(
            test_replays,
            sample_count,
            static_by_key,
            args.safe_probability,
            args.actual_safe_probability,
            args.safe_policy,
            args.safe_wilson_z,
            safe_wilson_threshold,
            None,
        )
        print_metrics(fallback_metrics, sample_count)
        if not final_worst:
            final_worst = fallback_worst
            final_title = f"worst fallback-only sample_count={sample_count}"
            final_false_safe = fallback_false_safe
            final_false_safe_title = f"false-safe fallback-only sample_count={sample_count}"

        for band in bands:
            hybrid_metrics, hybrid_worst, hybrid_false_safe = evaluate_policy(
                test_replays,
                sample_count,
                static_by_key,
                args.safe_probability,
                args.actual_safe_probability,
                args.safe_policy,
                args.safe_wilson_z,
                safe_wilson_threshold,
                band,
            )
            print_metrics(hybrid_metrics, sample_count)
            if band == bands[0] and sample_count == sample_counts[-1]:
                final_worst = hybrid_worst
                final_title = f"worst hybrid band={band} sample_count={sample_count}"
                final_false_safe = hybrid_false_safe
                final_false_safe_title = f"false-safe hybrid band={band} sample_count={sample_count}"

    print_worst(final_worst, args.print_worst, final_title)
    print_worst(final_false_safe, args.print_false_safe, final_false_safe_title)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
