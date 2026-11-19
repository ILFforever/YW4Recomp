"""Refuse to commit anything that looks like game data (CLAUDE.md rule 1).

    python -m tools.yw4.guard            check the files staged for commit
    python -m tools.yw4.guard --all      check every tracked file (the build check runs this)
    python -m tools.yw4.guard --install  install it as .git/hooks/pre-commit

A second line of defence behind .gitignore: it reads every staged file and
blocks the commit if one starts with a Switch / Level-5 / CRI container
signature, contains a NSO's MOD0 module header, or is suspiciously large.
It also blocks a real home folder in a text file (C:/Users/<name>/... with either
slash, or your own home folder), which would publish a user name.
False positives can be committed with `git commit --no-verify` after checking.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAX_BYTES = 2 * 1024 * 1024
# Bundled open-licence fonts (third_party/fonts/README.md) may be larger: the overlay's CJK font is 3.7 MB. Only
# the size limit is lifted for them; the signature checks below still run.
LARGE_OK = ("third_party/fonts/",)
LARGE_OK_SUFFIXES = (".ttf", ".otf")

# Signatures at offset 0.
MAGICS = {
    b"NSO0": "NSO executable",
    b"NRO0": "NRO executable",
    b"PFS0": "PFS0 container (NSP/ExeFS)",
    b"HFS0": "HFS0 container (XCI)",
    b"KIP1": "KIP executable",
    b"CPK ": "CRI CPK archive",
    b"@UTF": "CRI table (ACB/ACF)",
    b"AFS2": "CRI AWB audio bank",
    b"CRID": "CRI USM video",
    b"HBH1": "Level-5 shader binary",
    b"G4TX": "Level-5 texture",
    b"G4MG": "Level-5 model",
    b"G4PK": "Level-5 package",
    b"\x7fELF": "ELF binary",
}

# A Windows home folder with a real user name in it. Placeholders and CI runners' names are fine.
WIN_HOME = re.compile(rb"""[A-Za-z]:[\\/]+Users[\\/]+([^\\/\s`'"<>%$*{}()\[\]]+)""", re.I)
HOME_OK = {b"runneradmin", b"runner~1", b"public", b"default", b"user", b"username", b"you", b"name", b"me",
           b"o'brien", b"..."}
# This repo's own guard and its tests name the pattern; vendored code is someone else's.
HOME_SKIP = ("third_party/", "tools/yw4/guard.py", "tests/test_guard.py")

HOOK = """#!/bin/sh
# Installed by `python -m tools.yw4.guard --install` (CLAUDE.md rule 1).
exec python -m tools.yw4.guard
"""


def staged_files() -> list[str]:
    out = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
                         cwd=REPO, capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def staged_blob(path: str) -> bytes:
    return subprocess.run(["git", "show", f":{path}"], cwd=REPO, capture_output=True, check=True).stdout


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def tracked_blob(path: str) -> bytes:
    return (REPO / path).read_bytes()


def own_home() -> bytes | None:
    """This PC's home folder, as it would appear in a file (both slash styles are checked by the caller)."""
    home = os.path.expanduser("~")
    return home.encode("utf-8") if home not in ("~", "/", "") and len(home) > 3 else None


def home_path(path: str, data: bytes) -> str | None:
    if path.startswith(HOME_SKIP) or b"\0" in data[:8192]:  # binary files: images, fonts
        return None
    for m in WIN_HOME.finditer(data):
        if m.group(1).lower() not in HOME_OK:
            return f"names a user's home folder ({m.group(0).decode('utf-8', 'replace')})"
    home = own_home()
    if home and (home in data or home.replace(b"\\", b"/") in data or home.replace(b"\\", b"\\\\") in data):
        return "contains your own home folder path"
    return None


def problem(path: str, data: bytes) -> str | None:
    large_ok = path.startswith(LARGE_OK) and path.endswith(LARGE_OK_SUFFIXES)
    if len(data) > MAX_BYTES and not large_ok:
        return f"{len(data) / 1e6:.1f} MB (limit {MAX_BYTES / 1e6:.0f} MB)"
    kind = MAGICS.get(data[:4])
    if kind:
        return f"starts like a {kind}"
    if b"MOD0" in data[:0x100] and not path.endswith((".py", ".md", ".c", ".h", ".S", ".txt")):
        return "contains a Switch module header (MOD0)"
    return home_path(path, data)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--all", action="store_true", help="check every tracked file, not only the staged ones")
    args = ap.parse_args(argv)
    if args.install:
        hook = REPO / ".git" / "hooks" / "pre-commit"
        hook.write_text(HOOK, newline="\n")
        hook.chmod(0o755)
        print(f"installed {hook}")
        return 0
    files, blob = (tracked_files(), tracked_blob) if args.all else (staged_files(), staged_blob)
    bad = [(p, why) for p in files if (why := problem(p, blob(p)))]
    for p, why in bad:
        print(f"{'tracked' if args.all else 'refusing to commit'} {p}: {why}", file=sys.stderr)
    if bad:
        print("game data and user names must never be committed (CLAUDE.md rule 1); "
              "if this is a false positive, check it and use --no-verify", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
