#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from auditAdventureCallbackSurface import audit  # noqa: E402


REPO_ROOT = SCRIPT_DIR.parents[1]


class AdventureCallbackSurfaceAuditTest(unittest.TestCase):
    def test_scripted_adventure_callbacks_are_lua_visible_or_explained(self) -> None:
        result = audit(REPO_ROOT)

        self.assertEqual(result["missingOverrides"], [])
        self.assertEqual(result["missingImplementations"], [])
        self.assertEqual(result["missingScriptExposure"], [])
        self.assertEqual(result["missingBaseForward"], [])
        self.assertEqual(result["staleMissingOverrideExclusions"], [])
        self.assertEqual(result["staleNoScriptExposureExclusions"], [])
        self.assertEqual(result["staleNoBaseForwardExclusions"], [])
        self.assertGreaterEqual(result["callbackMethodCount"], 60)
        self.assertGreaterEqual(result["scriptedOverrideCount"], 55)
        self.assertGreaterEqual(result["scriptExposedCallbackCount"], 50)


if __name__ == "__main__":
    unittest.main()
