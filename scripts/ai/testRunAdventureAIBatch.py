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

from runAdventureAIBatch import run_one


class RunAdventureAIBatchTest(unittest.TestCase):
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
                exit_grace_after_outcome=10.0,
                output=root / "runs",
                cwd=None,
                clean=True,
                extra_arg=[],
                script=None,
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


if __name__ == "__main__":
    unittest.main()
