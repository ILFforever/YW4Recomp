"""Linux: the build's tools, the system's or our own, and which compiler builds (decisions 0071, 0083).

    python -m tools.yw4.system [--tools DIR]

On Windows the launcher downloads the compiler and ships its own tools (decision 0052). On Linux the build uses the
distribution's own packages where it has them, as README.md "Building on Linux" lists them: a C compiler with #embed
(Clang 19+ or GCC 15+), glslang, SDL3, and Python 3.11+ (this script's own). What the system lacks of the compiler
and glslang is downloaded instead (decision 0083: Zig and Khronos' glslang, tools/yw4/deps.py), so a read-only
system such as Bazzite or SteamOS builds with nothing installed. SDL3 is the system's (pkg-config sdl3) or the
launcher's own copy (YW4_SDL3_DIR), read directly, so pkg-config isn't needed either. check() raises one error
naming whatever is still missing, with the install line for the common distributions, instead of a build that stops
halfway with a compiler error.

YW4_OWN_TOOLS=1 uses the downloaded compiler and glslang even where the system has its own (to test them).
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

from . import config, deps

# the oldest major version with #embed (runtime/src/ui.c and others embed fonts and images)
MIN_CLANG = 19
MIN_GCC = 15

INSTALL = (
    "  Arch:            sudo pacman -S clang python pkgconf glslang sdl3\n"
    "  Fedora:          sudo dnf install clang python3 pkgconf glslang SDL3-devel\n"
    "  Debian 13, Ubuntu 25.04+:  sudo apt install clang python3 pkg-config glslang-tools libsdl3-dev\n"
    "  (Ubuntu 24.04 has no SDL3 package: see docs/setup.md)"
)

# where a desktop's libstdc++.so.6 is (Fedora / Arch, Debian / Ubuntu, others)
LIB_DIRS = ("/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib", "/lib64", "/lib")


def version_of(cc: str) -> tuple[str, int] | None:
    """("clang" | "gcc", major version) from `<cc> --version`, or None if it doesn't run."""
    try:
        out = subprocess.run([cc, "--version"], capture_output=True, text=True, timeout=60).stdout
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


def find_glslang(cfg: config.Config) -> str | None:
    if cfg.glslang and cfg.glslang.is_file():
        return str(cfg.glslang)
    return shutil.which("glslangValidator") or shutil.which("glslang")


def is_own(cc: str) -> bool:
    """Is `cc` our downloaded compiler (deps.fetch_zig's bin/zig-clang)?"""
    return Path(cc).name.startswith("zig-")


def sdl3() -> tuple[list[str], list[str], str] | None:
    """SDL3's compile flags, link flags and library folder: the system's (pkg-config sdl3), else the launcher's own copy
    (YW4_SDL3_DIR, its builder/sdl3: decision 0071) read directly. None: neither."""
    if shutil.which("pkg-config"):
        try:
            pc = lambda *a: subprocess.run(["pkg-config", *a, "sdl3"], capture_output=True, text=True, check=True).stdout
            return pc("--cflags").split(), pc("--libs").split(), pc("--variable=libdir").strip()
        except subprocess.CalledProcessError:
            pass
    own = Path(os.environ.get("YW4_SDL3_DIR") or "/nonexistent")
    so = own / "lib" / "libSDL3.so.0"
    if (own / "include" / "SDL3" / "SDL.h").is_file() and so.is_file():
        return [f"-I{own / 'include'}"], [str(so)], str(so.parent)  # linked by name: yw4 then needs libSDL3.so.0
    return None


def cxx_runtime(cc: str) -> list[str] | None:
    """The link flags for the C++ runtime of NVIDIA's NGX library (DLSS), which was built with GCC's libstdc++. With the
    system's compiler, -lstdc++. Ours (Zig) brings LLVM's libc++, another ABI, so the system's libstdc++.so.6 is named
    by its path: every desktop has it (Steam and KDE need it), without the development package -lstdc++ needs.
    None: not found (the game is then built without DLSS)."""
    if not is_own(cc):
        return ["-lstdc++"]
    return next(([str(Path(d) / "libstdc++.so.6")] for d in LIB_DIRS if (Path(d) / "libstdc++.so.6").is_file()), None)


def default_tools_dir(cfg: config.Config) -> Path:
    """Where the downloaded tools go: in the build's deps/, which the launcher's Free up space deletes."""
    return cfg.work_dir / "deps" / "linux-tools"


def own_forced() -> bool:
    return os.environ.get("YW4_OWN_TOOLS") == "1"


def can_download() -> bool:
    return platform.machine() in ("x86_64", "AMD64")


def will_download(cfg: config.Config, tools_dir: Path) -> bool:
    """Would check() download something? (Only for the launcher's time estimate.)"""
    if not can_download():
        return False
    need_cc = own_forced() or not find_compiler(cfg.cc)
    need_glslang = own_forced() or not find_glslang(cfg)
    return ((need_cc and not (tools_dir / "zig" / ".version").is_file())
            or (need_glslang and not (tools_dir / "glslang" / ".version").is_file()))


def check(cfg: config.Config, tools_dir: Path | None = None) -> str:
    """The compiler to build with. A compiler or glslang the system lacks is downloaded into `tools_dir` (default:
    default_tools_dir); the downloaded glslang's folder goes first on PATH, so every later step finds it. Raises
    RuntimeError naming whatever is still missing and how to install it."""
    tools_dir = tools_dir or default_tools_dir(cfg)
    missing = []
    cc = None if own_forced() else find_compiler(cfg.cc)
    if not cc and can_download():
        cc = str(deps.fetch_zig(tools_dir))
        if not good(cc):
            raise RuntimeError(f"the downloaded compiler ({cc}) doesn't run on this PC")
    if not cc:
        missing.append(f"a C compiler: Clang {MIN_CLANG} or newer (or GCC {MIN_GCC} or newer)"
                       + (f"; [tools] cc = {cfg.cc} is older or doesn't run" if cfg.cc else ""))
    if (own_forced() or not find_glslang(cfg)) and can_download():
        exe = deps.fetch_glslang_linux(tools_dir)
        os.environ["PATH"] = os.pathsep.join([str(exe.parent), os.environ.get("PATH", "")])
    if not find_glslang(cfg):
        missing.append("glslang (glslangValidator)")
    if not sdl3():
        missing.append("SDL3 with its headers (pkg-config sdl3)")
    if missing:
        raise RuntimeError("the build needs these, not found on this PC:\n" + "".join(f"  - {m}\n" for m in missing)
                           + "Install them with your package manager, then build again:\n" + INSTALL)
    return cc


def main() -> int:
    if sys.platform == "win32":
        print("Windows: the launcher brings its own compiler and tools")
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tools", type=Path, help="where to download missing tools (default: <work_dir>/deps/linux-tools)")
    args = ap.parse_args()
    try:
        cfg = config.load(require_exefs=False)
        cc = check(cfg, args.tools)
    except (RuntimeError, OSError, config.ConfigError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    glslang = find_glslang(cfg)
    print(f"compiler: {cc} ({' '.join(map(str, version_of(cc) or ()))}); glslang: {glslang}; SDL3: {sdl3()[2] or 'found'}")
    # by hand (no launcher): each later step is its own process, so a downloaded tool must be named in the config
    tools_dir = (args.tools or default_tools_dir(cfg)).resolve()
    own = [f'{k} = "{v}"' for k, v in (("cc", cc), ("glslang", glslang))
           if v and tools_dir in Path(v).resolve().parents and str(cfg.cc if k == "cc" else cfg.glslang) != v]
    if own:
        print("to build by hand with the downloaded tools, put these under [tools] in your config:\n  " + "\n  ".join(own))
    return 0


if __name__ == "__main__":
    sys.exit(main())
