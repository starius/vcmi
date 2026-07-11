#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from auditAdventureScriptDelegation import audit  # noqa: E402


REPO_ROOT = SCRIPT_DIR.parents[1]


class AdventureScriptDelegationAuditTest(unittest.TestCase):
    def test_only_explicit_control_script_uses_full_day_delegation(self) -> None:
        result = audit(REPO_ROOT)

        self.assertEqual(result["directDelegations"], {})
        self.assertEqual(result["staleFullDelegationAllowlist"], [])
        self.assertGreaterEqual(result["scriptCount"], 5)


if __name__ == "__main__":
    unittest.main()
