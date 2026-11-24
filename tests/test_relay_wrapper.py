"""Runs tools/relay/test_relay.py (the Python relay, no C build) as one test."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "tools" / "relay" / "test_relay.py"


class RelayScriptTest(unittest.TestCase):
    def test_relay_script_passes(self):
        try:
            r = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            self.fail("test_relay.py timed out after 120 s")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("all relay tests passed", r.stdout)


if __name__ == "__main__":
    unittest.main()
