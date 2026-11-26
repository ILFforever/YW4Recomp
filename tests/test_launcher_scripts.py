"""The launcher's screen scripts parse.

Launcher 1.7.0 shipped with a ' inside a '...' string in app.js's What's new list ("the game's overlay"): the
WebView refused the whole file, so no button worked (not even the title bar's X) and the saved install never loaded
(the Welcome screen every time). node --check catches that; skipped where node isn't installed.
(unittest, not pytest: CI runs python -m unittest without pytest installed.)
"""
import shutil
import subprocess
import unittest
from pathlib import Path

UI = Path(__file__).resolve().parents[1] / "launcher" / "ui"


@unittest.skipUnless(shutil.which("node"), "node not installed")
class LauncherScriptsParse(unittest.TestCase):
    def test_every_script_parses(self):
        scripts = sorted(UI.glob("*.js"))
        self.assertTrue(scripts, "no launcher scripts found")
        for js in scripts:
            with self.subTest(script=js.name):
                r = subprocess.run(["node", "--check", str(js)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
