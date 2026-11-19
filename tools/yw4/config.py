"""Load and validate config.local.toml.

Every tool gets user-supplied paths from here, so no path to game data is
ever hardcoded (CLAUDE.md rule 2).
"""

from __future__ import annotations

import os
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

def no_backslash(p: Path) -> Path:
    """Linux: a path through a folder whose name has a backslash, reached through a link without one instead. Clang
    can't find an #embed file by a relative path from such a folder (ui.c's fonts: "file not found"), zig can't read
    its dependency lists there, and AMD's shader compiler crashed (issue #22: launchers before 1.5.0 named a folder
    chosen on Linux "...\\YW4 Recomp"). The link is the one the zig wrappers make (deps.ZIG_WRAPPER), made atomically.
    Elsewhere, and on Windows, `p` as it is."""
    s = str(p)
    if os.name != "posix" or "\\" not in s:
        return p
    head, tail = s.split("\\", 1)
    top = head + "\\" + tail.split("/", 1)[0]  # the folder whose name has the (first) backslash
    link = Path(os.environ.get("XDG_RUNTIME_DIR") or "/tmp") / f"yw4-recomp-{os.getuid()}"
    try:
        if os.readlink(link) == top:
            return Path(str(link) + s[len(top):])
    except OSError:
        pass
    try:
        tmp = f"{link}.{os.getpid()}"
        os.symlink(top, tmp)
        os.replace(tmp, link)
    except OSError:
        return p  # no link (a read-only /tmp?): as before
    return Path(str(link) + s[len(top):])


REPO_ROOT = no_backslash(Path(__file__).resolve().parents[2])
# YW4_CONFIG: another config file, e.g. the one the launcher writes for an install folder (decision 0050)
CONFIG_PATH = Path(os.environ["YW4_CONFIG"]) if os.environ.get("YW4_CONFIG") else REPO_ROOT / "config.local.toml"
EXAMPLE_PATH = REPO_ROOT / "config.example.toml"


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    exefs_dir: Path
    romfs_dir: Path
    base_nsp: Path | None
    update_nsp: Path | None
    work_dir: Path
    hactool_exe: Path | None
    keys_dir: Path | None
    pi_host: str
    pi_key: Path | None
    ghidra_dir: Path | None
    emulator_exe: Path | None
    cc: str
    glslang: Path | None = None
    dlc_dir: Path | None = None

    def dlc_env(self) -> dict[str, str]:
        """Where the game looks for the owner's DLC on boot (decision 0049) and its extracted RomFS folders
        (decision 0056): YW4_DLC_DIR = [input] dlc_dir, YW4_DLCROMFS_DIR = <romfs_dir>/../dlc-romfs."""
        env = {"YW4_DLCROMFS_DIR": str(self.romfs_dir.parent / "dlc-romfs")}
        if self.dlc_dir and self.dlc_dir.is_dir():
            env["YW4_DLC_DIR"] = str(self.dlc_dir)
        return env


def _resolve(value: str) -> Path:
    p = Path(value).expanduser()
    return no_backslash(p if p.is_absolute() else (REPO_ROOT / p).resolve())


def _optional(value: str) -> Path | None:
    return _resolve(value) if value else None


def load(path: Path = CONFIG_PATH, require_exefs: bool = True) -> Config:
    """require_exefs=False is for the extract step, which creates exefs_dir's contents."""
    if not path.is_file():
        raise ConfigError(
            f"{path.name} not found. Copy {EXAMPLE_PATH.name} to {path.name} "
            "and fill in your paths."
        )
    with path.open("rb") as f:
        raw = tomllib.load(f)

    inp = raw.get("input", {})
    out = raw.get("output", {})
    tools = raw.get("tools", {})

    for key in ("exefs_dir", "romfs_dir"):
        if not inp.get(key):
            raise ConfigError(f"[input] {key} is empty in {path.name}.")

    cfg = Config(
        exefs_dir=_resolve(inp["exefs_dir"]),
        romfs_dir=_resolve(inp["romfs_dir"]),
        base_nsp=_optional(inp.get("base_nsp", "")),
        update_nsp=_optional(inp.get("update_nsp", "")),
        work_dir=_resolve(out.get("work_dir", "work")),
        hactool_exe=_optional(tools.get("hactool_exe", "")),
        keys_dir=_optional(tools.get("keys_dir", "")),
        pi_host=tools.get("pi_host", ""),
        pi_key=_optional(tools.get("pi_key", "")),
        ghidra_dir=_optional(tools.get("ghidra_dir", "")),
        emulator_exe=_optional(tools.get("emulator_exe", "")),
        cc=tools.get("cc", ""),
        glslang=_optional(tools.get("glslang", "")),
        dlc_dir=_optional(inp.get("dlc_dir", "")),
    )

    for name in ("exefs_dir", "romfs_dir"):
        d = getattr(cfg, name)
        if not require_exefs and not d.exists():
            d.mkdir(parents=True)  # a clean PC has no folders yet: the extract step fills them
        if not d.is_dir():
            raise ConfigError(f"[input] {name} does not exist or is not a folder: {d}")
    if require_exefs and not (cfg.exefs_dir / "main").is_file():
        raise ConfigError(f"No 'main' file in exefs_dir: {cfg.exefs_dir}")

    return cfg


def main() -> int:
    try:
        cfg = load()
    except ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    for field, value in vars(cfg).items():
        print(f"{field:13} {value}")
    if any(s in str(cfg.work_dir).lower() for s in ("onedrive", "dropbox", "google drive")):
        print("note: work_dir is under a cloud-folder path; if sync is on, "
              "game-derived files would be uploaded.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
