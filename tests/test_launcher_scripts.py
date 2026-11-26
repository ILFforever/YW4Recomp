"""The launcher's screen scripts parse.

Launcher 1.7.0 shipped with a ' inside a '...' string in app.js's What's new list ("the game's overlay"): the
WebView refused the whole file, so no button worked (not even the title bar's X) and the saved install never loaded
(the Welcome screen every time). node --check catches that; skipped where node isn't installed.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

UI = Path(__file__).resolve().parents[1] / "launcher" / "ui"


@pytest.mark.skipif(not shutil.which("node"), reason="node not installed")
@pytest.mark.parametrize("name", sorted(p.name for p in UI.glob("*.js")))
def test_script_parses(name):
    r = subprocess.run(["node", "--check", str(UI / name)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
