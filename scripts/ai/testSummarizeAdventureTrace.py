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

from summarizeAdventureTrace import analyze_mistakes, summarize


def input_record(script_input: dict) -> dict:
    return {
        "path": "input.json",
        "player": "red",
        "script": "scripts/ai/defaultAdventure.lua",
        "day": 1,
        "callIndex": 0,
        "input": script_input,
    }


def output_record(actions: list[dict]) -> dict:
    return {
        "path": "output.json",
        "player": "red",
        "script": "scripts/ai/defaultAdventure.lua",
        "day": 1,
        "callIndex": 0,
        "output": {
            "status": "need_replan",
            "actions": actions,
        },
    }


class DefensePressureMistakeTest(unittest.TestCase):
    def mistakes_for(self, action_space: dict, actions: list[dict]) -> list[dict]:
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
            {key: output_record(actions)},
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


class HeroThreatMistakeTest(unittest.TestCase):
    def mistakes_for(self, movement_options: list[dict], actions: list[dict]) -> list[dict]:
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
            {key: output_record(actions)},
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


class ImperativeTraceSummaryTest(unittest.TestCase):
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
