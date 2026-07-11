#!/usr/bin/env python3
"""Unit tests for ScriptedAdventureAI trace summarization."""

from __future__ import annotations

import unittest
import sys
import json
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from summarizeAdventureTrace import analyze_mistakes, iter_trace_files, summarize


def input_record(script_input: dict) -> dict:
    return {
        "path": "input.json",
        "player": "red",
        "script": "scripts/ai/defaultAdventure.lua",
        "day": 1,
        "callIndex": 0,
        "input": script_input,
    }


def output_record(actions: list[dict], progress: dict | None = None, status: str = "need_replan") -> dict:
    record = {
        "path": "output.json",
        "player": "red",
        "script": "scripts/ai/defaultAdventure.lua",
        "day": 1,
        "callIndex": 0,
        "output": {
            "status": status,
            "actions": actions,
        },
    }
    if progress is not None:
        record["progress"] = progress
    return record


class DefensePressureMistakeTest(unittest.TestCase):
    def mistakes_for(self, action_space: dict, actions: list[dict], progress: dict | None = None) -> list[dict]:
        key = ("red", 1, 0)
        script_input = {
            "analysis": {
                "defenseAlerts": [
                    {
                        "level": "critical",
                        "town_id": 10,
                    }
                ]
            },
            "actionSpace": action_space,
        }
        return analyze_mistakes(
            {key: input_record(script_input)},
            {key: output_record(actions, progress)},
            {},
        )

    def test_defense_pressure_without_candidates_is_not_actionable(self) -> None:
        mistakes = self.mistakes_for(
            {
                "movementOptions": [
                    {
                        "planAction": {
                            "type": "move_hero",
                            "hero_id": 5,
                        }
                    }
                ]
            },
            [
                {
                    "type": "move_hero",
                    "hero_id": 5,
                }
            ],
        )
        self.assertNotIn("defense_pressure_without_response", {item["type"] for item in mistakes})

    def test_missed_recruit_under_pressure_is_reported(self) -> None:
        mistakes = self.mistakes_for(
            {
                "recruitOptions": [
                    {
                        "planAction": {
                            "type": "recruit",
                            "town_id": 10,
                            "level": 0,
                        }
                    }
                ],
                "movementOptions": [
                    {
                        "planAction": {
                            "type": "move_hero",
                            "hero_id": 5,
                        }
                    }
                ],
            },
            [
                {
                    "type": "move_hero",
                    "hero_id": 5,
                }
            ],
        )
        self.assertIn("defense_pressure_without_response", {item["type"] for item in mistakes})

    def test_reinforce_transfer_counts_as_defensive_response(self) -> None:
        action = {
            "type": "transfer_army",
            "source_id": 5,
            "destination_id": 10,
            "source_slot": 0,
        }
        mistakes = self.mistakes_for(
            {
                "armyTransferOptions": [
                    {
                        "transferKindId": 2,
                        "planAction": action,
                    }
                ]
            },
            [action],
        )
        self.assertNotIn("defense_pressure_without_response", {item["type"] for item in mistakes})

    def test_native_task_touching_alert_town_counts_as_defensive_response(self) -> None:
        progress = {
            "executed": [
                {
                    "type": "nullkiller_turn_slice",
                    "didWork": True,
                    "passes": [
                        {
                            "adventure": {
                                "didExecute": True,
                                "attemptedTasks": [
                                    {
                                        "executed": True,
                                        "task": {
                                            "affectedObjectIds": [10],
                                        },
                                    }
                                ],
                            }
                        }
                    ],
                }
            ],
            "failed": [],
            "remaining": [],
        }
        mistakes = self.mistakes_for(
            {
                "recruitOptions": [
                    {
                        "planAction": {
                            "type": "recruit",
                            "town_id": 10,
                            "level": 0,
                        }
                    }
                ]
            },
            [],
            progress,
        )
        self.assertNotIn("defense_pressure_without_response", {item["type"] for item in mistakes})


class HeroThreatMistakeTest(unittest.TestCase):
    def mistakes_for(self, movement_options: list[dict], actions: list[dict], progress: dict | None = None) -> list[dict]:
        key = ("red", 1, 0)
        script_input = {
            "analysis": {
                "heroThreatAlerts": [
                    {
                        "level": "critical",
                        "hero_id": 5,
                        "distanceSquared": 4,
                        "enemyPosition": {
                            "x": 0,
                            "y": 0,
                            "z": 0,
                        },
                    },
                    {
                        "level": "critical",
                        "hero_id": 6,
                        "distanceSquared": 4,
                        "enemyPosition": {
                            "x": 10,
                            "y": 10,
                            "z": 0,
                        },
                    },
                ]
            },
            "actionSpace": {
                "movementOptions": movement_options,
            },
        }
        return analyze_mistakes(
            {key: input_record(script_input)},
            {key: output_record(actions, progress)},
            {},
        )

    def test_moving_one_threatened_hero_counts_as_progress(self) -> None:
        mistakes = self.mistakes_for(
            [
                {
                    "hero_id": 5,
                    "safe": True,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 5,
                    },
                    "path": {
                        "destination": {
                            "x": 5,
                            "y": 0,
                            "z": 0,
                        }
                    },
                },
                {
                    "hero_id": 6,
                    "safe": True,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 6,
                    },
                    "path": {
                        "destination": {
                            "x": 10,
                            "y": 15,
                            "z": 0,
                        }
                    },
                },
            ],
            [
                {
                    "type": "move_hero",
                    "hero_id": 5,
                }
            ],
        )
        self.assertNotIn("hero_threat_without_escape", {item["type"] for item in mistakes})

    def test_not_moving_any_actionable_threat_is_reported(self) -> None:
        mistakes = self.mistakes_for(
            [
                {
                    "hero_id": 5,
                    "safe": True,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 5,
                    },
                    "path": {
                        "destination": {
                            "x": 5,
                            "y": 0,
                            "z": 0,
                        }
                    },
                }
            ],
            [
                {
                    "type": "build",
                    "town_id": 10,
                }
            ],
        )
        self.assertIn("hero_threat_without_escape", {item["type"] for item in mistakes})


    def test_threat_without_safe_move_candidate_is_not_actionable(self) -> None:
        mistakes = self.mistakes_for(
            [
                {
                    "hero_id": 5,
                    "safe": False,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 5,
                    },
                    "path": {
                        "destination": {
                            "x": 5,
                            "y": 0,
                            "z": 0,
                        }
                    },
                }
            ],
            [
                {
                    "type": "build",
                    "town_id": 10,
                }
            ],
        )
        self.assertNotIn("hero_threat_without_escape", {item["type"] for item in mistakes})

    def test_threat_without_distance_improving_move_is_not_actionable(self) -> None:
        mistakes = self.mistakes_for(
            [
                {
                    "hero_id": 5,
                    "safe": True,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 5,
                    },
                    "path": {
                        "destination": {
                            "x": 1,
                            "y": 0,
                            "z": 0,
                        }
                    },
                }
            ],
            [
                {
                    "type": "build",
                    "town_id": 10,
                }
            ],
        )
        self.assertNotIn("hero_threat_without_escape", {item["type"] for item in mistakes})

    def test_native_task_touching_threatened_hero_counts_as_progress(self) -> None:
        progress = {
            "executed": [
                {
                    "type": "nullkiller_turn_slice",
                    "didWork": True,
                    "passes": [
                        {
                            "adventure": {
                                "didExecute": True,
                                "attemptedTasks": [
                                    {
                                        "executed": True,
                                        "task": {
                                            "affectedObjectIds": [5],
                                        },
                                    }
                                ],
                            }
                        }
                    ],
                }
            ],
            "failed": [],
            "remaining": [],
        }
        mistakes = self.mistakes_for(
            [
                {
                    "hero_id": 5,
                    "safe": True,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 5,
                    },
                    "path": {
                        "destination": {
                            "x": 5,
                            "y": 0,
                            "z": 0,
                        }
                    },
                }
            ],
            [],
            progress,
        )
        self.assertNotIn("hero_threat_without_escape", {item["type"] for item in mistakes})


class ImperativeTraceSummaryTest(unittest.TestCase):
    def test_trace_file_discovery_recurses_batch_run_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = root / "scenario" / "scenario-run-001"
            trace_dir = run / "cache" / "vcmi" / "scriptedAdventureAI"
            trace_dir.mkdir(parents=True)

            (root / "results.json").write_text("{}", encoding="utf-8")
            (run / "run.json").write_text("{}", encoding="utf-8")
            event_path = trace_dir / "player-red-day-1-event-0-imperative-input.json"
            event_path.write_text("{}", encoding="utf-8")

            self.assertEqual(iter_trace_files([str(root)]), [event_path])

    def test_imperative_trace_labels_are_counted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            input_path = root / "player-red-day-1-event-0-imperative-input.json"
            input_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-input",
                        "player": "red",
                        "script": "ai/defaultAdventure.lua",
                        "payload": {
                            "input": {
                                "state": {"heroes": [], "towns": [], "resources": {}},
                                "updates": {"events": []},
                                "opponentUpdates": {"events": []},
                                "analysis": {},
                                "actionSpace": {},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            command_path = root / "player-red-day-1-event-1-imperative-command.json"
            command_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-command",
                        "player": "red",
                        "script": "ai/defaultAdventure.lua",
                        "payload": {
                            "commandIndex": 0,
                            "command": {
                                "kind": "execute",
                                "payload": {"type": "build", "town_id": 7, "building_id": 12},
                            },
                            "response": {
                                "ok": True,
                                "result": {"ok": True, "type": "build", "stop": False},
                            },
                            "progress": {"executed": [], "failed": [], "remaining": []},
                        },
                    }
                ),
                encoding="utf-8",
            )
            output_path = root / "player-red-day-1-event-2-imperative-output.json"
            output_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-output",
                        "player": "red",
                        "script": "ai/defaultAdventure.lua",
                        "payload": {
                            "output": {
                                "status": "fallback",
                                "actions": [],
                                "intent": "delegate",
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )

            summary = summarize([input_path, command_path, output_path])

        self.assertEqual(summary["labels"]["imperative-input"], 1)
        self.assertEqual(summary["labels"]["imperative-command"], 1)
        self.assertEqual(summary["labels"]["imperative-output"], 1)
        self.assertEqual(summary["output_statuses"]["fallback"], 1)
        self.assertEqual(summary["requested_actions"]["build"], 1)
        self.assertEqual(summary["executed_actions"]["build"], 1)
        self.assertEqual(summary["output_intents"]["delegate"], 1)

    def test_imperative_output_progress_suppresses_false_idle_and_stopped_mistakes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            input_path = root / "player-red-day-1-event-0-imperative-input.json"
            input_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-input",
                        "player": "red",
                        "script": "ai/boundedNullkillerControl.lua",
                        "payload": {
                            "input": {
                                "state": {"heroes": [], "towns": [], "resources": {}},
                                "updates": {"events": []},
                                "opponentUpdates": {"events": []},
                                "analysis": {},
                                "actionSpace": {
                                    "movementOptions": [
                                        {
                                            "planAction": {
                                                "type": "move_hero",
                                                "hero_id": 5,
                                            }
                                        }
                                    ]
                                },
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            command_path = root / "player-red-day-1-event-1-imperative-command.json"
            command_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-command",
                        "player": "red",
                        "script": "ai/boundedNullkillerControl.lua",
                        "payload": {
                            "commandIndex": 0,
                            "command": {
                                "kind": "execute",
                                "payload": {"type": "nullkiller_turn_slice"},
                            },
                            "response": {
                                "ok": True,
                                "result": {"ok": True, "type": "nullkiller_turn_slice", "stop": True},
                            },
                            "progress": {
                                "executed": [
                                    {
                                        "type": "nullkiller_turn_slice",
                                        "didWork": False,
                                        "shouldStopTurn": True,
                                    }
                                ],
                                "failed": [],
                                "remaining": [],
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )
            output_path = root / "player-red-day-1-event-2-imperative-output.json"
            output_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-output",
                        "player": "red",
                        "script": "ai/boundedNullkillerControl.lua",
                        "payload": {
                            "output": {
                                "status": "end_turn",
                                "actions": [],
                                "intent": "bounded native stop",
                            },
                            "progress": {
                                "executed": [
                                    {
                                        "type": "nullkiller_turn_slice",
                                        "didWork": True,
                                    },
                                    {
                                        "type": "nullkiller_turn_slice",
                                        "didWork": False,
                                        "shouldStopTurn": True,
                                    },
                                ],
                                "failed": [],
                                "remaining": [],
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )

            summary = summarize([input_path, command_path, output_path])

        mistakes = summary["mistakes"]["counts"]
        self.assertNotIn("idle_with_candidates", mistakes)
        self.assertNotIn("stopped_batch", mistakes)

    def test_bounded_max_pass_intent_suppresses_false_idle_mistake(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            input_path = root / "player-red-day-1-event-0-imperative-input.json"
            input_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-input",
                        "player": "red",
                        "script": "ai/candidates/boundedNullkillerControl.lua",
                        "payload": {
                            "input": {
                                "state": {"heroes": [], "towns": [], "resources": {}},
                                "updates": {"events": []},
                                "opponentUpdates": {"events": []},
                                "analysis": {},
                                "actionSpace": {
                                    "buildOptions": [
                                        {
                                            "planAction": {
                                                "type": "build",
                                                "town_id": 17,
                                                "building_id": 5,
                                            }
                                        }
                                    ],
                                    "reachableObjects": [
                                        {
                                            "planAction": {
                                                "type": "visit_object",
                                                "hero_id": 5,
                                                "object_id": 9,
                                            },
                                            "safe": True,
                                        }
                                    ],
                                },
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            output_path = root / "player-red-day-1-event-1-imperative-output.json"
            output_path.write_text(
                json.dumps(
                    {
                        "label": "imperative-output",
                        "player": "red",
                        "script": "ai/candidates/boundedNullkillerControl.lua",
                        "payload": {
                            "output": {
                                "status": "end_turn",
                                "actions": [],
                                "intent": "bounded Nullkiller control accepted native max-pass limit",
                            },
                            "progress": {
                                "executed": [
                                    {
                                        "type": "nullkiller_turn_slice",
                                        "didWork": True,
                                    }
                                ],
                                "failed": [],
                                "remaining": [],
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )

            summary = summarize([input_path, output_path])

        self.assertNotIn("idle_with_candidates", summary["mistakes"]["counts"])

    def test_map_progress_deltas_are_summarized(self) -> None:
        def write_input(
            path: Path,
            explored: int,
            ratio: float,
            visible_objects: int,
            passable: int,
            roads: int,
            control: list[dict],
            kinds: list[dict],
        ) -> None:
            path.write_text(
                json.dumps(
                    {
                        "label": "imperative-input",
                        "player": "red",
                        "script": "ai/defaultAdventure.lua",
                        "payload": {
                            "input": {
                                "state": {
                                    "heroes": [],
                                    "towns": [],
                                    "resources": {},
                                    "map": {
                                        "totalTiles": 100,
                                        "exploredTilesCount": explored,
                                        "exploredRatio": ratio,
                                        "visibleObjectsCount": visible_objects,
                                        "exploredPassableTilesCount": passable,
                                        "exploredRoadTilesCount": roads,
                                        "visibleControl": {
                                            "objectCountsByControl": control,
                                            "objectCountsByKind": kinds,
                                            "objectCountsByOwner": [
                                                {"ownerId": 0, "count": 1},
                                            ],
                                        },
                                    },
                                },
                                "updates": {"events": []},
                                "opponentUpdates": {"events": []},
                                "analysis": {},
                                "actionSpace": {},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first_path = root / "player-red-day-1-event-0-imperative-input.json"
            final_path = root / "player-red-day-3-event-0-imperative-input.json"
            write_input(
                first_path,
                10,
                0.10,
                2,
                6,
                1,
                [
                    {"controlId": 0, "count": 1},
                    {"controlId": 3, "count": 1},
                ],
                [
                    {"kindId": 2, "count": 1},
                    {"kindId": 3, "count": 1},
                ],
            )
            write_input(
                final_path,
                25,
                0.25,
                5,
                16,
                3,
                [
                    {"controlId": 0, "count": 2},
                    {"controlId": 2, "count": 1},
                    {"controlId": 3, "count": 2},
                ],
                [
                    {"kindId": 2, "count": 2},
                    {"kindId": 3, "count": 2},
                    {"kindId": 5, "count": 1},
                ],
            )

            summary = summarize([first_path, final_path])

        progress = summary["mapProgress"]
        self.assertEqual(progress["players"], 1)
        self.assertEqual(progress["score"], 118)
        self.assertEqual(progress["totals"]["exploredTilesDelta"], 15)
        self.assertEqual(progress["totals"]["finalExploredTiles"], 25)
        self.assertEqual(progress["totals"]["visibleObjectsDelta"], 3)
        self.assertEqual(progress["totals"]["selfVisibleObjectsDelta"], 1)
        self.assertEqual(progress["totals"]["enemyVisibleObjectsDelta"], 1)
        self.assertEqual(progress["totals"]["mineObjectsDelta"], 1)
        self.assertEqual(progress["totals"]["townObjectsDelta"], 1)
        delta = progress["perPlayer"][0]["delta"]
        self.assertAlmostEqual(delta["exploredRatio"], 0.15)
        self.assertEqual(delta["objectCountsByControlId"]["2"], 1)
        self.assertEqual(delta["objectCountsByKindId"]["5"], 1)


class IgnoredBetterObjectMistakeTest(unittest.TestCase):
    def mistakes_for(self, memory: dict | None = None) -> list[dict]:
        key = ("red", 1, 0)
        chosen_action = {
            "type": "visit_object",
            "hero_id": 5,
            "object_id": 1,
            "route_id": "chosen",
        }
        script_input = {
            "memory": memory or {},
            "analysis": {},
            "actionSpace": {
                "reachableObjects": [
                    {
                        "hero_id": 5,
                        "safe": True,
                        "value": 100,
                        "object": {
                            "id": 1,
                        },
                        "planAction": chosen_action,
                    },
                    {
                        "hero_id": 5,
                        "safe": True,
                        "value": 1000,
                        "object": {
                            "id": 2,
                        },
                        "planAction": {
                            "type": "visit_object",
                            "hero_id": 5,
                            "object_id": 2,
                            "route_id": "better",
                        },
                    },
                ]
            },
        }
        return analyze_mistakes(
            {key: input_record(script_input)},
            {key: output_record([chosen_action])},
            {},
        )

    def test_unvisited_better_object_is_reported(self) -> None:
        mistakes = self.mistakes_for()
        self.assertIn("ignored_better_object", {item["type"] for item in mistakes})

    def test_visited_better_object_is_not_reported(self) -> None:
        mistakes = self.mistakes_for({"visitedTargets": {"2": True}})
        self.assertNotIn("ignored_better_object", {item["type"] for item in mistakes})


if __name__ == "__main__":
    unittest.main()
