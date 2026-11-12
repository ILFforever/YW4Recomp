"""Test runs by name, checked before and after: the save, the replay, the settings and what to capture.

    python -m tools.dev.scene NAME [NAME ...] [--settings none|owner|FILE] [--shots 1700,1800] [--frames N]
                              [--seconds N] [--task NAME] [--allow-unknown] [ENV=value ...]
    python -m tools.dev.scene --list
    python -m tools.dev.scene --clean TASK        delete <work>/shots_TASK
    python -m tools.dev.scene --prune DAYS        delete logs and perf files older than DAYS

NAME is a scene or a group of tools/dev/scenes.toml (`busy-all`: every busy scene, one table at the end).
Everything a task writes goes in one folder, <work>/shots_<task>/<scene>/: the log, the per-frame perf log, the
events, the screenshots, the settings used and result.json. Delete it when the task is done (--clean).

Before the run it refuses, with the reason, when: the game is already running; a save or replay is missing; the
settings file isn't a full path to a file; an ENV name is one the runtime never reads (a typo does nothing,
silently); captures are asked for with under 30 GB free. `--settings owner` plays with a COPY of the owner's
settings.ini, so a test can't change it.

How long it runs: with something scheduled at a frame (--shots, YW4_SET_AT, YW4_AB_AT, YW4_DUMP_IMAGE_AT ...) or
--frames, until that frame has been presented (YW4_EXIT_AT), however fast the game runs: no reckoning of frames
from seconds. Otherwise the scene's `seconds`. --seconds is a limit on top (default 900 with a last frame).

After the run it checks: every scheduled frame was reached, every screenshot exists, the game did not stop on
an error, and each same-frame A/B (YW4_AB_AT) found 0 texels different. It prints one line per scene (frame rate
and frame times in play, spikes, stalls) and exits 1 when a check failed. "In play" starts 8 s after the 3D
world first draws (over 100 draws a frame), so loading isn't counted.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import statistics
import sys
import time
import tomllib
from pathlib import Path

from tools.dev.run import game_running, run
from tools.yw4 import config

REPO = Path(__file__).resolve().parents[2]
# variables whose value is a list of items that each start with a frame number (runtime/src/perf.c has the same list)
SCHEDULED = ("YW4_SCREENSHOTS", "YW4_SET_AT", "YW4_DUMP_IMAGE_AT", "YW4_AB_AT", "YW4_DUMP_AT", "YW4_CAPTURE_LAYER_AT",
             "YW4_GPU_TRACE_AT", "YW4_FG_AT", "YW4_FLASH_SAVE", "YW4_GBUFFER_ALTERNATE", "YW4_PCSS_ALTERNATE")
OLD_PANEL_KEYS = {"F1", "F2", "UP", "DOWN", "LEFT", "RIGHT", "ENTER"}


def scheduled_frames(env: dict[str, str]) -> list[tuple[str, int]]:
    """(variable, frame) for everything `env` schedules at a frame number."""
    out = []
    for name in SCHEDULED:
        for item in env.get(name, "").split(","):
            m = re.match(r"\s*(\d+)", item)
            if m:
                out.append((name, int(m.group(1))))
    return out


def known_variables() -> set[str]:
    """Every YW4_ name the runtime's source mentions in quotes: what an ENV=value can mean something to."""
    names: set[str] = set()
    for f in (REPO / "runtime" / "src").iterdir():
        if f.suffix in (".c", ".h", ".cpp"):
            names.update(re.findall(r'"(YW4_[A-Z0-9_]+)"', f.read_text(encoding="utf-8", errors="ignore")))
    return names


def analyse(csv: Path, events: Path | None) -> dict:
    """Frame times in play and the run's events, from the runtime's perf_<time>.csv and .jsonl."""
    rows = []
    for line in csv.read_text(errors="ignore").splitlines()[1:]:
        a = line.split(",")
        if len(a) >= 10 and a[0].isdigit():
            rows.append((int(a[0]), float(a[1]), float(a[2]), float(a[3]), int(a[6]), float(a[9] or 0)))
    out: dict = {"frames": len(rows), "last_frame": rows[-1][0] if rows else 0}
    start = next((t for _, t, _, _, d, _ in rows if d > 100), None)
    play = [r for r in rows if start is not None and r[1] >= start + 8]
    if len(play) > 100:
        iv = sorted(r[2] for r in play)
        med = statistics.median(iv)
        spikes = sorted((r for r in play if r[2] > max(2 * med, med + 8)), key=lambda r: -r[2])
        out["play"] = {
            "from_frame": play[0][0], "seconds": round(play[-1][1] - play[0][1], 1), "frames": len(play),
            "fps": round(1000 * len(play) / sum(iv), 1), "p50_ms": round(med, 2),
            "p95_ms": round(iv[int(len(iv) * .95)], 2), "p99_ms": round(iv[int(len(iv) * .99)], 2),
            "max_ms": round(iv[-1], 1), "draws": int(statistics.median(r[4] for r in play)),
            "work_ms": round(statistics.median(r[3] for r in play), 2),
            "gpu_ms": round(statistics.median(r[5] for r in play), 2), "spikes": len(spikes),
            "worst": [{"frame": r[0], "ms": round(r[2], 1)} for r in spikes[:6]]}
    ev = []
    if events and events.is_file():
        for line in events.read_text(errors="ignore").splitlines():
            try:
                ev.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # a line cut short when the game was stopped
    out["events"] = ev
    return out


def check(result: dict, env: dict[str, str], shot_dir: Path | None, shots: list[int]) -> list[str]:
    """What went wrong, in words; empty when the run did what it was asked."""
    bad = []
    last = result["last_frame"]
    for name, frame in scheduled_frames(env):
        if frame > last:
            bad.append(f"never reached: {name} frame {frame} (the run ended at frame {last})")
    for frame in shots:
        if shot_dir and not list(shot_dir.glob(f"frame_{frame:06d}*.bmp")):
            bad.append(f"no screenshot of frame {frame} in {shot_dir}")
    for e in result["events"]:
        if e.get("kind") == "fatal":
            bad.append(f"the game stopped: {e.get('text', '')}")
        elif e.get("kind") == "ab" and e.get("differ", 0):
            bad.append(f"A/B {e.get('pass')} frame {e['frame']} target {e.get('target')}: {e['differ']} of "
                       f"{e.get('texels')} texels differ")
    if result.get("returncode") not in (0, None):
        bad.append(f"the game exited with code {result['returncode']}")
    if result.get("timed_out") and result.get("exit_at"):
        bad.append(f"stopped by the time limit before frame {result['exit_at']} (last frame {last})")
    return bad


def folder_size(p: Path) -> int:
    return sum(f.stat().st_size for f in p.rglob("*") if f.is_file())


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="scenes or groups (tools/dev/scenes.toml), then ENV=value pairs")
    ap.add_argument("--settings", default="none", help="none (the game's defaults, 30 frames/s), owner, or a file")
    ap.add_argument("--shots", default="", help="screenshot frames, comma separated")
    ap.add_argument("--frames", type=int, help="end the run once this frame has been presented")
    ap.add_argument("--seconds", type=int, help="time limit")
    ap.add_argument("--task", default="scene", help="folder name: <work>/shots_<task>")
    ap.add_argument("--allow-unknown", action="store_true", help="pass ENV names the runtime's source doesn't mention")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--clean", metavar="TASK")
    ap.add_argument("--prune", type=int, metavar="DAYS")
    a = ap.parse_intermixed_args(argv)  # names and ENV=value pairs on both sides of the options
    cfg = config.load()
    table = tomllib.loads((Path(__file__).parent / "scenes.toml").read_text(encoding="utf-8"))
    groups = table.pop("groups", {})
    if a.list:
        for name, s in table.items():
            print(f"{name:12} {s['save']:22} {s['replay']:14} {s.get('about', '')}")
        for name, members in groups.items():
            print(f"{name:12} = {' '.join(members)}")
        return 0
    if a.clean:
        d = cfg.work_dir / f"shots_{a.clean}"
        if d.is_dir():
            print(f"deleting {d} ({folder_size(d) / 1e6:.0f} MB)")
            shutil.rmtree(d)
        return 0
    if a.prune is not None:
        old = time.time() - a.prune * 86400
        gone = [f for d in ("perf", "logs") for f in (cfg.work_dir / d).glob("*") if f.is_file() and f.stat().st_mtime < old]
        size = sum(f.stat().st_size for f in gone)
        for f in gone:
            f.unlink()
        print(f"deleted {len(gone)} files older than {a.prune} days from perf/ and logs/ ({size / 1e6:.0f} MB)")
        return 0

    env_pairs = [n for n in a.names if "=" in n]
    names = [m for n in a.names if "=" not in n for m in groups.get(n, [n])]
    env = dict(p.split("=", 1) for p in env_pairs)
    shots = [int(x) for x in a.shots.split(",") if x.strip()]
    # ---- before the run: refuse what would fail silently
    problems = []
    if not names:
        problems.append("no scene named (--list shows them)")
    problems += [f"no scene or group called {n} (--list shows them)" for n in names if n not in table]
    if game_running():
        problems.append("yw4.exe is running: close it first (a second game spoils timings)")
    if not a.allow_unknown:
        known = known_variables()
        problems += [f"{k} is not a variable the runtime reads (a typo? --allow-unknown passes it anyway)"
                     for k in env if k.startswith("YW4_") and k not in known and k != "YW4_EXIT_AT"]
    settings = a.settings
    if settings == "owner":
        if not (cfg.work_dir / "build" / "settings.ini").is_file():
            problems.append(f"--settings owner: no {cfg.work_dir / 'build' / 'settings.ini'}")
    elif settings != "none":
        if not Path(settings).is_absolute() or not Path(settings).is_file():
            problems.append(f"--settings {settings}: give the full path of a file that exists (a bare name is looked "
                            "for in the game's home folder, and a new default file is made there)")
    for n in names:
        s = table.get(n)
        if s and not (cfg.work_dir / "testsaves" / s["save"]).is_dir():
            problems.append(f"{n}: no save {cfg.work_dir / 'testsaves' / s['save']}")
        if s and not (cfg.work_dir / "inputs" / s["replay"]).is_file():
            problems.append(f"{n}: no replay {cfg.work_dir / 'inputs' / s['replay']}")
    captures = bool(shots) or any(k in env for k in ("YW4_SCREENSHOTS", "YW4_DUMP_IMAGE_AT", "YW4_FLASH_SAVE", "YW4_DUMP_AT"))
    free_gb = shutil.disk_usage(cfg.work_dir).free / 1e9
    if captures and free_gb < 30:
        problems.append(f"only {free_gb:.0f} GB free: clean up before capturing (--clean, --prune)")
    if problems:
        print("not started:\n  " + "\n  ".join(problems), file=sys.stderr)
        return 2
    task_dir = cfg.work_dir / f"shots_{a.task}"
    others = [(d, folder_size(d)) for d in cfg.work_dir.glob("shots_*") if d.is_dir() and d != task_dir]
    if sum(s for _, s in others) > 1e9:
        print("note: capture folders of other tasks, to delete when those are done (--clean):")
        for d, s in sorted(others, key=lambda x: -x[1])[:8]:
            print(f"  {d.name}: {s / 1e6:.0f} MB")

    failed = False
    lines = []
    for n in names:
        s = table[n]
        out = task_dir / n
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        run_env = dict(env)
        if shots:
            run_env["YW4_SCREENSHOTS"] = ",".join(str(x) for x in shots)
        replay = cfg.work_dir / "inputs" / s["replay"]
        if settings == "owner":
            shutil.copy2(cfg.work_dir / "build" / "settings.ini", out / "settings_used.ini")
            run_env["YW4_SETTINGS_FILE"] = str(out / "settings_used.ini")
        elif settings != "none":
            run_env["YW4_SETTINGS_FILE"] = settings
        text = (out / "settings_used.ini").read_text(errors="ignore") if settings == "owner" else ""
        if "YW4_LEGACY_KEYS=1" not in text and run_env.get("YW4_LEGACY_KEYS") != "1":
            keys = {m for m in re.findall(r"^k \d+ (\S+)", replay.read_text(errors="ignore"), re.M)} & OLD_PANEL_KEYS
            if keys:
                print(f"note: {s['replay']} presses {' '.join(sorted(keys))}: those work the OLD panel; the overlay's "
                      "keys are MENU, M_SET, M_UP, M_DOWN, M_LEFT, M_RIGHT, M_OK, M_BACK")
        sched = scheduled_frames(run_env)
        exit_at = a.frames or (max(f for _, f in sched) + 30 if sched else 0)
        if exit_at:
            run_env["YW4_EXIT_AT"] = str(exit_at)
        run_env["YW4_PERF_DIR"] = str(out)
        seconds = a.seconds or (900 if exit_at else s.get("seconds", 120))
        how = f"until frame {exit_at} (at most {seconds} s)" if exit_at else f"for {seconds} s"
        print(f"{n}: {s['save']} + {s['replay']}, settings {settings}, {how}", flush=True)
        outcome: dict = {}
        log = run(cfg, cfg.work_dir / "testsaves" / s["save"], replay, seconds, False,
                  [f"{k}={v}" for k, v in run_env.items()], None, out if (shots or "YW4_SCREENSHOTS" in env) else None,
                  out / "run.log", outcome)
        csvs = sorted(out.glob("perf_*.csv"))
        result = analyse(csvs[-1], csvs[-1].with_suffix(".jsonl")) if csvs else {"frames": 0, "last_frame": 0, "events": []}
        result.update(scene=n, save=s["save"], replay=s["replay"], settings=settings, env=run_env, log=str(log),
                      exit_at=exit_at, **outcome)
        bad = check(result, run_env, out, shots)
        if not csvs:
            bad.append("no performance log was written (did the game start? see the log)")
        result["problems"] = bad
        (out / "result.json").write_text(json.dumps(result, indent=1))
        p = result.get("play")
        ev = result["events"]
        stalls = sum(1 for e in ev if e.get("kind") == "stall" and p and e["frame"] >= p["from_frame"])
        ab = [e for e in ev if e.get("kind") == "ab"]
        line = f"{n:11} " + (f"{p['fps']:5.1f} frames/s  median {p['p50_ms']:5.1f}  p99 {p['p99_ms']:5.1f}  worst "
                              f"{p['max_ms']:6.1f} ms  spikes {p['spikes']:2}  stalls {stalls:2}  ({p['seconds']:.0f} s in play)"
                              if p else f"{result['frames']} frames, none in play")
        if ab:
            line += f"  A/B: {sum(1 for e in ab if not e.get('differ'))} of {len(ab)} identical"
        lines.append(line + ("  FAILED" if bad else ""))
        for b in bad:
            print(f"  FAILED: {b}", flush=True)
        failed |= bool(bad)
    print()
    print("\n".join(lines))
    print(f"\nresults, logs and captures: {task_dir}  ({folder_size(task_dir) / 1e6:.0f} MB; "
          f"python -m tools.dev.scene --clean {a.task} when the task is done)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
