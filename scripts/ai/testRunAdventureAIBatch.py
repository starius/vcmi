#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from runAdventureAIBatch import (
    annotate_infrastructure_failure,
    compact_result,
    load_scenarios,
    run_one,
    run_one_with_infrastructure_retries,
    run_outcome,
    summarize_results,
)


class RunAdventureAIBatchTest(unittest.TestCase):
    def test_evaluation_file_contains_small_generated_training_and_heldout_corpus(self) -> None:
        def args_for(group: str) -> argparse.Namespace:
            return argparse.Namespace(
                scenario_file=SCRIPT_DIR / "evaluationScenarios.json",
                map=[],
                runs=1,
                testdays=0,
                timeout=300,
                idle_timeout=0.0,
                infrastructure_retries=0,
                extra_arg=[],
                group=[group],
                stage=["outcome"],
                kind=["generated-random"],
                include_disabled=True,
            )

        training = load_scenarios(args_for("training"))
        heldout = load_scenarios(args_for("heldout"))

        self.assertEqual([scenario["seed"] for scenario in training], list(range(53001, 53011)))
        self.assertEqual([scenario["gameSeed"] for scenario in training], list(range(63001, 63011)))
        self.assertEqual([scenario["seed"] for scenario in heldout], list(range(53011, 53017)))
        self.assertEqual([scenario["gameSeed"] for scenario in heldout], list(range(63011, 63017)))

        for scenario in training + heldout:
            self.assertEqual(scenario["randomMap"]["size"], "S")
            self.assertEqual(scenario["randomMap"]["levels"], 2)
            self.assertEqual(scenario["randomMap"]["players"], 2)
            self.assertEqual(scenario["randomMap"]["water"], "none")
            self.assertEqual(scenario["randomMap"]["monsterStrength"], "normal")
            self.assertEqual(scenario["testdays"], 0)
            self.assertEqual(scenario["timeout"], 1800)
            self.assertEqual(scenario["idle_timeout"], 240.0)
            self.assertEqual(scenario["infrastructure_retries"], 1)

    def test_legacy_sixteen_map_corpus_uses_watchdogs(self) -> None:
        args = argparse.Namespace(
            scenario_file=SCRIPT_DIR / "rmgSmallUndergroundNoWater16.json",
            map=[],
            runs=1,
            testdays=0,
            timeout=300,
            idle_timeout=0.0,
            infrastructure_retries=0,
            extra_arg=[],
            group=["training"],
            stage=["outcome"],
            kind=["generated-random"],
            include_disabled=True,
        )

        scenarios = load_scenarios(args)

        self.assertEqual([scenario["seed"] for scenario in scenarios], list(range(53001, 53017)))
        self.assertEqual([scenario["gameSeed"] for scenario in scenarios], list(range(63001, 63017)))
        for scenario in scenarios:
            self.assertEqual(scenario["timeout"], 1800)
            self.assertEqual(scenario["idle_timeout"], 240.0)
            self.assertEqual(scenario["infrastructure_retries"], 1)

    def test_run_outcome_uses_last_started_day_for_terminal_result(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            stdout = Path(temp_dir) / "stdout.log"
            stdout.write_text(
                "\x1b[0m\x1b[1;32mPlayer 0 (red) starting turn, day 27\n"
                "\x1b[0m\x1b[1;32mPlayer 1 (blue) starting turn, day 28\n"
                "\x1b[0m\x1b[1;32mRed player lost. Ending game.\n",
                encoding="utf-8",
            )

            outcome = run_outcome(stdout, timed_out=False, idle_timed_out=False, return_code=0)

            self.assertEqual(outcome["result"], "red_loss")
            self.assertEqual(outcome["lastStartedDay"], 28)
            self.assertEqual(outcome["completedDays"], 28)

    def test_run_outcome_keeps_exact_day_limit_count(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            stdout = Path(temp_dir) / "stdout.log"
            stdout.write_text(
                "Player 0 (red) starting turn, day 8\n"
                "Reached test day limit 7 after completing day 7\n",
                encoding="utf-8",
            )

            outcome = run_outcome(stdout, timed_out=False, idle_timed_out=False, return_code=0)

            self.assertEqual(outcome["result"], "day_limit")
            self.assertEqual(outcome["lastStartedDay"], 8)
            self.assertEqual(outcome["completedDays"], 7)

    def test_run_one_classifies_stdout_idle_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            client = root / "fake-client.py"
            client.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import time

                    print("fake client started", flush=True)
                    time.sleep(10)
                    """
                ),
                encoding="utf-8",
            )
            client.chmod(0o755)

            args = argparse.Namespace(
                client=str(client),
                ai=["ScriptedAdventureAI", "Nullkiller2"],
                runs=1,
                testdays=0,
                timeout=10,
                idle_timeout=0.2,
                infrastructure_retries=0,
                exit_grace_after_outcome=10.0,
                output=root / "runs",
                cwd=None,
                clean=True,
                extra_arg=[],
                script=None,
                player_script=[],
                red_script=None,
                blue_script=None,
                trace=False,
                json=False,
            )
            scenario = {
                "name": "idle",
                "group": "training",
                "stage": "outcome",
                "kind": "unit",
                "map": "unit-test-map",
                "runs": 1,
                "testdays": 0,
                "timeout": 10,
                "idle_timeout": 0.2,
                "extra_arg": [],
                "enabled": True,
                "tags": [],
            }

            result = run_one(args, scenario, 1)

            self.assertTrue(result["idleTimedOut"])
            self.assertFalse(result["timedOut"])
            self.assertEqual(result["outcome"]["result"], "idle_timeout")
            self.assertNotEqual(result["returnCode"], 0)
            self.assertGreater(result["stdoutSummary"]["bytes"], 0)
            self.assertEqual(result["stdoutSummary"]["tailSignature"], "unknown")
            self.assertIn("fake client started", result["stdoutSummary"]["tail"])
            self.assertGreaterEqual(result["stdoutSummary"]["lastOutputAgeSeconds"], 0.0)
            self.assertEqual(compact_result(result)["stdoutTailSignature"], "unknown")
            summary = summarize_results([result])
            self.assertEqual(summary["idleTimeouts"], 1)
            self.assertEqual(summary["nonzeroExit"], 0)

    def test_summarize_results_reports_terminal_and_script_wins(self) -> None:
        results = []
        for index in range(9):
            results.append({
                "scenario": f"lua-win-{index}",
                "run": index + 1,
                "ai": ["ScriptedAdventureAI", "Nullkiller2"],
                "outcome": {"result": "red_win", "completedDays": 30 + index},
                "timedOut": False,
                "idleTimedOut": False,
                "returnCode": 0,
                "elapsedSeconds": 1.0,
            })
        for index in range(5):
            results.append({
                "scenario": f"nullkiller-win-{index}",
                "run": index + 10,
                "ai": ["ScriptedAdventureAI", "Nullkiller2"],
                "outcome": {"result": "red_loss", "completedDays": 40 + index},
                "timedOut": False,
                "idleTimedOut": False,
                "returnCode": 0,
                "elapsedSeconds": 1.0,
            })
        for index in range(2):
            results.append({
                "scenario": f"idle-{index}",
                "run": index + 15,
                "ai": ["ScriptedAdventureAI", "Nullkiller2"],
                "outcome": {"result": "idle_timeout", "completedDays": 50 + index},
                "timedOut": False,
                "idleTimedOut": True,
                "returnCode": -15,
                "elapsedSeconds": 1.0,
            })

        summary = summarize_results(results)

        self.assertEqual(summary["runs"], 16)
        self.assertEqual(summary["terminalRuns"], 14)
        self.assertEqual(summary["nonTerminalRuns"], 2)
        self.assertEqual(summary["scriptedAdventureAIWins"], 9)
        self.assertEqual(summary["nullkiller2Wins"], 5)
        self.assertEqual(summary["redWins"], 9)
        self.assertEqual(summary["redLosses"], 5)
        self.assertEqual(summary["idleTimeouts"], 2)

    def test_run_one_passes_player_script_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            client = root / "fake-client.py"
            client.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import os

                    keys = [
                        "VCMI_SCRIPTED_ADVENTURE_SCRIPT",
                        "VCMI_SCRIPTED_ADVENTURE_RED_SCRIPT",
                        "VCMI_SCRIPTED_ADVENTURE_BLUE_SCRIPT",
                        "VCMI_SCRIPTED_ADVENTURE_PLAYER_1_SCRIPT",
                    ]
                    for key in keys:
                        print(f"{key}={os.environ.get(key, '')}", flush=True)
                    """
                ),
                encoding="utf-8",
            )
            client.chmod(0o755)

            red_script = root / "red.lua"
            blue_script = root / "blue.lua"
            red_script.write_text("return {}", encoding="utf-8")
            blue_script.write_text("return {}", encoding="utf-8")

            args = argparse.Namespace(
                client=str(client),
                ai=["ScriptedAdventureAI", "ScriptedAdventureAI"],
                runs=1,
                testdays=0,
                timeout=10,
                idle_timeout=0.0,
                infrastructure_retries=0,
                exit_grace_after_outcome=10.0,
                output=root / "runs",
                cwd=None,
                clean=True,
                extra_arg=[],
                script="ai/global.lua",
                player_script=["1=ai/player-one.lua"],
                red_script=str(red_script),
                blue_script=None,
                trace=False,
                json=False,
            )
            scenario = {
                "name": "player-scripts",
                "group": "training",
                "stage": "outcome",
                "kind": "unit",
                "map": "unit-test-map",
                "runs": 1,
                "testdays": 0,
                "timeout": 10,
                "idle_timeout": 0.0,
                "extra_arg": [],
                "enabled": True,
                "tags": [],
                "player_scripts": {"blue": str(blue_script)},
            }

            result = run_one(args, scenario, 1)
            stdout_tail = "\n".join(result["stdoutSummary"]["tail"])

            self.assertFalse(result["timedOut"])
            self.assertFalse(result["idleTimedOut"])
            self.assertEqual(result["outcome"]["result"], "unknown")
            self.assertIn("VCMI_SCRIPTED_ADVENTURE_SCRIPT=ai/global.lua", stdout_tail)
            self.assertIn(f"VCMI_SCRIPTED_ADVENTURE_RED_SCRIPT=file:{red_script.resolve()}", stdout_tail)
            self.assertIn(f"VCMI_SCRIPTED_ADVENTURE_BLUE_SCRIPT=file:{blue_script.resolve()}", stdout_tail)
            self.assertIn("VCMI_SCRIPTED_ADVENTURE_PLAYER_1_SCRIPT=ai/player-one.lua", stdout_tail)
            self.assertEqual(result["playerScripts"]["VCMI_SCRIPTED_ADVENTURE_PLAYER_1_SCRIPT"], "ai/player-one.lua")

    def test_infrastructure_failure_can_retry_to_terminal_result(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            client = root / "fake-client.py"
            client.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import pathlib
                    import time

                    state = pathlib.Path(__file__).with_name("attempt.txt")
                    attempt = int(state.read_text()) + 1 if state.exists() else 1
                    state.write_text(str(attempt))

                    if attempt == 1:
                        print("Creating battle AI BattleAI", flush=True)
                        time.sleep(10)
                    else:
                        print("Red player won. Ending game.", flush=True)
                    """
                ),
                encoding="utf-8",
            )
            client.chmod(0o755)

            args = argparse.Namespace(
                client=str(client),
                ai=["ScriptedAdventureAI", "Nullkiller2"],
                runs=1,
                testdays=0,
                timeout=10,
                idle_timeout=0.2,
                infrastructure_retries=1,
                exit_grace_after_outcome=0.0,
                output=root / "runs",
                cwd=None,
                clean=True,
                extra_arg=[],
                script=None,
                player_script=[],
                red_script=None,
                blue_script=None,
                trace=False,
                json=False,
            )
            scenario = {
                "name": "retry-infra",
                "group": "training",
                "stage": "outcome",
                "kind": "unit",
                "map": "unit-test-map",
                "runs": 1,
                "testdays": 0,
                "timeout": 10,
                "idle_timeout": 0.2,
                "infrastructure_retries": 1,
                "extra_arg": [],
                "enabled": True,
                "tags": [],
            }

            result = run_one_with_infrastructure_retries(args, scenario, 1)
            compact = compact_result(result)

            self.assertEqual(result["outcome"]["result"], "red_win")
            self.assertEqual(result["attempt"], 2)
            self.assertEqual(result["infrastructureRetriesUsed"], 1)
            self.assertFalse(result["infrastructureFailure"])
            self.assertEqual(len(result["previousAttempts"]), 1)
            self.assertTrue(result["previousAttempts"][0]["infrastructureFailure"])
            self.assertEqual(result["previousAttempts"][0]["infrastructureFailureReason"], "battle_ai_creation")
            self.assertEqual(compact["outcome"], "red_win")
            self.assertEqual(compact["infrastructureRetriesUsed"], 1)

    def test_inflight_imperative_command_is_infrastructure_failure(self) -> None:
        result = {
            "outcome": {"result": "idle_timeout"},
            "stdoutSummary": {"tailSignature": "turn_start"},
            "traceSummary": {
                "mistakes": {
                    "items": [
                        {
                            "type": "inflight_imperative_command",
                            "details": {"actionType": "nullkiller_turn_slice"},
                        }
                    ]
                }
            },
        }

        annotate_infrastructure_failure(result)

        self.assertTrue(result["infrastructureFailure"])
        self.assertEqual(
            result["infrastructureFailureReason"],
            "inflight_imperative_command:nullkiller_turn_slice",
        )

    def test_inflight_imperative_trace_can_retry_to_terminal_result(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            client = root / "fake-client.py"
            client.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import json
                    import os
                    import pathlib
                    import time

                    state = pathlib.Path(__file__).with_name("attempt.txt")
                    attempt = int(state.read_text()) + 1 if state.exists() else 1
                    state.write_text(str(attempt))

                    if attempt == 1:
                        print("turn started", flush=True)
                        trace_dir = pathlib.Path(os.environ["XDG_CACHE_HOME"]) / "vcmi" / "scriptedAdventureAI"
                        trace_dir.mkdir(parents=True, exist_ok=True)
                        (trace_dir / "player-red-day-1-event-0-imperative-command-start.json").write_text(json.dumps({
                            "label": "imperative-command-start",
                            "player": "red",
                            "script": "ai/candidates/boundedNullkillerControl.lua",
                            "payload": {
                                "commandIndex": 0,
                                "command": {
                                    "kind": "execute",
                                    "payload": {"type": "nullkiller_turn_slice", "type_id": 106}
                                }
                            }
                        }))
                        time.sleep(10)
                    else:
                        print("Red player won. Ending game.", flush=True)
                    """
                ),
                encoding="utf-8",
            )
            client.chmod(0o755)

            args = argparse.Namespace(
                client=str(client),
                ai=["ScriptedAdventureAI", "Nullkiller2"],
                runs=1,
                testdays=0,
                timeout=10,
                idle_timeout=0.2,
                infrastructure_retries=1,
                exit_grace_after_outcome=0.0,
                output=root / "runs",
                cwd=None,
                clean=True,
                extra_arg=[],
                script=None,
                player_script=[],
                red_script=None,
                blue_script=None,
                trace=True,
                json=False,
            )
            scenario = {
                "name": "retry-inflight-infra",
                "group": "training",
                "stage": "outcome",
                "kind": "unit",
                "map": "unit-test-map",
                "runs": 1,
                "testdays": 0,
                "timeout": 10,
                "idle_timeout": 0.2,
                "infrastructure_retries": 1,
                "extra_arg": [],
                "enabled": True,
                "tags": [],
            }

            result = run_one_with_infrastructure_retries(args, scenario, 1)

        self.assertEqual(result["outcome"]["result"], "red_win")
        self.assertEqual(result["attempt"], 2)
        self.assertEqual(result["infrastructureRetriesUsed"], 1)
        self.assertEqual(len(result["previousAttempts"]), 1)
        self.assertTrue(result["previousAttempts"][0]["infrastructureFailure"])
        self.assertEqual(
            result["previousAttempts"][0]["infrastructureFailureReason"],
            "inflight_imperative_command:nullkiller_turn_slice",
        )


if __name__ == "__main__":
    unittest.main()
