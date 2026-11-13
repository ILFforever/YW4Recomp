"""Keep the version in README.md in step with the VERSION file.

The README shows the game version twice: the shields.io badge at the top and the "X is out" banner under
Status. Both used to be edited by hand and went stale after a release. This script rewrites both from VERSION.

    python -m tools.dev.readme_version          # rewrite README.md
    python -m tools.dev.readme_version --check  # exit 1 if README.md is out of date (no writes)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (pattern, replacement template). Each pattern must match exactly once, so a reworded README fails loudly
# instead of silently going stale again.
RULES = [
    (re.compile(r'alt="Version: [0-9][0-9.]*" src="https://img\.shields\.io/badge/version-[0-9][0-9.]*-blue"'),
     'alt="Version: {v}" src="https://img.shields.io/badge/version-{v}-blue"'),
    (re.compile(r'^> \*\*[0-9][0-9.]* is out \(beta\):\*\*', re.M),
     '> **{v} is out (beta):**'),
]


def sync(text: str, version: str) -> str:
    for pattern, template in RULES:
        text, n = pattern.subn(template.format(v=version), text)
        if n != 1:
            raise SystemExit(f"readme_version: expected 1 match for {pattern.pattern!r}, found {n}")
    return text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="only report, exit 1 if README.md is out of date")
    args = ap.parse_args()

    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    readme = ROOT / "README.md"
    old = readme.read_bytes().decode("utf-8")  # bytes: keep the file's own line endings
    new = sync(old, version)
    if new == old:
        print(f"README.md already says {version}")
        return 0
    if args.check:
        print(f"README.md is out of date: VERSION is {version}. Run: python -m tools.dev.readme_version")
        return 1
    readme.write_bytes(new.encode("utf-8"))
    print(f"README.md now says {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
