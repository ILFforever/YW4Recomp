"""Load and validate config.local.toml.

Every tool gets user-supplied paths from here, so no path to game data is
ever hardcoded (CLAUDE.md rule 2).
"""

from __future__ import annotations

import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = REPO_ROOT / "config.local.toml"
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


def _resolve(value: str) -> Path:
    p = Path(value).expanduser()
    return p if p.is_absolute() else (REPO_ROOT / p).resolve()


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
    )

    for name in ("exefs_dir", "romfs_dir"):
        d = getattr(cfg, name)
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
