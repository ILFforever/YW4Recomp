"""Fetch the build's binary dependencies into <work_dir>/deps/ (decision 0019).

    python -m tools.yw4.deps

Only SDL3 so far (zlib licence): on Windows the official MinGW development release, pinned
by version and SHA-256. Linux builds use the system's SDL3 (pkg-config sdl3) instead.
Nothing here is committed: the archive is checked against the pinned hash and unpacked
into its own folder.
"""

from __future__ import annotations

import hashlib
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

from . import config

SDL_VERSION = "3.4.18"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-devel-{SDL_VERSION}-mingw.tar.gz"
SDL_SHA256 = "049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf"


def sdl_dir(cfg: config.Config) -> Path:
    """The unpacked SDL3 for x86-64 MinGW: include/, lib/, bin/SDL3.dll."""
    return cfg.work_dir / "deps" / f"SDL3-{SDL_VERSION}" / "x86_64-w64-mingw32"


def fetch_sdl(cfg: config.Config) -> Path:
    out = sdl_dir(cfg)
    if (out / "bin" / "SDL3.dll").is_file():
        return out
    deps = cfg.work_dir / "deps"
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / Path(SDL_URL).name
    if not archive.is_file():
        print(f"downloading {SDL_URL}")
        with urllib.request.urlopen(SDL_URL) as r, open(archive, "wb") as f:
            shutil.copyfileobj(r, f)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SDL_SHA256:
        archive.unlink()
        raise RuntimeError(f"{archive.name}: SHA-256 {digest}, expected {SDL_SHA256}")
    with tarfile.open(archive) as t:
        t.extractall(deps, filter="data")  # "data": no absolute paths, links or devices
    if not (out / "bin" / "SDL3.dll").is_file():
        raise RuntimeError(f"{archive.name} unpacked without {out / 'bin' / 'SDL3.dll'}")
    return out


def main() -> int:
    try:
        cfg = config.load()
        if sys.platform != "win32":
            print("not Windows: use the system's SDL3 (pkg-config sdl3)")
            return 0
        print(f"SDL3 {SDL_VERSION}: {fetch_sdl(cfg)}")
    except (config.ConfigError, RuntimeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
