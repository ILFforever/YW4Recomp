"""Build the recompiled game: work/recomp/*.c + runtime/src/*.c -> work/build/yw4.exe.

    python -m recomp.build           compile what changed, link
    python -m recomp.build --run     ... then run it on work/exefs/main.bin

Uses [tools] cc (Clang recommended, decision 0007) with -O1 for now. Objects go to
work/build/obj/; a source is recompiled when it, or any header, is newer than its object.
The executable is built from the game's code, so it stays in work/ (never commit or share).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
from pathlib import Path

from tools.yw4 import config

from . import check

RUNTIME = check.REPO / "runtime"
CFLAGS = ["-std=c11", "-O1", "-ffp-contract=off", "-Wall", "-Wno-unused-label"]


def sources(cfg: config.Config) -> list[Path]:
    gen = cfg.work_dir / "recomp"
    return sorted(gen.glob("*.c")) + sorted((RUNTIME / "src").glob("*.c"))


def newest_header(cfg: config.Config) -> float:
    headers = list((RUNTIME / "include").glob("*.h")) + list((cfg.work_dir / "recomp").glob("*.h"))
    return max(h.stat().st_mtime for h in headers)


def compile_one(cc: str, src: Path, obj: Path, includes: list[str]) -> str | None:
    proc = subprocess.run([cc, *CFLAGS, *includes, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    if proc.returncode != 0:
        return f"{src.name}:\n{proc.stderr[-2000:]}"
    if proc.stderr.strip():
        print(proc.stderr[-1000:], file=sys.stderr)
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="store_true", help="run the result on work/exefs/main.bin")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args(argv)
    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    if not (cfg.work_dir / "recomp" / "image_info.c").is_file():
        print("no generated code; run python -m recomp.translate first", file=sys.stderr)
        return 1

    cc = check.host_cc()
    objdir = cfg.work_dir / "build" / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    includes = [f"-I{RUNTIME / 'include'}", f"-I{cfg.work_dir / 'recomp'}"]
    hdr = newest_header(cfg)
    todo, objs = [], []
    for src in sources(cfg):
        obj = objdir / (("rt_" if src.parent.name == "src" else "") + src.stem + ".o")
        objs.append(obj)
        if not obj.exists() or obj.stat().st_mtime < max(src.stat().st_mtime, hdr):
            todo.append((src, obj))

    t0 = time.time()
    print(f"compiling {len(todo)} of {len(objs)} files with {Path(cc).name} ({args.jobs} jobs)")
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        errors = [e for e in pool.map(lambda so: compile_one(cc, so[0], so[1], includes), todo) if e]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print(f"{len(errors)} file(s) failed", file=sys.stderr)
        return 1
    exe = cfg.work_dir / "build" / "yw4.exe"
    link = subprocess.run([cc, "-o", str(exe), *map(str, objs)], capture_output=True, text=True)
    if link.returncode != 0:
        print(f"link failed:\n{link.stderr[-3000:]}", file=sys.stderr)
        return 1
    print(f"built {exe} in {time.time() - t0:.0f} s")
    if args.run:
        return subprocess.run([str(exe), str(cfg.work_dir / "exefs" / "main.bin")]).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
