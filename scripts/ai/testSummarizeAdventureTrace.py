#!/usr/bin/env python3
"""Unit tests for ScriptedAdventureAI trace summarization."""

from __future__ import annotations

import unittest
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from summarizeAdventureTrace import analyze_mistakes


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
                    },
                    {
                        "level": "critical",
                        "hero_id": 6,
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
                },
                {
                    "hero_id": 6,
                    "safe": True,
                    "planAction": {
                        "type": "move_hero",
                        "hero_id": 6,
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


if __name__ == "__main__":
    unittest.main()
