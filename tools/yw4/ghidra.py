"""Run Ghidra headless on main and export function boundaries.

    python -m tools.yw4.ghidra            import + analyze (first run: long), then export
    python -m tools.yw4.ghidra --export   re-run only the export on the existing project

Needs [tools] ghidra_dir (with the Switch loader extension installed) and a
verified ingest. The Ghidra project goes in <work_dir>/ghidra/ because its
database contains the game's code (gitignored, CLAUDE.md rule 1).
Output: <work_dir>/ghidra/main.functions.json (see tools/ghidra/ExportFunctions.java).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys

from . import config, ingest, nso

SCRIPT_DIR = config.REPO_ROOT / "tools" / "ghidra"
PROJECT_NAME = "yw4"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--export", action="store_true", help="skip import/analysis, only export")
    args = ap.parse_args(argv)

    try:
        cfg = config.load()
        if cfg.ghidra_dir is None:
            raise config.ConfigError("[tools] ghidra_dir is not set")
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    headless = cfg.ghidra_dir / "support" / ("analyzeHeadless.bat" if sys.platform == "win32" else "analyzeHeadless")
    if not headless.is_file():
        print(f"analyzeHeadless not found under {cfg.ghidra_dir}", file=sys.stderr)
        return 1

    # Same version lock as ingest: never analyze a different build.
    main_nso = cfg.exefs_dir / "main"
    want = ingest.load_target().get("image_sha256", "")
    got = hashlib.sha256(nso.flat_image(main_nso.read_bytes())).hexdigest()
    if not want or got != want:
        print("main does not match target.toml; run python -m tools.yw4.ingest first", file=sys.stderr)
        return 1

    proj = cfg.work_dir / "ghidra"
    proj.mkdir(parents=True, exist_ok=True)
    out_json = proj / "main.functions.json"

    cmd = [str(headless), str(proj), PROJECT_NAME]
    if args.export:
        cmd += ["-process", "main", "-noanalysis"]
    else:
        cmd += ["-import", str(main_nso), "-overwrite"]
    cmd += ["-scriptPath", str(SCRIPT_DIR), "-postScript", "ExportFunctions.java", str(out_json)]

    print(" ".join(f'"{c}"' if " " in c else c for c in cmd))
    log = proj / ("export.log" if args.export else "analyze.log")
    # Ghidra's default 2 GB heap is too small for a 17 MB .text; allow an override.
    env = dict(os.environ)
    env.setdefault("GHIDRA_HEADLESS_MAXMEM", "12G")
    with log.open("w", encoding="utf-8") as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env).returncode
    print(f"analyzeHeadless exit {rc}; log: {log}")
    if rc != 0 or not out_json.is_file():
        return 1
    print(f"wrote {out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
