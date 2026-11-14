"""Linux: are the build's system tools there, and which compiler builds (decision 0071).

    python -m tools.yw4.system

On Windows the launcher downloads the compiler and ships its own tools (decision 0052). On Linux the build uses the
distribution's own packages, as README.md "Building on Linux" lists them: a C compiler with #embed (Clang 19+ or
GCC 15+), glslang, pkg-config with SDL3, and Python 3.11+ (this script's own). check() finds them and raises one
error naming everything missing, with the install line for the common distributions, instead of a build that stops
halfway with a compiler error.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from . import config

# the oldest major version with #embed (runtime/src/ui.c and others embed fonts and images)
MIN_CLANG = 19
MIN_GCC = 15

INSTALL = (
    "  Arch:            sudo pacman -S clang python pkgconf glslang sdl3\n"
    "  Fedora:          sudo dnf install clang python3 pkgconf glslang SDL3-devel\n"
    "  Debian 13, Ubuntu 25.04+:  sudo apt install clang python3 pkg-config glslang-tools libsdl3-dev\n"
    "  (Ubuntu 24.04 has no SDL3 package: see docs/setup.md)"
)


def version_of(cc: str) -> tuple[str, int] | None:
    """("clang" | "gcc", major version) from `<cc> --version`, or None if it doesn't run."""
    try:
        out = subprocess.run([cc, "--version"], capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    first = out.splitlines()[0] if out else ""
    m = re.search(r"clang version (\d+)", first)
    if m:
        return "clang", int(m.group(1))
    m = re.search(r"\(GCC\)\s+(\d+)|^gcc\S*\s.*?\s(\d+)\.\d+", first)
    if m:
        return "gcc", int(m.group(1) or m.group(2))
    return None


def good(cc: str) -> bool:
    v = version_of(cc)
    return bool(v) and v[1] >= (MIN_CLANG if v[0] == "clang" else MIN_GCC)


def find_compiler(configured: str = "") -> str | None:
    """The configured compiler if it is new enough, else the newest clang-NN / clang / gcc-NN / gcc on the PATH."""
    if configured:
        return configured if good(configured) else None
    names = [f"clang-{n}" for n in range(30, MIN_CLANG - 1, -1)] + ["clang"]
    names += [f"gcc-{n}" for n in range(30, MIN_GCC - 1, -1)] + ["gcc"]
    for name in names:
        path = shutil.which(name)
        if path and good(path):
            return path
    return None


def check(cfg: config.Config) -> str:
    """The compiler to build with; raises RuntimeError naming every missing tool and how to install them."""
    missing = []
    cc = find_compiler(cfg.cc)
    if not cc:
        missing.append(f"a C compiler: Clang {MIN_CLANG} or newer (or GCC {MIN_GCC} or newer)"
                       + (f"; [tools] cc = {cfg.cc} is older or doesn't run" if cfg.cc else ""))
    if not (cfg.glslang and cfg.glslang.is_file()) and not (shutil.which("glslangValidator") or shutil.which("glslang")):
        missing.append("glslang (glslangValidator)")
    if not shutil.which("pkg-config"):
        missing.append("pkg-config")
    elif subprocess.run(["pkg-config", "--exists", "sdl3"]).returncode:
        # the launcher's own SDL3 (YW4_SDL3_DIR, its builder/sdl3: decision 0071), only when the system has none;
        # every later step inherits the path, and recomp/build.py puts the library next to yw4
        own = os.environ.get("YW4_SDL3_DIR", "")
        pc = Path(own) / "lib" / "pkgconfig"
        if own and (pc / "sdl3.pc").is_file():
            os.environ["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, [str(pc), os.environ.get("PKG_CONFIG_PATH")]))
        if subprocess.run(["pkg-config", "--exists", "sdl3"]).returncode:
            missing.append("SDL3 with its headers (pkg-config sdl3)")
    if missing:
        raise RuntimeError("the build needs these, not found on this PC:\n" + "".join(f"  - {m}\n" for m in missing)
                           + "Install them with your package manager, then build again:\n" + INSTALL)
    return cc


def main() -> int:
    if sys.platform == "win32":
        print("Windows: the launcher brings its own compiler and tools")
        return 0
    try:
        cc = check(config.load(require_exefs=False))
    except (RuntimeError, config.ConfigError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"compiler: {cc} ({' '.join(map(str, version_of(cc) or ()))}); glslang, pkg-config and SDL3 found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
