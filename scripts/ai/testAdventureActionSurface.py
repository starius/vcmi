#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from auditAdventureActionSurface import audit  # noqa: E402


REPO_ROOT = SCRIPT_DIR.parents[1]


class AdventureActionSurfaceAuditTest(unittest.TestCase):
    def test_scripted_adventure_actions_cover_strategic_server_packets(self) -> None:
        result = audit(REPO_ROOT)

        self.assertEqual(result["missingPackClassifications"], [])
        self.assertEqual(result["stalePackClassifications"], [])
        self.assertEqual(result["missingRegisteredActions"], {})
        self.assertGreaterEqual(result["coveredPackCount"], 30)
        self.assertGreaterEqual(result["registeredActionCount"], 50)


if __name__ == "__main__":
    unittest.main()
