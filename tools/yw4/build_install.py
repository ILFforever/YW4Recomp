"""Build the game into a launcher's install folder: every step, in order (decision 0054).

    python -m tools.yw4.build_install --mode files|folders [--exefs DIR --romfs DIR] [--saves install|appdata]
                                      [--toolchain DIR] [--no-fsr] [--no-dlss]

The launcher runs only this, with YW4_CONFIG naming the install's config.toml and YW4_PROGRESS=1. The steps live
here, in the game's code, so a game update can change them without a new launcher. What the launcher reads on
stdout (everything else is its log):

    @steps <json>              once, first: [{"id", "label", "seconds", "parts": {<progress step>: [from, to, text, unit]}}]
    @step <id> start|done|failed
    @progress <step> <done> <total> <what>     (tools/yw4/progress.py), placed inside a step by its "parts"

A progress step is looked up as "<step>:<what>" first, then "<step>". unit: "bytes" (shown as sizes), "bytes+files"
(sizes drive the bar, <what> is "<files done>/<files>"), or none (a count). "seconds" are measured times
(2026-11-05, 16-thread laptop, game on an SSD); compiling is scaled by the CPU's threads.

Updating an install that already has a game: right before the steps that overwrite what the game runs from (the
translated shaders, then yw4.exe and its files), those are moved to work/previous/. If a later step fails they are
put back, so the old game still plays; once the build is done they are deleted.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

from . import config, deps, progress

PROTOCOL = 2  # 2 adds --fresh-files; the @steps/@step/@progress lines and the options above; a launcher that knows less refuses
# The oldest launcher that can build this game code (release.toml, decision 0054): raise it when a change here needs
# something only a newer launcher does, e.g. a new protocol, a new bundled tool or a newer Python.
MIN_LAUNCHER = "0.5.0"


def steps(mode: str, toolchain: bool) -> list[dict]:
    cores = os.cpu_count() or 16
    out = []
    if toolchain:
        out.append({"id": "tools", "label": "Downloading the compiler", "seconds": 30,
                    "parts": {"compiler": [0, 0.6, "Downloading the compiler", "bytes"],
                              "compilerunpack": [0.6, 1, "Unpacking the compiler", "bytes"]}})
    out += [
        {"id": "game", "label": "Unpacking your game files" if mode == "files" else "Copying your game files",
         "seconds": 60, "parts": {"unpack": [0, 0.07, "Unpacking the NSP or XCI", "bytes"],
                                  "extract": [0.07, 0.99, "Extracting the game files", "bytes"],
                                  "icon": [0.99, 1, "Taking the game icon"], "copy": [0, 1, "Copying", "bytes"]}},
        {"id": "version", "label": "Checking the version", "seconds": 3,
         "parts": {"ingest": [0, 1, "Checking the game code"]}},
        {"id": "translate", "label": "Translating the game code", "seconds": 30,
         "parts": {"translate": [0, 0.55, "Translating the game code"],
                   "translate:sdk": [0.55, 1, "Translating the system code"]}},
        {"id": "shaders", "label": "Preparing shaders", "seconds": 40,
         "parts": {"shaders": [0.3, 1, "Translating shaders"]}},
        {"id": "compile", "label": "Compiling", "seconds": round(360 * min(3.0, max(0.5, 16 / cores))),
         "parts": {"download": [0, 0.1, "Downloading", "bytes"], "compile": [0.1, 0.95, "Compiling", "bytes+files"],
                   "link": [0.95, 1, "Linking"]}},
        {"id": "finish", "label": "Finishing up", "seconds": 1, "parts": {}},
    ]
    return out


def say(line: str) -> None:
    print(line, flush=True)


# `python -c BOOT <code folder> <module> <args>`: `python -m <module>` from that folder. The launcher's bundled
# Python (its ._pth file) only looks where it is told, and a game update lives in its own folder, so this game
# code's folder is put first on the path; multiprocessing's workers get the same path from their parent.
BOOT = ("import runpy, sys; sys.path.insert(0, sys.argv[1]); m = sys.argv[2]; sys.argv = [m] + sys.argv[3:]; "
        "runpy.run_module(m, run_name='__main__', alter_sys=True)")


def run(module: str, *args: str) -> None:
    """One build step's tool, in its own process (as before), its output straight to the launcher."""
    say(f"> python -m {module} {' '.join(args)}".rstrip())
    r = subprocess.run([sys.executable, "-u", "-c", BOOT, str(config.REPO_ROOT), module, *args], cwd=config.REPO_ROOT)
    if r.returncode:
        raise RuntimeError(f"{module} failed")


def romfs_bytes() -> int:
    target = tomllib.loads((config.REPO_ROOT / "target.toml").read_text())
    return int(target["extracted"]["romfs_bytes"])


def folder_size(d: Path) -> int:
    return sum(p.stat().st_size for p in d.rglob("*") if p.is_file()) if d.is_dir() else 0


def copy_tree(src: Path, dst: Path) -> None:
    """Copies a folder with progress, skipping files already there at full size (a stopped copy goes on)."""
    total = folder_size(src)
    done = 0
    for p in src.rglob("*"):
        if not p.is_file():
            continue
        d = dst / p.relative_to(src)
        d.parent.mkdir(parents=True, exist_ok=True)
        size = p.stat().st_size
        if not d.is_file() or d.stat().st_size != size:
            shutil.copy2(p, d)
        done += size
        progress.report("copy", done, total, every=0.25)


def old_files(d: Path) -> Path:
    return d.with_name(d.name + ".previous")


def put_files_back(cfg: config.Config) -> None:
    """After a stopped --fresh-files build: the game files it set aside back in place, over what it left there. The
    installed game reads them directly (game.ini), so without this it wouldn't run any more."""
    for d in (cfg.exefs_dir, cfg.romfs_dir):
        if old_files(d).is_dir():
            shutil.rmtree(d, ignore_errors=True)
            old_files(d).rename(d)


def game_files(cfg: config.Config, args: argparse.Namespace) -> None:
    exefs, romfs = cfg.exefs_dir, cfg.romfs_dir
    if args.fresh_files:
        if args.mode == "folders" and any(Path(p).resolve() in (exefs.resolve(), romfs.resolve()) for p in (args.exefs, args.romfs)):
            raise RuntimeError("those are this install's own game files: choose your original folders")
        for d in (exefs, romfs):  # set aside, not deleted: dropped only once the new ones are in place
            if d.is_dir():
                shutil.rmtree(old_files(d), ignore_errors=True)
                d.rename(old_files(d))
    elif (exefs / "main").is_file() and folder_size(romfs) == romfs_bytes():
        say("game files already here: skipped")
        return
    new_game_files(cfg, args)
    for d in (exefs, romfs):
        shutil.rmtree(old_files(d), ignore_errors=True)


def new_game_files(cfg: config.Config, args: argparse.Namespace) -> None:
    exefs, romfs = cfg.exefs_dir, cfg.romfs_dir
    if args.mode == "folders":
        copy_tree(Path(args.exefs), exefs)
        copy_tree(Path(args.romfs), romfs)
        return
    for d in (exefs, romfs):  # the extractor wants empty folders; a stopped extraction left ours half full
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
    run("tools.yw4.extract")


def game_ini(cfg: config.Config, saves: str) -> None:
    """game.ini next to yw4.exe: its folders relative to it (decision 0050), so the install folder can move."""
    inst = cfg.work_dir.parent
    if saves == "appdata":
        base = Path(os.environ.get("APPDATA", "")) / "YW4 Recomp"
        save, settings = str(base / "save"), str(base / "settings.ini")
        (base / "save").mkdir(parents=True, exist_ok=True)
    else:
        save, settings = "../../save", "../../settings.ini"
        (inst / "save").mkdir(exist_ok=True)
    (cfg.work_dir / "build" / "game.ini").write_text(
        "# Written by the YW4 Recomp launcher: how yw4.exe runs on its own (decision 0050).\n"
        f"YW4_EXEFS=../exefs\nYW4_ROMFS=../../data/romfs\nYW4_SAVE={save}\nYW4_SETTINGS_FILE={settings}\n"
        "YW4_DLC_DIR=../../data/dlc\nYW4_DLCROMFS_DIR=../../data/dlc-romfs\nYW4_MODS=../../mods\n"
        "YW4_SCREENSHOT_DIR=../../screenshots\nYW4_LOG_DIR=../../logs\nYW4_PERF_DIR=../../logs\nYW4_RECORD=0\n")


class Previous:
    """The installed game's run-time files, moved aside while an update rebuilds them (work/previous/)."""

    def __init__(self, work: Path):
        self.work, self.dir, self.moved = work, work / "previous", []

    def keep(self, rels: list[str]) -> None:
        exe_name = "yw4.exe" if sys.platform == "win32" else "yw4"
        if not (self.work / "build" / exe_name).is_file() and not self.moved:
            return  # nothing playable yet: a first build has nothing to keep
        for rel in rels:
            src = self.work / rel
            if src.exists():
                dst = self.dir / rel
                shutil.rmtree(dst, ignore_errors=True) if dst.is_dir() else dst.unlink(missing_ok=True)
                dst.parent.mkdir(parents=True, exist_ok=True)
                src.rename(dst)  # same drive: instant
                self.moved.append(rel)
        if self.moved:
            say(f"the installed game is kept in {self.dir} until this build is done")

    def put_back(self) -> bool:
        """Everything in work/previous/ back in its place (over what a stopped build left there). Works from the
        folder itself, not from this run's list, so it also mends a build the launcher's Cancel ended mid-way."""
        kept = [p for p in [*(self.dir / "build").glob("*"), self.dir / "shaders" / "glsl"] if p.exists()]
        for src in kept:
            dst = self.work / src.relative_to(self.dir)
            shutil.rmtree(dst, ignore_errors=True) if dst.is_dir() else dst.unlink(missing_ok=True)
            dst.parent.mkdir(parents=True, exist_ok=True)
            src.rename(dst)
        shutil.rmtree(self.dir, ignore_errors=True)
        return bool(kept)

    def drop(self) -> None:
        shutil.rmtree(self.dir, ignore_errors=True)


def ensure_install_icon(cfg: config.Config) -> None:
    dst = cfg.work_dir / "icon"
    if (dst / "yw4.ico").is_file():
        return
    src = config.REPO_ROOT / "work" / "icon"
    if (src / "yw4.ico").is_file():
        dst.mkdir(parents=True, exist_ok=True)
        for name in ("icon.jpg", "yw4.ico", "icon.png"):
            if (src / name).is_file():
                shutil.copy2(src / name, dst / name)
        return
    try:
        run("tools.yw4.icon")
    except Exception:
        pass


def game_runtime_files(work: Path) -> list[str]:
    b = work / "build"
    exe_name = "yw4.exe" if sys.platform == "win32" else "yw4"
    names = [exe_name, "game.ini", "shaders"] + [p.name for p in b.glob("*.dll")] if b.is_dir() else []
    return [f"build/{n}" for n in names]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("files", "folders"))
    ap.add_argument("--exefs")
    ap.add_argument("--romfs")
    ap.add_argument("--saves", choices=("install", "appdata"), default="install")
    ap.add_argument("--toolchain", help="download the compiler into this folder first")
    ap.add_argument("--fresh-files", action="store_true",
                    help="extract or copy the game files again, even if this install already has them")
    ap.add_argument("--no-fsr", action="store_true")
    ap.add_argument("--no-dlss", action="store_true")
    ap.add_argument("--protocol", action="store_true", help="print the protocol number and stop")
    ap.add_argument("--recover", action="store_true",
                    help="after a stopped build: put the kept game back (the launcher runs it after Cancel)")
    args, _ = ap.parse_known_args()
    if args.protocol:
        print(PROTOCOL)
        return 0
    if args.recover:
        cfg = config.load(require_exefs=False)
        put_files_back(cfg)
        if Previous(cfg.work_dir).put_back():
            say("the game you had is back and still plays")
        return 0
    args = ap.parse_args()
    if not args.mode:
        ap.error("--mode is needed")
    if args.mode == "folders" and not (args.exefs and args.romfs):
        ap.error("--mode folders needs --exefs and --romfs")

    plan = steps(args.mode, bool(args.toolchain))
    say("@steps " + json.dumps(plan))
    cfg = config.load(require_exefs=False)
    prev = Previous(cfg.work_dir)
    put_files_back(cfg)
    if prev.put_back():  # a stopped build's leftovers first: the kept game is the one that works
        say("a stopped build had set the game aside: it is back")
    build_args = [*(["--no-fsr"] if args.no_fsr else []), *(["--no-dlss"] if args.no_dlss else [])]
    actions = {
        "tools": lambda: say(f"compiler: {deps.fetch_toolchain(Path(args.toolchain))}"),
        "game": lambda: game_files(cfg, args),
        "version": lambda: run("tools.yw4.ingest"),
        "translate": lambda: run("recomp.translate"),
        "shaders": lambda: (prev.keep(["shaders/glsl"]), run("tools.yw4.shaders"), run("recomp.shader", "--check")),
        "compile": lambda: (ensure_install_icon(cfg), prev.keep(game_runtime_files(cfg.work_dir)), run("recomp.build", *build_args)),
        "finish": lambda: game_ini(cfg, args.saves),
    }
    current = None
    try:
        for s in plan:
            current = s["id"]
            say(f"@step {current} start")
            actions[current]()
            say(f"@step {current} done")
    except (RuntimeError, OSError, config.ConfigError) as e:
        say(f"@step {current} failed")
        print(f"error: {e}", file=sys.stderr, flush=True)
        put_files_back(cfg)
        if prev.put_back():
            say("the build failed: the game you had is back and still plays")
        return 1
    except KeyboardInterrupt:  # the launcher's Cancel ends the whole process tree; a terminal's Ctrl+C lands here
        put_files_back(cfg)
        prev.put_back()
        raise
    prev.drop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
