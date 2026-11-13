"""Controller test: press buttons and see what SDL3 and the game make of each one (tools/dev/padtest.c).

    python -m tools.dev.padtest

Builds work/build/padtest(.exe) with the game's compiler and SDL3 (rebuilt when padtest.c changes), then runs it
in this console. Use it when a controller's printed letters and the game's button icons disagree: it shows the
type the pad reports, which names the game picks from that, and for each face button its place, the letter SDL
thinks is printed on it, and the Switch button the game presses.
"""

from __future__ import annotations

import subprocess
import sys

from tools.yw4 import config, deps

from recomp import check


def main() -> int:
    cfg = config.load()
    src = check.REPO / "tools" / "dev" / "padtest.c"
    build = cfg.work_dir / "build"
    build.mkdir(parents=True, exist_ok=True)
    exe = build / ("padtest.exe" if sys.platform == "win32" else "padtest")
    if not exe.exists() or exe.stat().st_mtime < src.stat().st_mtime:
        if sys.platform == "win32":
            sdl = deps.fetch_sdl(cfg)  # the game's SDL3; its DLL is already next to yw4.exe in work/build
            flags = [f"-I{sdl / 'include'}", str(sdl / "lib" / "libSDL3.dll.a")]
            dll = build / "SDL3.dll"
            if not dll.exists():
                dll.write_bytes((sdl / "bin" / "SDL3.dll").read_bytes())
        else:
            pc = lambda *a: subprocess.run(["pkg-config", *a, "sdl3"], capture_output=True, text=True,
                                           check=True).stdout.split()
            flags = pc("--cflags") + pc("--libs")
        cc = str(cfg.cc) if getattr(cfg, "cc", None) else "cc"
        cmd = [cc, "-std=c11", "-O1", "-Wall", str(src), *flags, "-o", str(exe)]
        print("building", exe.name)
        if subprocess.run(cmd).returncode:
            return 1
    return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    sys.exit(main())
