"""The launcher's screens take the system from the Rust side, not the user agent (issue #22).

On a Steam Deck the 1.6.0 launcher's user agent had no "Linux" in it, so the screens took themselves for Windows:
folders were named "...\\YW4 Recomp" and the Linux folder fix never showed. A real Deck can't be tested here, so
these check the code keeps the Rust-set value first.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_JS = (ROOT / "launcher" / "ui" / "app.js").read_text(encoding="utf-8")
MAIN_RS = (ROOT / "launcher" / "src-tauri" / "src" / "main.rs").read_text(encoding="utf-8")


def test_rust_sets_the_system_before_the_screens_run():
    assert "js_init_script" in MAIN_RS
    assert re.search(r"window\.__YW4_OS = '\{\}'.*screens_os\(\)", MAIN_RS)
    # the built-for system is the default; YW4_LAUNCHER_OS only overrides it
    assert re.search(r"fn screens_os\(\).*YW4_LAUNCHER_OS.*_ => std::env::consts::OS", MAIN_RS, re.S)


def test_linux_comes_from_rust_first():
    m = re.search(r"const LINUX = (.+?);\n", APP_JS, re.S)
    assert m, "app.js has no LINUX"
    first = m.group(1).strip()
    assert first.startswith("window.__YW4_OS ? window.__YW4_OS === 'linux'"), first


def test_slash_fix_not_gated_by_the_guess():
    m = re.search(r"async function fixSlashFolder\(install\) \{\n(.+?)\n", APP_JS)
    assert m and "LINUX" not in m.group(1), m and m.group(1)
