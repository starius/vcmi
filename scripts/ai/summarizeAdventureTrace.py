#!/usr/bin/env python3
"""Summarize ScriptedAdventureAI trace JSON files."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any


DAY_PATTERN = re.compile(r"-day-(\d+)-")
EVENT_PATTERN = re.compile(r"-event-(\d+)-")
RESOURCE_VALUES = {
    "wood": 100,
    "ore": 100,
    "mercury": 200,
    "sulfur": 200,
    "crystal": 200,
    "gems": 200,
    "gold": 1,
}
IMPORTANT_MISTAKE_TYPES = {
    "fallback_output",
    "idle_with_candidates",
    "unsafe_object_action",
    "unsafe_move_action",
    "ignored_better_object",
    "hero_threat_without_escape",
    "defense_pressure_without_response",
    "failed_action",
    "repeated_failed_target",
}


def as_list(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    return []


def as_dict(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    return {}


def nested(value: Any, *keys: str) -> Any:
    for key in keys:
        value = as_dict(value).get(key)
    return value


def as_int(value: Any, default: int = 0) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        try:
            return int(float(value))
        except ValueError:
            return default
    return default


def as_float(value: Any, default: float = 0.0) -> float:
    if isinstance(value, bool):
        return float(value)
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value)
        except ValueError:
            return default
    return default


def iter_trace_files(paths: list[str]) -> list[Path]:
    files: list[Path] = []
    for raw_path in paths:
        path = Path(raw_path).expanduser()
        if path.is_dir():
            files.extend(sorted(candidate for candidate in path.rglob("*.json") if EVENT_PATTERN.search(candidate.name)))
        elif path.is_file():
            files.append(path)
        else:
            print(f"warning: trace path does not exist: {path}", file=sys.stderr)
    return files


def counter_to_dict(counter: Counter[str]) -> dict[str, int]:
    return dict(counter.most_common())


def count_actions(counter: Counter[str], actions: Any) -> None:
	for action in as_list(actions):
		action_type = as_dict(action).get("type", "<missing>")
		counter[str(action_type)] += 1


def count_action(counter: Counter[str], action: Any) -> None:
	action_type = as_dict(action).get("type", "<missing>")
	counter[str(action_type)] += 1


def day_from_path(path: Path) -> str | None:
    match = DAY_PATTERN.search(path.name)
    if match:
        return match.group(1)
    return None


def int_from_path(pattern: re.Pattern[str], path: Path) -> int:
    match = pattern.search(path.name)
    return int(match.group(1)) if match else -1


def day_number(path: Path) -> int:
    return int_from_path(DAY_PATTERN, path)


def event_number(path: Path) -> int:
    return int_from_path(EVENT_PATTERN, path)


def resource_score(resources: Any) -> int:
    data = as_dict(resources)
    return sum(as_int(data.get(name)) * value for name, value in RESOURCE_VALUES.items())


MAP_PROGRESS_FIELDS = (
    "exploredTiles",
    "visibleObjects",
    "exploredLandTiles",
    "exploredWaterTiles",
    "exploredRockTiles",
    "exploredPassableTiles",
    "exploredBlockedTiles",
    "exploredVisitableTiles",
    "exploredRoadTiles",
    "selfVisibleObjects",
    "allyVisibleObjects",
    "enemyVisibleObjects",
    "neutralVisibleObjects",
    "unflaggableVisibleObjects",
    "townObjects",
    "heroObjects",
    "mineObjects",
    "resourceObjects",
    "artifactObjects",
)


def count_map_entries(entries: Any, id_field: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for entry in as_list(entries):
        data = as_dict(entry)
        if id_field not in data:
            continue
        key = str(as_int(data.get(id_field)))
        result[key] = result.get(key, 0) + as_int(data.get("count"))
    return result


def map_count_delta(first: dict[str, int], final: dict[str, int]) -> dict[str, int]:
    keys = sorted(set(first) | set(final), key=lambda item: (as_int(item), item))
    return {key: final.get(key, 0) - first.get(key, 0) for key in keys}


def map_snapshot_from_input(record: dict[str, Any]) -> dict[str, Any] | None:
    script_input = as_dict(record.get("input"))
    state = as_dict(script_input.get("state"))
    map_state = as_dict(state.get("map"))
    if not map_state:
        return None

    visible_control = as_dict(map_state.get("visibleControl"))
    counts_by_control = count_map_entries(visible_control.get("objectCountsByControl"), "controlId")
    counts_by_kind = count_map_entries(visible_control.get("objectCountsByKind"), "kindId")
    counts_by_owner = count_map_entries(visible_control.get("objectCountsByOwner"), "ownerId")
    explored_tiles = as_int(map_state.get("exploredTilesCount"), as_int(map_state.get("visibleTilesCount")))

    return {
        "player": record.get("player"),
        "day": record.get("day"),
        "trace": record.get("path"),
        "totalTiles": as_int(map_state.get("totalTiles")),
        "exploredTiles": explored_tiles,
        "exploredRatio": as_float(map_state.get("exploredRatio")),
        "visibleObjects": as_int(map_state.get("visibleObjectsCount")),
        "exploredLandTiles": as_int(map_state.get("exploredLandTilesCount")),
        "exploredWaterTiles": as_int(map_state.get("exploredWaterTilesCount")),
        "exploredRockTiles": as_int(map_state.get("exploredRockTilesCount")),
        "exploredPassableTiles": as_int(map_state.get("exploredPassableTilesCount")),
        "exploredBlockedTiles": as_int(map_state.get("exploredBlockedTilesCount")),
        "exploredVisitableTiles": as_int(map_state.get("exploredVisitableTilesCount")),
        "exploredRoadTiles": as_int(map_state.get("exploredRoadTilesCount")),
        "selfVisibleObjects": counts_by_control.get("0", 0),
        "allyVisibleObjects": counts_by_control.get("1", 0),
        "enemyVisibleObjects": counts_by_control.get("2", 0),
        "neutralVisibleObjects": counts_by_control.get("3", 0),
        "unflaggableVisibleObjects": counts_by_control.get("4", 0),
        "townObjects": counts_by_kind.get("5", 0),
        "heroObjects": counts_by_kind.get("6", 0),
        "mineObjects": counts_by_kind.get("3", 0),
        "resourceObjects": counts_by_kind.get("2", 0),
        "artifactObjects": counts_by_kind.get("4", 0),
        "objectCountsByControlId": counts_by_control,
        "objectCountsByKindId": counts_by_kind,
        "objectCountsByOwnerId": counts_by_owner,
        "exploredByLevel": as_list(map_state.get("exploredByLevel")),
    }


def map_snapshot_delta(first: dict[str, Any], final: dict[str, Any]) -> dict[str, Any]:
    delta: dict[str, Any] = {
        "days": as_int(final.get("day")) - as_int(first.get("day")),
        "exploredRatio": as_float(final.get("exploredRatio")) - as_float(first.get("exploredRatio")),
        "objectCountsByControlId": map_count_delta(
            as_dict(first.get("objectCountsByControlId")),
            as_dict(final.get("objectCountsByControlId")),
        ),
        "objectCountsByKindId": map_count_delta(
            as_dict(first.get("objectCountsByKindId")),
            as_dict(final.get("objectCountsByKindId")),
        ),
        "objectCountsByOwnerId": map_count_delta(
            as_dict(first.get("objectCountsByOwnerId")),
            as_dict(final.get("objectCountsByOwnerId")),
        ),
    }
    for field in MAP_PROGRESS_FIELDS:
        delta[field] = as_int(final.get(field)) - as_int(first.get(field))
    return delta


def map_progress_score(delta: dict[str, Any]) -> int:
    return (
        as_int(delta.get("exploredTiles")) * 2
        + as_int(delta.get("exploredPassableTiles"))
        + as_int(delta.get("exploredRoadTiles")) * 2
        + as_int(delta.get("visibleObjects")) * 5
        + as_int(delta.get("selfVisibleObjects")) * 15
        + as_int(delta.get("allyVisibleObjects")) * 10
        + as_int(delta.get("enemyVisibleObjects")) * 10
        + as_int(delta.get("neutralVisibleObjects")) * 4
        + as_int(delta.get("mineObjects")) * 10
        + as_int(delta.get("townObjects")) * 20
    )


def aggregate_map_progress(
    first_inputs: dict[tuple[str, str], dict[str, Any]],
    latest_inputs: dict[tuple[str, str], dict[str, Any]],
) -> dict[str, Any]:
    per_player: list[dict[str, Any]] = []
    totals: Counter[str] = Counter()
    score = 0

    for series_key in sorted(latest_inputs):
        final = map_snapshot_from_input(latest_inputs[series_key])
        if not final:
            continue
        first = map_snapshot_from_input(first_inputs.get(series_key, latest_inputs[series_key])) or final
        delta = map_snapshot_delta(first, final)
        item_score = map_progress_score(delta)
        score += item_score
        for field in MAP_PROGRESS_FIELDS:
            totals[f"{field}Delta"] += as_int(delta.get(field))
            totals[f"final{field[0].upper()}{field[1:]}"] += as_int(final.get(field))
        per_player.append({
            "series": series_key[0],
            "player": series_key[1],
            "score": item_score,
            "first": first,
            "final": final,
            "delta": delta,
        })

    return {
        "score": score,
        "players": len(per_player),
        "totals": dict(totals),
        "perPlayer": per_player,
    }


def alert_counts(alerts: Any) -> Counter[str]:
    counts: Counter[str] = Counter()
    for alert in as_list(alerts):
        level = str(as_dict(alert).get("level", "watch"))
        counts[level] += 1
    return counts


def plan_action_key(action: Any) -> tuple[Any, ...]:
    data = as_dict(action)
    return (
        str(data.get("type", "")),
        as_int(data.get("hero_id")) if "hero_id" in data else None,
        as_int(data.get("object_id")) if "object_id" in data else None,
        as_int(data.get("town_id")) if "town_id" in data else None,
        as_int(data.get("building_id")) if "building_id" in data else None,
        as_int(data.get("x")) if "x" in data else None,
        as_int(data.get("y")) if "y" in data else None,
        as_int(data.get("z")) if "z" in data else None,
        data.get("route_id"),
    )


def action_type_counts(actions: Any) -> Counter[str]:
    counter: Counter[str] = Counter()
    for action in as_list(actions):
        counter[str(as_dict(action).get("type", "<missing>"))] += 1
    return counter


def progress_executed_actions(progress: Any) -> list[Any]:
    return as_list(as_dict(progress).get("executed"))


def native_slice_did_work(action: Any) -> bool:
    data = as_dict(action)
    return (
        data.get("didWork") is True
        or as_int(data.get("priorityTasksExecuted")) > 0
        or as_int(data.get("adventureStepsExecuted")) > 0
        or as_int(data.get("adventureReplanSteps")) > 0
        or as_int(data.get("tradePasses")) > 0
        or data.get("paused") is True
    )


def native_slice_should_end_turn(action: Any) -> bool:
    data = as_dict(action)
    return (
        data.get("type") == "nullkiller_turn_slice"
        and (
            data.get("shouldStopTurn") is True
            or as_int(data.get("adventureStopTurnSteps")) > 0
            or (data.get("exhaustedCandidates") is True and not native_slice_did_work(data))
        )
    )


def progress_has_native_stop(progress: Any) -> bool:
    native_slices = [
        as_dict(action)
        for action in progress_executed_actions(progress)
        if as_dict(action).get("type") == "nullkiller_turn_slice"
    ]
    return bool(native_slices and native_slice_should_end_turn(native_slices[-1]))


def output_accepts_native_max_pass(output: Any) -> bool:
    data = as_dict(output)
    intent = str(data.get("intent") or "").lower()
    return "native max-pass limit" in intent


def append_task(tasks: list[dict[str, Any]], task: Any) -> None:
    task_data = as_dict(task)
    if task_data:
        tasks.append(task_data)


def append_native_step_tasks(tasks: list[dict[str, Any]], step: Any) -> None:
    step_data = as_dict(step)
    if step_data.get("didExecute") is True:
        append_task(tasks, step_data.get("selectedTask"))
    for attempt in as_list(step_data.get("attemptedTasks")):
        attempt_data = as_dict(attempt)
        if attempt_data.get("executed") is True:
            append_task(tasks, attempt_data.get("task"))


def native_tasks_from_action(action: Any) -> list[dict[str, Any]]:
    data = as_dict(action)
    tasks: list[dict[str, Any]] = []
    action_type = data.get("type")

    if action_type == "nullkiller_priority_pass" and as_int(data.get("executed")) > 0:
        append_task(tasks, data.get("lastTask"))
    elif action_type == "nullkiller_step":
        append_native_step_tasks(tasks, data)
    elif action_type == "nullkiller_pass":
        for step in as_list(data.get("steps")):
            append_native_step_tasks(tasks, step)
    elif action_type == "nullkiller_turn_slice":
        for turn_pass in as_list(data.get("passes")):
            pass_data = as_dict(turn_pass)
            priority = as_dict(pass_data.get("priority"))
            if as_int(priority.get("executed")) > 0:
                append_task(tasks, priority.get("lastTask"))
            append_native_step_tasks(tasks, pass_data.get("adventure"))

    return tasks


def progress_native_tasks(progress: Any) -> list[dict[str, Any]]:
    tasks: list[dict[str, Any]] = []
    for action in progress_executed_actions(progress):
        tasks.extend(native_tasks_from_action(action))
    return tasks


def task_touches_object(task: Any, object_id: Any) -> bool:
    task_data = as_dict(task)
    object_id = as_int(object_id, -1)
    if object_id < 0:
        return False

    for field in ("object_id", "town_id", "hero_id"):
        if field in task_data and as_int(task_data.get(field), -2) == object_id:
            return True

    for touched_id in as_list(task_data.get("affectedObjectIds")):
        if as_int(touched_id, -2) == object_id:
            return True

    nested_goal = as_dict(task_data.get("goal"))
    if nested_goal:
        return task_touches_object(nested_goal, object_id)
    return False


def progress_touches_object(progress: Any, object_id: Any) -> bool:
    for task in progress_native_tasks(progress):
        if task_touches_object(task, object_id):
            return True
    return False


def progress_touches_any_object(progress: Any, object_ids: Any) -> bool:
    ids = {as_int(object_id, -1) for object_id in as_list(object_ids)}
    ids.discard(-1)
    return any(progress_touches_object(progress, object_id) for object_id in ids)


def action_matches(action: Any, action_type: str, field: str | None = None, value: Any = None) -> bool:
    data = as_dict(action)
    if data.get("type") != action_type:
        return False
    if field is None:
        return True
    return data.get(field) == value


def candidate_action(candidate: Any) -> dict[str, Any]:
    return as_dict(as_dict(candidate).get("planAction"))


def candidate_by_action(candidates: Any) -> dict[tuple[Any, ...], dict[str, Any]]:
    result: dict[tuple[Any, ...], dict[str, Any]] = {}
    for candidate in as_list(candidates):
        candidate_dict = as_dict(candidate)
        action = candidate_action(candidate_dict)
        key = plan_action_key(action)
        if key[0]:
            result[key] = candidate_dict
    return result


def target_was_visited(target: Any, memory: Any) -> bool:
    object_id = as_dict(as_dict(target).get("object")).get("id")
    if object_id is None:
        return False
    return str(object_id) in as_dict(as_dict(memory).get("visitedTargets"))


def best_safe_object(objects: Any, memory: Any = None) -> dict[str, Any] | None:
    best: dict[str, Any] | None = None
    best_value = float("-inf")
    for target in as_list(objects):
        target_dict = as_dict(target)
        if target_dict.get("safe") is False:
            continue
        if memory is not None and target_was_visited(target_dict, memory):
            continue
        value = as_float(target_dict.get("value"))
        if value > best_value:
            best = target_dict
            best_value = value
    return best


def has_relevant_candidates(action_space: Any) -> bool:
    data = as_dict(action_space)
    return any(
        as_list(data.get(field))
        for field in ("buildOptions", "recruitOptions", "reachableObjects", "movementOptions", "recommendedActions")
    )


def is_reinforce_transfer_candidate(candidate: Any) -> bool:
    data = as_dict(candidate)
    kind = as_int(data.get("transferKindId", data.get("transfer_kind_id")))
    return kind == 2 or str(data.get("transferKind")) == "reinforce_town"


def has_defensive_response_candidate(action_space: Any) -> bool:
    data = as_dict(action_space)
    if as_list(data.get("buildOptions")) or as_list(data.get("recruitOptions")):
        return True
    return any(is_reinforce_transfer_candidate(candidate) for candidate in as_list(data.get("armyTransferOptions")))


def output_has_defensive_response(actions: Any, action_space: Any) -> bool:
    action_counts = action_type_counts(actions)
    if (
        action_counts["build"]
        or action_counts["recruit"]
        or action_counts["nullkiller_build_army"]
        or action_counts["nullkiller_recruit_creatures"]
        or action_counts["nullkiller_move_creatures_to_hero"]
    ):
        return True

    transfer_candidates = candidate_by_action(as_dict(action_space).get("armyTransferOptions"))
    for action in as_list(actions):
        action_data = as_dict(action)
        if action_data.get("type") != "transfer_army":
            continue
        if is_reinforce_transfer_candidate(transfer_candidates.get(plan_action_key(action_data))):
            return True
    return False


def distance_squared(left: Any, right: Any) -> float | None:
    left_data = as_dict(left)
    right_data = as_dict(right)
    if not left_data or not right_data:
        return None
    if left_data.get("z") is not None and right_data.get("z") is not None and as_int(left_data.get("z")) != as_int(right_data.get("z")):
        return None
    dx = as_float(left_data.get("x")) - as_float(right_data.get("x"))
    dy = as_float(left_data.get("y")) - as_float(right_data.get("y"))
    return dx * dx + dy * dy


def hero_has_escape_candidate(action_space: Any, alert: Any) -> bool:
    alert_data = as_dict(alert)
    hero_id = alert_data.get("hero_id")
    current_distance = as_float(alert_data.get("distanceSquared"))
    enemy_position = alert_data.get("enemyPosition")
    for option in as_list(as_dict(action_space).get("movementOptions")):
        data = as_dict(option)
        action = candidate_action(data)
        if data.get("hero_id") != hero_id or data.get("safe") is False or action.get("type") != "move_hero":
            continue
        next_distance = distance_squared(nested(data, "path", "destination"), enemy_position)
        if next_distance is not None and next_distance > current_distance:
            return True
    return False


def compact_action(action: Any) -> dict[str, Any]:
    data = as_dict(action)
    result: dict[str, Any] = {"type": data.get("type", "<missing>")}
    for field in (
        "hero_id",
        "object_id",
        "town_id",
        "building_id",
        "creature_id",
        "source_id",
        "destination_id",
        "x",
        "y",
        "z",
        "route_id",
        "error",
    ):
        if field in data:
            result[field] = data[field]
    return result


def fixture_for_mistake(kind: str, script: str, script_input: dict[str, Any], details: dict[str, Any]) -> dict[str, Any] | None:
    name = details.get("name", kind)
    fixture: dict[str, Any] = {
        "name": name,
        "script": script,
        "input": script_input,
        "expect": {},
        "notes": details.get("description", kind),
    }

    if kind == "idle_with_candidates":
        fixture["expect"]["actionsDoNotContain"] = [{"type": "end_turn"}]
    elif kind in ("unsafe_object_action", "unsafe_move_action"):
        if action := details.get("action"):
            fixture["expect"]["actionsDoNotContain"] = [compact_action(action)]
    elif kind == "ignored_better_object":
        if better := details.get("betterAction"):
            fixture["expect"]["actionsContain"] = [compact_action(better)]
    elif kind == "hero_threat_without_escape":
        if "hero_id" in details:
            fixture["expect"]["actionsContain"] = [{"type": "move_hero", "hero_id": details["hero_id"]}]
    elif kind == "defense_pressure_without_response":
        action_space = as_dict(script_input.get("actionSpace"))
        if as_list(action_space.get("recruitOptions")):
            fixture["expect"]["actionsContain"] = [{"type": "recruit"}]
        elif as_list(action_space.get("buildOptions")):
            fixture["expect"]["actionsContain"] = [{"type": "build"}]
        else:
            fixture["expect"]["actionsContain"] = [{"type": "transfer_army"}]
    elif kind == "failed_action":
        if action := details.get("action"):
            fixture["expect"]["actionsDoNotContain"] = [compact_action(action)]
    else:
        return None

    return fixture


def make_mistake(
    kind: str,
    severity: int,
    record: dict[str, Any],
    description: str,
    details: dict[str, Any] | None = None,
    fixture: dict[str, Any] | None = None,
) -> dict[str, Any]:
    mistake_details = details or {}
    item = {
        "type": kind,
        "severity": severity,
        "description": description,
        "player": record.get("player"),
        "day": record.get("day"),
        "callIndex": record.get("callIndex"),
        "trace": record.get("path"),
        "details": mistake_details,
    }
    if fixture:
        item["fixture"] = fixture
    return item


def quality_from_input(record: dict[str, Any]) -> dict[str, Any]:
    script_input = as_dict(record.get("input"))
    state = as_dict(script_input.get("state"))
    analysis = as_dict(script_input.get("analysis"))
    action_space = as_dict(script_input.get("actionSpace"))

    heroes = as_list(state.get("heroes"))
    towns = as_list(state.get("towns"))
    resources = as_dict(state.get("resources"))
    defense_alerts = alert_counts(analysis.get("defenseAlerts"))
    hero_alerts = alert_counts(analysis.get("heroThreatAlerts"))
    build_options = as_list(action_space.get("buildOptions"))
    recruit_options = as_list(action_space.get("recruitOptions"))
    reachable_objects = as_list(action_space.get("reachableObjects"))
    movement_options = as_list(action_space.get("movementOptions"))

    hero_strength = sum(as_int(as_dict(hero).get("armyStrength")) for hero in heroes)
    town_strength = sum(as_int(as_dict(town).get("armyStrength")) for town in towns)
    hero_levels = sum(as_int(as_dict(hero).get("level")) for hero in heroes)
    movement = sum(as_int(as_dict(hero).get("movement")) for hero in heroes)
    buildings = sum(len(as_list(as_dict(town).get("buildings"))) for town in towns)
    recruitable_amount = sum(as_int(as_dict(option).get("amount")) for option in recruit_options)
    resource_value = resource_score(resources)
    threat_penalty = (
        defense_alerts["critical"] * 350
        + defense_alerts["high"] * 180
        + defense_alerts["watch"] * 60
        + hero_alerts["critical"] * 220
        + hero_alerts["high"] * 120
        + hero_alerts["watch"] * 40
    )
    score = int(
        as_int(record.get("day")) * 20
        + len(towns) * 750
        + len(heroes) * 250
        + hero_levels * 45
        + hero_strength / 100
        + town_strength / 120
        + resource_value / 100
        + buildings * 25
        + movement / 100
        + recruitable_amount * 5
        + len(build_options) * 5
        + len(reachable_objects) * 3
        + len(movement_options)
        - threat_penalty
    )

    return {
        "player": record.get("player"),
        "day": record.get("day"),
        "trace": record.get("path"),
        "score": score,
        "heroes": len(heroes),
        "towns": len(towns),
        "heroLevels": hero_levels,
        "heroArmyStrength": hero_strength,
        "townArmyStrength": town_strength,
        "armyStrength": hero_strength + town_strength,
        "movement": movement,
        "buildings": buildings,
        "recruitableAmount": recruitable_amount,
        "resources": resources,
        "resourceValue": resource_value,
        "buildOptions": len(build_options),
        "recruitOptions": len(recruit_options),
        "reachableObjects": len(reachable_objects),
        "movementOptions": len(movement_options),
        "visibleEnemyHeroes": len(as_list(analysis.get("visibleEnemyHeroes"))),
        "visibleEnemyTowns": len(as_list(analysis.get("visibleEnemyTowns"))),
        "defenseAlerts": counter_to_dict(defense_alerts),
        "heroThreatAlerts": counter_to_dict(hero_alerts),
        "threatPenalty": threat_penalty,
    }


def aggregate_quality(final_inputs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    per_player = [quality_from_input(record) for record in final_inputs.values()]
    totals: Counter[str] = Counter()
    resources: Counter[str] = Counter()
    score = 0
    max_day = 0
    for item in per_player:
        score += as_int(item.get("score"))
        max_day = max(max_day, as_int(item.get("day")))
        for field in (
            "heroes",
            "towns",
            "heroLevels",
            "heroArmyStrength",
            "townArmyStrength",
            "armyStrength",
            "movement",
            "buildings",
            "recruitableAmount",
            "resourceValue",
            "buildOptions",
            "recruitOptions",
            "reachableObjects",
            "movementOptions",
            "visibleEnemyHeroes",
            "visibleEnemyTowns",
            "threatPenalty",
        ):
            totals[field] += as_int(item.get(field))
        for resource, value in as_dict(item.get("resources")).items():
            resources[resource] += as_int(value)

    result = {
        "score": score,
        "players": len(per_player),
        "maxDay": max_day,
        "totals": dict(totals),
        "resources": dict(resources),
        "perPlayer": sorted(per_player, key=lambda item: str(item.get("player"))),
    }
    return result


def analyze_mistakes(
    inputs: dict[tuple[str, int, int], dict[str, Any]],
    outputs: dict[tuple[str, int, int], dict[str, Any]],
    progresses: dict[tuple[str, int, int], dict[str, Any]],
) -> list[dict[str, Any]]:
    mistakes: list[dict[str, Any]] = []
    failed_object_counts: Counter[int] = Counter()
    failed_object_records: dict[int, dict[str, Any]] = {}

    for key, input_record in inputs.items():
        script_input = as_dict(input_record.get("input"))
        action_space = as_dict(script_input.get("actionSpace"))
        analysis = as_dict(script_input.get("analysis"))
        output_record = outputs.get(key)
        if not output_record:
            continue

        output = as_dict(output_record.get("output"))
        actions = as_list(output.get("actions"))
        progress = as_dict(output_record.get("progress"))
        progress_actions = progress_executed_actions(progress)
        all_actions = actions + progress_actions
        action_counts = action_type_counts(all_actions)
        script = str(output_record.get("script") or input_record.get("script") or "")

        if output.get("status") == "fallback":
            details = {"name": "fallback-output", "description": "Script explicitly requested fallback."}
            mistakes.append(make_mistake("fallback_output", 5, output_record, details["description"], details))

        if (
            output.get("status") == "end_turn"
            and has_relevant_candidates(action_space)
            and not progress_has_native_stop(progress)
            and not output_accepts_native_max_pass(output)
        ):
            details = {
                "name": "idle-with-candidates",
                "description": "Script ended the turn while build, recruit, movement, or object candidates were available.",
                "candidateCounts": {
                    "buildOptions": len(as_list(action_space.get("buildOptions"))),
                    "recruitOptions": len(as_list(action_space.get("recruitOptions"))),
                    "reachableObjects": len(as_list(action_space.get("reachableObjects"))),
                    "movementOptions": len(as_list(action_space.get("movementOptions"))),
                },
            }
            fixture = fixture_for_mistake("idle_with_candidates", script, script_input, details)
            mistakes.append(make_mistake("idle_with_candidates", 4, output_record, details["description"], details, fixture))

        object_candidates = candidate_by_action(action_space.get("reachableObjects"))
        move_candidates = candidate_by_action(action_space.get("movementOptions"))
        for action in all_actions:
            action_data = as_dict(action)
            key_for_action = plan_action_key(action_data)
            if action_data.get("type") == "visit_object":
                candidate = object_candidates.get(key_for_action)
                if candidate and candidate.get("safe") is False:
                    details = {
                        "name": f"unsafe-object-{action_data.get('object_id')}",
                        "description": "Script chose an object target marked unsafe by host analysis.",
                        "action": action_data,
                        "risk": candidate.get("risk"),
                        "dangerRatio": candidate.get("dangerRatio"),
                        "value": candidate.get("value"),
                    }
                    fixture = fixture_for_mistake("unsafe_object_action", script, script_input, details)
                    mistakes.append(make_mistake("unsafe_object_action", 4, output_record, details["description"], details, fixture))

                best = best_safe_object(action_space.get("reachableObjects"), script_input.get("memory"))
                if candidate and best and as_dict(best.get("object")).get("id") != action_data.get("object_id"):
                    chosen_value = as_float(candidate.get("value"))
                    best_value = as_float(best.get("value"))
                    if best_value >= chosen_value + 250:
                        details = {
                            "name": f"ignored-better-object-{as_dict(best.get('object')).get('id')}",
                            "description": "Script chose a lower-value object while a much better safe object was available.",
                            "action": action_data,
                            "chosenValue": chosen_value,
                            "betterValue": best_value,
                            "betterAction": candidate_action(best),
                        }
                        fixture = fixture_for_mistake("ignored_better_object", script, script_input, details)
                        mistakes.append(make_mistake("ignored_better_object", 3, output_record, details["description"], details, fixture))

            if action_data.get("type") == "move_hero":
                candidate = move_candidates.get(key_for_action)
                if candidate and candidate.get("safe") is False:
                    details = {
                        "name": f"unsafe-move-{action_data.get('hero_id')}",
                        "description": "Script chose a movement target marked unsafe by host analysis.",
                        "action": action_data,
                        "risk": candidate.get("risk"),
                        "dangerRatio": candidate.get("dangerRatio"),
                        "value": candidate.get("value"),
                    }
                    fixture = fixture_for_mistake("unsafe_move_action", script, script_input, details)
                    mistakes.append(make_mistake("unsafe_move_action", 4, output_record, details["description"], details, fixture))

        high_threats = [
            as_dict(alert)
            for alert in as_list(analysis.get("heroThreatAlerts"))
            if str(as_dict(alert).get("level")) in {"high", "critical"}
        ]
        actionable_threats = [
            alert
            for alert in high_threats
            if alert.get("hero_id") is not None and hero_has_escape_candidate(action_space, alert)
        ]
        moved_heroes = {
            as_dict(action).get("hero_id")
            for action in all_actions
            if as_dict(action).get("type") == "move_hero"
        }
        if actionable_threats and not any(
            alert.get("hero_id") in moved_heroes or progress_touches_object(progress, alert.get("hero_id"))
            for alert in actionable_threats
        ):
            alert = actionable_threats[0]
            hero_id = alert.get("hero_id")
            details = {
                "name": f"threatened-hero-{hero_id}",
                "description": "A hero had a high/critical threat alert with a safe movement candidate, but the script did not move any threatened hero.",
                "hero_id": hero_id,
                "alert": alert,
            }
            fixture = fixture_for_mistake("hero_threat_without_escape", script, script_input, details)
            mistakes.append(make_mistake("hero_threat_without_escape", 4, output_record, details["description"], details, fixture))

        high_defense = [
            as_dict(alert)
            for alert in as_list(analysis.get("defenseAlerts"))
            if str(as_dict(alert).get("level")) in {"high", "critical"}
        ]
        if (
            high_defense
            and has_defensive_response_candidate(action_space)
            and not output_has_defensive_response(all_actions, action_space)
            and not progress_touches_any_object(progress, [alert.get("town_id") for alert in high_defense])
        ):
            details = {
                "name": "defense-pressure-without-response",
                "description": "A town had a high/critical defense alert but the script did not recruit or build.",
                "alerts": high_defense,
            }
            fixture = fixture_for_mistake("defense_pressure_without_response", script, script_input, details)
            mistakes.append(make_mistake("defense_pressure_without_response", 4, output_record, details["description"], details, fixture))

    for key, progress_record in progresses.items():
        progress = as_dict(progress_record.get("progress"))
        for item in as_list(progress.get("failed")):
            item_data = as_dict(item)
            details = {
                "name": f"failed-action-{item_data.get('type', 'unknown')}",
                "description": "The host rejected or could not finish a scripted action.",
                "action": item_data,
                "error": item_data.get("error", "<missing>"),
            }
            source_input = as_dict(inputs.get(key, {}).get("input"))
            script = str(progress_record.get("script") or inputs.get(key, {}).get("script") or "")
            fixture = fixture_for_mistake("failed_action", script, source_input, details) if source_input else None
            mistakes.append(make_mistake("failed_action", 3, progress_record, details["description"], details, fixture))
            object_id = item_data.get("object_id")
            if isinstance(object_id, int):
                failed_object_counts[object_id] += 1
                failed_object_records[object_id] = progress_record

        if progress_record.get("stopped"):
            details = {
                "name": "stopped-action-batch",
                "description": "The host stopped a scripted action batch before completing all actions.",
                "remaining": len(as_list(progress.get("remaining"))),
            }
            mistakes.append(make_mistake("stopped_batch", 1, progress_record, details["description"], details))

    for object_id, count in failed_object_counts.items():
        if count <= 1:
            continue
        details = {
            "name": f"repeated-failed-target-{object_id}",
            "description": "The same object target failed more than once in this trace set.",
            "object_id": object_id,
            "failures": count,
        }
        mistakes.append(make_mistake("repeated_failed_target", 4, failed_object_records[object_id], details["description"], details))

    mistakes.sort(key=lambda item: (-as_int(item.get("severity")), str(item.get("type")), str(item.get("trace"))))
    return mistakes


def summarize(files: list[Path], max_mistakes: int = 100) -> dict[str, Any]:
    labels: Counter[str] = Counter()
    players: Counter[str] = Counter()
    scripts: Counter[str] = Counter()
    days: Counter[str] = Counter()
    output_statuses: Counter[str] = Counter()
    output_intents: Counter[str] = Counter()
    requested_actions: Counter[str] = Counter()
    executed_actions: Counter[str] = Counter()
    failed_actions: Counter[str] = Counter()
    failure_errors: Counter[str] = Counter()
    update_types: Counter[str] = Counter()
    opponent_update_types: Counter[str] = Counter()
    candidate_risks: Counter[str] = Counter()
    parse_errors: list[str] = []
    progress_counts: Counter[str] = Counter()
    analysis_counts: Counter[str] = Counter()
    inputs_by_key: dict[tuple[str, int, int], dict[str, Any]] = {}
    outputs_by_key: dict[tuple[str, int, int], dict[str, Any]] = {}
    progresses_by_key: dict[tuple[str, int, int], dict[str, Any]] = {}
    first_inputs: dict[tuple[str, str], dict[str, Any]] = {}
    latest_inputs: dict[tuple[str, str], dict[str, Any]] = {}

    for path in files:
        try:
            with path.open("r", encoding="utf-8") as handle:
                trace = json.load(handle)
        except (OSError, json.JSONDecodeError) as error:
            parse_errors.append(f"{path}: {error}")
            continue

        label = str(trace.get("label", "<missing>"))
        player = str(trace.get("player", "<missing>"))
        script = str(trace.get("script", "<missing>"))
        day = day_number(path)
        event = event_number(path)
        labels[label] += 1
        players[player] += 1
        scripts[script] += 1
        if day_label := day_from_path(path):
            days[day_label] += 1

        payload = as_dict(trace.get("payload"))
        call_index = as_int(payload.get("callIndex"))
        key = (player, day, call_index)
        record = {
            "path": str(path),
            "player": player,
            "script": script,
            "day": day,
            "event": event,
            "callIndex": call_index,
        }
        if label in {"output", "imperative-output"}:
            output = as_dict(payload.get("output"))
            output_record = dict(record)
            output_record["output"] = output
            progress = as_dict(payload.get("progress"))
            if progress:
                output_record["progress"] = progress
            outputs_by_key[key] = output_record
            output_statuses[str(output.get("status", "<missing>"))] += 1
            if output.get("intent"):
                output_intents[str(output["intent"])] += 1
            count_actions(requested_actions, output.get("actions"))
        elif label == "imperative-command":
            command = as_dict(payload.get("command"))
            response = as_dict(payload.get("response"))
            action = as_dict(command.get("payload"))
            result = as_dict(response.get("result"))
            if command.get("kind") == "execute":
                count_action(requested_actions, action)
                if response.get("ok") is False:
                    count_action(failed_actions, action)
                    failure_errors[str(response.get("error", "<missing>"))] += 1
                    progress_counts["failed"] += 1
                else:
                    count_action(executed_actions, action)
                    progress_counts["executed"] += 1
                if result.get("stop"):
                    progress_counts["stopped_actions"] += 1
            else:
                progress_counts[f"imperative_{command.get('kind', '<missing>')}"] += 1
            if progress := as_dict(payload.get("progress")):
                progress_record = dict(record)
                progress_record["progress"] = progress
                progress_record["stopped"] = False
                progresses_by_key[(player, day, as_int(payload.get("commandIndex")))] = progress_record
        elif label == "progress":
            progress = as_dict(payload.get("progress"))
            progress_record = dict(record)
            progress_record["progress"] = progress
            progress_record["stopped"] = bool(payload.get("stopped"))
            progresses_by_key[key] = progress_record
            executed = as_list(progress.get("executed"))
            failed = as_list(progress.get("failed"))
            remaining = as_list(progress.get("remaining"))
            progress_counts["executed"] += len(executed)
            progress_counts["failed"] += len(failed)
            progress_counts["remaining"] += len(remaining)
            if payload.get("stopped"):
                progress_counts["stopped_batches"] += 1
            count_actions(executed_actions, executed)
            count_actions(failed_actions, failed)
            for item in failed:
                error = as_dict(item).get("error", "<missing>")
                failure_errors[str(error)] += 1
        elif label in {"input", "imperative-input"}:
            script_input = as_dict(payload.get("input"))
            input_record = dict(record)
            input_record["input"] = script_input
            inputs_by_key[key] = input_record
            series_key = (str(path.parent), player)
            first = first_inputs.get(series_key)
            if not first or (day_number(path), event) < (as_int(first.get("day"), 10**9), as_int(first.get("event"), 10**9)):
                first_inputs[series_key] = input_record
            latest = latest_inputs.get(series_key)
            if not latest or (day_number(path), event) >= (as_int(latest.get("day"), -1), as_int(latest.get("event"), -1)):
                latest_inputs[series_key] = input_record
            updates = as_list(nested(script_input, "updates", "events"))
            opponent_updates = as_list(nested(script_input, "opponentUpdates", "events"))
            analysis = as_dict(script_input.get("analysis"))
            action_space = as_dict(script_input.get("actionSpace"))
            progress_counts["input_update_events"] += len(updates)
            progress_counts["input_opponent_events"] += len(opponent_updates)
            analysis_counts["defense_alerts"] += len(as_list(analysis.get("defenseAlerts")))
            analysis_counts["hero_threat_alerts"] += len(as_list(analysis.get("heroThreatAlerts")))
            analysis_counts["visible_enemy_heroes"] += len(as_list(analysis.get("visibleEnemyHeroes")))
            analysis_counts["visible_enemy_towns"] += len(as_list(analysis.get("visibleEnemyTowns")))
            reachable_objects = as_list(action_space.get("reachableObjects"))
            movement_options = as_list(action_space.get("movementOptions"))
            analysis_counts["reachable_object_candidates"] += len(reachable_objects)
            analysis_counts["movement_candidates"] += len(movement_options)
            for candidate in reachable_objects + movement_options:
                candidate_dict = as_dict(candidate)
                risk = str(candidate_dict.get("risk", "<missing>"))
                candidate_risks[risk] += 1
                if candidate_dict.get("safe") is False:
                    analysis_counts["unsafe_candidates"] += 1
            for event in updates:
                update_types[str(as_dict(event).get("type", "<missing>"))] += 1
            for event in opponent_updates:
                opponent_update_types[str(as_dict(event).get("type", "<missing>"))] += 1

    mistakes = analyze_mistakes(inputs_by_key, outputs_by_key, progresses_by_key)
    mistake_counts = Counter(str(item.get("type", "<missing>")) for item in mistakes)
    return {
        "files": len(files),
        "parsed": len(files) - len(parse_errors),
        "parse_errors": parse_errors,
        "labels": counter_to_dict(labels),
        "players": counter_to_dict(players),
        "scripts": counter_to_dict(scripts),
        "days": counter_to_dict(days),
        "output_statuses": counter_to_dict(output_statuses),
        "output_intents": counter_to_dict(output_intents),
        "requested_actions": counter_to_dict(requested_actions),
        "executed_actions": counter_to_dict(executed_actions),
        "failed_actions": counter_to_dict(failed_actions),
        "failure_errors": counter_to_dict(failure_errors),
        "progress": counter_to_dict(progress_counts),
        "analysis": counter_to_dict(analysis_counts),
        "candidate_risks": counter_to_dict(candidate_risks),
        "update_types": counter_to_dict(update_types),
        "opponent_update_types": counter_to_dict(opponent_update_types),
        "quality": aggregate_quality(latest_inputs),
        "mapProgress": aggregate_map_progress(first_inputs, latest_inputs),
        "mistakes": {
            "total": len(mistakes),
            "important": sum(1 for item in mistakes if item.get("type") in IMPORTANT_MISTAKE_TYPES),
            "counts": counter_to_dict(mistake_counts),
            "items": mistakes[: max(0, max_mistakes)],
            "truncated": max(0, len(mistakes) - max(0, max_mistakes)),
        },
    }


def print_counter(title: str, values: dict[str, int], limit: int) -> None:
    if not values:
        return
    print(f"{title}:")
    for key, count in list(values.items())[:limit]:
        print(f"  {key}: {count}")


def print_text(summary: dict[str, Any], limit: int) -> None:
    print(f"files: {summary['files']}")
    print(f"parsed: {summary['parsed']}")
    print_counter("labels", summary["labels"], limit)
    print_counter("players", summary["players"], limit)
    print_counter("scripts", summary["scripts"], limit)
    print_counter("days", summary["days"], limit)
    print_counter("output statuses", summary["output_statuses"], limit)
    print_counter("requested actions", summary["requested_actions"], limit)
    print_counter("executed actions", summary["executed_actions"], limit)
    print_counter("failed actions", summary["failed_actions"], limit)
    print_counter("failure errors", summary["failure_errors"], limit)
    print_counter("progress totals", summary["progress"], limit)
    print_counter("analysis totals", summary["analysis"], limit)
    print_counter("candidate risks", summary["candidate_risks"], limit)
    print_counter("update types", summary["update_types"], limit)
    print_counter("opponent update types", summary["opponent_update_types"], limit)
    quality = as_dict(summary.get("quality"))
    print(f"quality score: {quality.get('score', 0)}")
    print_counter("quality totals", as_dict(quality.get("totals")), limit)
    map_progress = as_dict(summary.get("mapProgress"))
    print(f"map progress score: {map_progress.get('score', 0)}")
    print_counter("map progress totals", as_dict(map_progress.get("totals")), limit)
    print_counter("mistakes", as_dict(nested(summary, "mistakes", "counts")), limit)
    print_counter("intents", summary["output_intents"], limit)
    if summary["parse_errors"]:
        print("parse errors:")
        for error in summary["parse_errors"][:limit]:
            print(f"  {error}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize ScriptedAdventureAI trace JSON files.")
    parser.add_argument("paths", nargs="+", help="Trace files or directories containing trace JSON files.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON summary.")
    parser.add_argument("--limit", type=int, default=12, help="Maximum entries per text section.")
    parser.add_argument("--mistake-limit", type=int, default=100, help="Maximum mistake items included in JSON output.")
    args = parser.parse_args()

    files = iter_trace_files(args.paths)
    summary = summarize(files, max_mistakes=max(0, args.mistake_limit))
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print_text(summary, max(1, args.limit))
    return 1 if summary["parse_errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
