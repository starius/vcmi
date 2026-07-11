#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from mineAdventureTraceMistakes import fixture_items, imperative_fixture  # noqa: E402


def summary_with_fixture(fixture: dict) -> dict:
    return {
        "mistakes": {
            "items": [
                {
                    "type": "defense_pressure_without_response",
                    "severity": 4,
                    "trace": "trace.json",
                    "description": "description",
                    "fixture": fixture,
                }
            ]
        }
    }


class MineAdventureTraceMistakesTest(unittest.TestCase):
    def test_fixture_items_copy_source_before_annotating(self) -> None:
        source = {
            "name": "sample",
            "script": "scripts/ai/defaultAdventure.lua",
            "input": {"state": {}},
            "expect": {
                "actionsContain": [
                    {
                        "type": "recruit",
                    }
                ]
            },
        }

        fixtures = fixture_items(summary_with_fixture(source))

        self.assertEqual(len(fixtures), 1)
        self.assertIn("sourceMistake", fixtures[0])
        self.assertNotIn("sourceMistake", source)

    def test_imperative_fixture_rewrites_action_expectations_to_command_expectations(self) -> None:
        source = {
            "name": "sample",
            "script": "scripts/ai/defaultAdventure.lua",
            "input": {"state": {}},
            "expect": {
                "firstAction": {"type": "recruit"},
                "actionsExact": [{"type": "recruit"}],
                "actionsContain": [{"type": "build"}],
                "actionsDoNotContain": [{"type": "end_turn"}],
                "memoryContains": {"version": 1},
            },
        }

        fixture = imperative_fixture(source)

        self.assertEqual(fixture["mode"], "imperative")
        self.assertNotIn("firstAction", fixture["expect"])
        self.assertNotIn("actionsExact", fixture["expect"])
        self.assertNotIn("actionsContain", fixture["expect"])
        self.assertNotIn("actionsDoNotContain", fixture["expect"])
        self.assertEqual(fixture["expect"]["firstCommand"], {"type": "recruit"})
        self.assertEqual(fixture["expect"]["commandsExact"], [{"type": "recruit"}])
        self.assertEqual(fixture["expect"]["commandsContain"], [{"type": "build"}])
        self.assertEqual(fixture["expect"]["commandsDoNotContain"], [{"type": "end_turn"}])
        self.assertEqual(fixture["expect"]["memoryContains"], {"version": 1})
        self.assertIn("refreshInput", fixture["notes"])

    def test_fixture_items_can_emit_imperative_fixtures(self) -> None:
        source = {
            "name": "sample",
            "script": "scripts/ai/defaultAdventure.lua",
            "input": {"state": {}},
            "expect": {
                "actionsContain": [{"type": "recruit"}],
            },
        }

        fixtures = fixture_items(summary_with_fixture(source), "imperative")

        self.assertEqual(fixtures[0]["mode"], "imperative")
        self.assertEqual(fixtures[0]["expect"]["commandsContain"], [{"type": "recruit"}])
        self.assertNotIn("actionsContain", fixtures[0]["expect"])


if __name__ == "__main__":
    unittest.main()
