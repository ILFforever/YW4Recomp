"""One test run of the built game from a fresh copy of a save, then the log's summary lines.

    python -m tools.dev.run [--save DIR] [--replay FILE] [--seconds N] [--exit] [--shots 1800,1900]
                            [--shot-dir DIR] [--grep TEXT] [ENV=value ...]

The save directory is copied to <work>/testsaves/save_run first, so every run starts in the same place
(the game autosaves). ENV=value pairs go to the game's environment (docs/debugging.md). Refuses to run
while another yw4.exe is running: a second game spoils timings and the build can't replace the exe.
Prints the log's path and the lines matching --grep (default: the summaries of the passes and perf).
Use that path to read the log later: `ls -t work/logs` in Git Bash does not list the newest file first.

Examples:
    python -m tools.dev.run --save work/testsaves/save_school --exit YW4_GBUFFER=check
    python -m tools.dev.run --replay work/inputs/walk.txt --exit YW4_SHADOWS=native --shots 1400,1440
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from tools.yw4 import config

DEFAULT_GREP = ("gbuffer: check", "world =", "shadows: frame", "perf, whole run", "game work ms", "GPU busy ms",
                "gpu: pipeline took", "gpu_stop", "fatal")


def game_running() -> bool:
    if os.name != "nt":
        return subprocess.run(["pgrep", "-x", "yw4"], capture_output=True).returncode == 0
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq yw4.exe"], capture_output=True, text=True)
    return "yw4.exe" in r.stdout


def run(cfg: config.Config, save: Path | None, replay: Path | None, seconds: int, exit_at_end: bool,
        env_pairs: list[str], shots: str | None = None, shot_dir: Path | None = None,
        log: Path | None = None) -> Path:
    """One run; returns the path of the log it wrote (stdout and stderr of the game)."""
    if game_running():
        sys.exit("yw4.exe is running: close it first")
    run_save = cfg.work_dir / "testsaves" / "save_run"
    if save:
        shutil.rmtree(run_save, ignore_errors=True)
        shutil.copytree(save, run_save)
    env = dict(os.environ)
    env.setdefault("YW4_AUDIO", "0")
    env.setdefault("YW4_SETTINGS_FILE", "none")  # the owner's settings.ini stays out of test runs, and unchanged
    env.setdefault("YW4_MOUSE", "0")  # the game doesn't take the mouse: the owner can use the PC meanwhile
    if replay:
        env["YW4_REPLAY"] = str(replay)
    if exit_at_end:
        env["YW4_REPLAY_EXIT"] = "1"
    if shots:
        env["YW4_SCREENSHOTS"] = shots
    if shot_dir:
        env["YW4_SCREENSHOT_DIR"] = str(shot_dir)
    for pair in env_pairs:
        k, _, v = pair.partition("=")
        env[k] = v
    exe = cfg.work_dir / "build" / ("yw4.exe" if os.name == "nt" else "yw4")
    log = log or cfg.work_dir / "logs" / f"dev_run_{time.strftime('%Y%m%d_%H%M%S')}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    args = [str(exe), str(cfg.work_dir / "exefs"), str(cfg.romfs_dir), str(run_save if save else cfg.work_dir / "save")]
    with open(log, "wb") as out:
        try:
            subprocess.run(args, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=seconds or None)
        except subprocess.TimeoutExpired:
            pass  # the run's time is up; the log has what it wrote
    return log


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--save", type=Path, help="save directory to start from (copied first); default: work/save as is")
    ap.add_argument("--replay", type=Path, default=Path("work/inputs/load_save.txt"),
                    help="input replay (default: presses through the title into the save)")
    ap.add_argument("--seconds", type=int, default=120, help="stop the game after this long (0: when you close it)")
    ap.add_argument("--exit", action="store_true", help="quit when the replay ends (YW4_REPLAY_EXIT)")
    ap.add_argument("--shots", help="screenshot frames, comma separated")
    ap.add_argument("--shot-dir", type=Path)
    ap.add_argument("--grep", action="append", help="log lines to print (repeatable)")
    ap.add_argument("env", nargs="*", help="ENV=value for the game")
    a = ap.parse_args(argv)
    cfg = config.load()
    log = run(cfg, a.save, a.replay, a.seconds, a.exit, a.env, a.shots, a.shot_dir)
    print(log)
    want = tuple(a.grep) if a.grep else DEFAULT_GREP
    for line in log.read_text(errors="ignore").splitlines():
        if any(w in line for w in want):
            print(" ", line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
