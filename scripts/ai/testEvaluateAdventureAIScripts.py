#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from evaluateAdventureAIScripts import promotion_verdict, run_metrics  # noqa: E402


def result(
    *,
    timed_out: bool = False,
    idle_timed_out: bool = False,
    return_code: int = 0,
    infrastructure: bool = False,
    outcome: str = "day_limit",
) -> dict:
    return {
        "timedOut": timed_out,
        "idleTimedOut": idle_timed_out,
        "returnCode": return_code,
        "infrastructureFailure": infrastructure,
        "elapsedSeconds": 1.0,
        "outcome": {"result": outcome},
    }


def empty_summary() -> dict:
    return {
        "parse_errors": [],
        "output_statuses": {},
        "progress": {},
        "executed_actions": {},
        "quality": {},
        "mapProgress": {},
        "mistakes": {},
    }


def promotion_args() -> argparse.Namespace:
    return argparse.Namespace(
        allow_more_failed_actions=0,
        allow_more_important_mistakes=0,
        allow_more_infrastructure_failures=0,
        min_score_delta=1,
        min_quality_delta=0,
        min_training_score_delta=1,
        min_training_quality_delta=0,
        allow_heldout_score_regression=0,
        allow_heldout_quality_regression=0,
        allow_heldout_important_mistake_regression=0,
    )


class EvaluateAdventureAIScriptsTest(unittest.TestCase):
    def test_metrics_separate_infrastructure_idle_from_script_idle(self) -> None:
        metrics = run_metrics(
            [
                result(),
                result(idle_timed_out=True, return_code=-15, infrastructure=True, outcome="idle_timeout"),
                result(idle_timed_out=True, return_code=-15, infrastructure=False, outcome="idle_timeout"),
            ],
            empty_summary(),
        )

        self.assertEqual(metrics["runs"], 3)
        self.assertEqual(metrics["completed"], 1)
        self.assertEqual(metrics["scriptCompleted"], 1)
        self.assertEqual(metrics["nonInfrastructureRuns"], 2)
        self.assertEqual(metrics["idleTimeouts"], 2)
        self.assertEqual(metrics["scriptIdleTimeouts"], 1)
        self.assertEqual(metrics["infrastructureFailures"], 1)

    def test_promotion_allows_equal_infrastructure_failures_but_rejects_script_idle(self) -> None:
        args = promotion_args()
        baseline = run_metrics(
            [
                result(),
                result(idle_timed_out=True, return_code=-15, infrastructure=True, outcome="idle_timeout"),
            ],
            empty_summary(),
        )
        candidate = run_metrics(
            [
                result(),
                result(idle_timed_out=True, return_code=-15, infrastructure=True, outcome="idle_timeout"),
            ],
            empty_summary(),
        )

        verdict = promotion_verdict(args, baseline, candidate, {})
        self.assertNotIn("candidateHasNoScriptIdleTimeouts", verdict["reasons"])
        self.assertNotIn("candidateInfrastructureFailuresAllowed", verdict["reasons"])

        candidate_with_script_idle = run_metrics(
            [
                result(),
                result(idle_timed_out=True, return_code=-15, infrastructure=False, outcome="idle_timeout"),
            ],
            empty_summary(),
        )

        rejected = promotion_verdict(args, baseline, candidate_with_script_idle, {})
        self.assertIn("candidateHasNoScriptIdleTimeouts", rejected["reasons"])

    def test_promotion_rejects_extra_infrastructure_failures_by_default(self) -> None:
        args = promotion_args()
        baseline = run_metrics([result()], empty_summary())
        candidate = run_metrics(
            [
                result(),
                result(idle_timed_out=True, return_code=-15, infrastructure=True, outcome="idle_timeout"),
            ],
            empty_summary(),
        )

        verdict = promotion_verdict(args, baseline, candidate, {})

        self.assertIn("candidateInfrastructureFailuresAllowed", verdict["reasons"])


if __name__ == "__main__":
    unittest.main()
