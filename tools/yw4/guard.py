"""Refuse to commit anything that looks like game data (CLAUDE.md rule 1).

    python -m tools.yw4.guard            check the files staged for commit
    python -m tools.yw4.guard --install  install it as .git/hooks/pre-commit

A second line of defence behind .gitignore: it reads every staged file and
blocks the commit if one starts with a Switch / Level-5 / CRI container
signature, contains a NSO's MOD0 module header, or is suspiciously large.
False positives can be committed with `git commit --no-verify` after checking.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAX_BYTES = 2 * 1024 * 1024

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


def problem(path: str, data: bytes) -> str | None:
    if len(data) > MAX_BYTES:
        return f"{len(data) / 1e6:.1f} MB (limit {MAX_BYTES / 1e6:.0f} MB)"
    kind = MAGICS.get(data[:4])
    if kind:
        return f"starts like a {kind}"
    if b"MOD0" in data[:0x100] and not path.endswith((".py", ".md", ".c", ".h", ".S", ".txt")):
        return "contains a Switch module header (MOD0)"
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", action="store_true")
    args = ap.parse_args(argv)
    if args.install:
        hook = REPO / ".git" / "hooks" / "pre-commit"
        hook.write_text(HOOK, newline="\n")
        hook.chmod(0o755)
        print(f"installed {hook}")
        return 0
    bad = [(p, why) for p in staged_files() if (why := problem(p, staged_blob(p)))]
    for p, why in bad:
        print(f"refusing to commit {p}: {why}", file=sys.stderr)
    if bad:
        print("game data must never be committed (CLAUDE.md rule 1); "
              "if this is a false positive, check it and use --no-verify", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
