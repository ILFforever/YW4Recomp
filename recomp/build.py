"""Build the recompiled game: work/recomp/*.c + runtime/src/*.c + musl's math -> work/build/yw4.exe.

    python -m recomp.build           compile what changed, link
    python -m recomp.build --run     ... then run it on work/exefs/ (main.bin, sdk.bin)

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
import tomllib
from pathlib import Path

import shutil

from tools.yw4 import config, deps

from . import check

RUNTIME = check.REPO / "runtime"
CFLAGS = ["-std=c11", "-O1", "-ffp-contract=off", "-Wall", "-Wno-unused-label"]

# musl 1.1.19's math (third_party/musl, decision 0013). These are the flags its results were
# verified with against the sdk (tools/yw4/sdkref.py --check uses this same list): no FMA
# contraction, no builtins (the host must not constant-fold with its own libm).
MUSL = check.REPO / "third_party" / "musl"
MUSL_CFLAGS = ["-std=c99", "-O2", "-ffp-contract=off", "-fno-builtin", "-frounding-math",
               "-include", str(MUSL / "yw4" / "glue.h"), f"-I{MUSL / 'yw4'}", f"-I{MUSL / 'src' / 'internal'}"]


def musl_sources() -> list[Path]:
    return sorted((MUSL / "src" / "math").glob("*.c"))


def sources(cfg: config.Config) -> list[Path]:
    gen = cfg.work_dir / "recomp"
    return sorted(gen.glob("*.c")) + sorted((RUNTIME / "src").glob("*.c")) + musl_sources()


def newest_header(cfg: config.Config) -> float:
    headers = list((RUNTIME / "include").glob("*.h")) + list((cfg.work_dir / "recomp").glob("*.h"))
    return max(h.stat().st_mtime for h in headers)


# What generated code includes: funcs.h -> yw4rt.h -> yw4cpu.h -> yw4fp.h, and yw4load.h for
# image_info.c only. Other runtime headers changing doesn't rebuild the ~150 generated files.
GENERATED_HEADERS = ("yw4rt.h", "yw4cpu.h", "yw4fp.h")


def generated_header(cfg: config.Config, src: Path) -> float:
    names = GENERATED_HEADERS + (("yw4load.h",) if src.name == "image_info.c" else ())
    headers = [RUNTIME / "include" / n for n in names] + list((cfg.work_dir / "recomp").glob("*.h"))
    return max(h.stat().st_mtime for h in headers)


def compile_one(cc: str, src: Path, obj: Path, includes: list[str]) -> str | None:
    flags = MUSL_CFLAGS if MUSL in src.parents else [*CFLAGS, *includes]
    proc = subprocess.run([cc, *flags, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    if proc.returncode != 0:
        return f"{src.name}:\n{proc.stderr[-2000:]}"
    if proc.stderr.strip():
        print(proc.stderr[-1000:], file=sys.stderr)
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="store_true", help="run the result on work/exefs/")
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
    # The window (decision 0019): SDL3 (fetched on Windows, the system's elsewhere), Vulkan headers
    includes.append(f"-I{check.REPO / 'third_party' / 'vulkan' / 'include'}")
    if sys.platform == "win32":
        sdl = deps.fetch_sdl(cfg)
        includes.append(f"-I{sdl / 'include'}")
        sdl_libs = [str(sdl / "lib" / "libSDL3.dll.a")]
    else:
        pc = lambda *a: subprocess.run(["pkg-config", *a, "sdl3"], capture_output=True, text=True, check=True).stdout.split()
        includes += pc("--cflags")
        sdl_libs = pc("--libs")
    hdr = newest_header(cfg)
    # runtime/src/*.h are private to the runtime: changing one doesn't rebuild generated code
    rt_hdr = max([hdr, *(h.stat().st_mtime for h in (RUNTIME / "src").glob("*.h"))])
    musl_hdr = max(h.stat().st_mtime for h in MUSL.rglob("*.h"))
    todo, objs = [], []
    for src in sources(cfg):
        rt = src.parent == RUNTIME / "src"
        musl = MUSL in src.parents
        obj = objdir / (("rt_" if rt else "musl_" if musl else "") + src.stem + ".o")
        objs.append(obj)
        dep = musl_hdr if musl else rt_hdr if rt else generated_header(cfg, src)
        if not obj.exists() or obj.stat().st_mtime < max(src.stat().st_mtime, dep):
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
    libs = [*sdl_libs, "-pthread"] + (["-lwinmm"] if sys.platform == "win32" else [])  # winmm: timeBeginPeriod
    if sys.platform == "win32":
        shutil.copy2(sdl / "bin" / "SDL3.dll", exe.parent / "SDL3.dll")  # found next to yw4.exe
    link = subprocess.run([cc, "-o", str(exe), *map(str, objs), *libs], capture_output=True, text=True)
    if link.returncode != 0:
        print(f"link failed:\n{link.stderr[-3000:]}", file=sys.stderr)
        return 1
    print(f"built {exe} in {time.time() - t0:.0f} s")
    if args.run:
        save = cfg.work_dir / "save"  # the game's save data (the owner's: never commit)
        save.mkdir(exist_ok=True)
        shots = cfg.work_dir / "screenshots"  # F12 / YW4_SCREENSHOTS (images of the game: never commit)
        shots.mkdir(exist_ok=True)
        env = {**os.environ, "YW4_SCREENSHOT_DIR": os.environ.get("YW4_SCREENSHOT_DIR", str(shots))}
        app_id = int(tomllib.loads((check.REPO / "target.toml").read_text())["title"]["application_id"], 16)
        aoc = cfg.dlc_indices(app_id)
        if aoc:  # add-on content the owner has (svc_misc.c aoc:u)
            env.setdefault("YW4_AOC", ",".join(map(str, aoc)))
            print(f"add-on content: {env['YW4_AOC']}")
        return subprocess.run([str(exe), str(cfg.work_dir / "exefs"), str(cfg.romfs_dir), str(save)], env=env).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
