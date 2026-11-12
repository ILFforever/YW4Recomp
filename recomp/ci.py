"""Compile everything that needs no game files: the runtime, musl's math and our own shaders.

    python -m recomp.ci [--work DIR] [--jobs N] [--no-fsr] [--no-dlss]

For GitHub's build check (.github/workflows/build.yml) and for a quick look before a push: does our own code
still compile on this system? It is the compile half of recomp.build with the same flags (taken from there, so
the two can't drift apart), without the game's translated code, so nothing is linked and nothing runs. No
config.local.toml is needed. The compiler is $CC, else [tools] cc when there is a config, else gcc.

What it can't tell: whether the translated game compiles, links or plays. That needs a player's own files, which
are never on GitHub (rule 1).

SDL3's headers: `pkg-config sdl3` off Windows when it finds them, else the pinned Windows package's (headers
only; they are the same on every system). On Windows the AMD FSR, NVIDIA DLSS and Streamline headers are
fetched as recomp.build fetches them, so the files that use them are compiled too.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

from tools.yw4 import deps

from . import build, check


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=Path, help="where downloads and objects go (default: a temporary folder)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-fsr", action="store_true", help="without AMD FSR's headers (Windows)")
    ap.add_argument("--no-dlss", action="store_true", help="without NVIDIA DLSS's and Streamline's headers (Windows)")
    args = ap.parse_args(argv)
    with tempfile.TemporaryDirectory() as tmp:
        work = (args.work or Path(tmp)).resolve()
        work.mkdir(parents=True, exist_ok=True)
        # what the build's helpers read from a config: only where generated files go, and which glslang
        cfg = SimpleNamespace(work_dir=work, glslang=None)
        cc = check.host_cc()
        includes = [f"-I{build.RUNTIME / 'include'}", f"-I{check.REPO / 'third_party' / 'vulkan' / 'include'}",
                    f"-I{check.REPO / 'third_party' / 'stb'}"]
        sdl: list[str] = []
        if sys.platform != "win32":
            try:
                sdl = subprocess.run(["pkg-config", "--cflags", "sdl3"], capture_output=True, text=True,
                                     check=True).stdout.split()
            except (subprocess.CalledProcessError, FileNotFoundError):
                pass
        includes += sdl or [f"-I{deps.fetch_sdl(cfg) / 'include'}"]
        if sys.platform == "win32":
            if not args.no_fsr:
                includes += [f"-I{deps.fetch_fsr(cfg) / 'Kits' / 'FidelityFX' / 'api' / 'include'}", "-DYW4_FSR=1"]
            if not args.no_dlss:
                includes += [f"-I{deps.fetch_dlss(cfg) / 'include'}", "-DYW4_DLSS=1",
                             f"-I{deps.fetch_streamline(cfg) / 'include'}", "-DYW4_STREAMLINE=1"]
        srcs = (sorted((build.RUNTIME / "src").glob("*.c")) + sorted((build.RUNTIME / "src").glob("*.cpp"))
                + build.musl_sources() + [build.version_source(cfg), build.dlc_source(cfg)])
        objdir = work / "ci_obj"
        objdir.mkdir(exist_ok=True)
        version = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[:1]
        print(f"compiling {len(srcs)} files with {' '.join(version) or cc} ({args.jobs} jobs)", flush=True)
        t0 = time.time()
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            # musl and the runtime both have a log.c: the folder's name keeps their objects apart
            errors = [e for e in pool.map(
                lambda s: build.compile_one(cc, s, objdir / f"{s.parent.name}_{s.stem}.o", includes), srcs) if e]
        if errors:
            print("\n".join(errors), file=sys.stderr)
            print(f"{len(errors)} of {len(srcs)} file(s) failed", file=sys.stderr)
            return 1
        print(f"{len(srcs)} files compiled in {time.time() - t0:.0f} s")
        from . import shader
        if not shader.glslang(cfg):
            print("no glslang (glslangValidator on PATH): our shaders not compiled")
        elif not build.native_shaders(cfg):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
