"""Catch the minimap going blank: run the game with the minimap watch on, then say what it saw.

    python -m tools.dev.minimap run SCENE [--settings none|owner|FILE] [--frames N] [--seconds N]
    python -m tools.dev.minimap play [--save NAME] [--replay FILE]
    python -m tools.dev.minimap report [FOLDER]

`run` plays a scene of tools/dev/scenes.toml (tools.dev.scene: `walk` is the one that moves). `play` starts the
game for the owner to play, with their settings and keys, from a fresh copy of a save (default
save_base_lategame), until they close it: the bug shows "sometimes", and a person walking around finds it sooner
than a replay does. Both end with the report; `report` prints it again for a folder (default: the last one).

The watch itself is in the runtime (runtime/src/gpu_watch.c, YW4_MINIMAP_WATCH=1). Every frame, for each of the
minimap's own images (340x304, 516x460), it counts:

    draws    draws into the image                 (fewer: the game, or we, didn't draw the map)
    clears   colour clears of it
    shown    draws elsewhere that sample it       (0: the image never reached the screen)
    missing  texture slots of its draws with no texture behind them (they read black)
    fresh    textures made or uploaded for its draws in that very frame
    lit      texels brighter than black, per mille (the image read back: the truth about "blank")
    opaque   texels with alpha, per mille

and when a count or the picture drops it writes an event, the pictures before / blank / after (.bmp) and the two
frames' draws with their textures (minimap_<frame>_draws.txt). Everything goes in <work>/shots_minimap/<name>/:
delete it when done (`python -m tools.dev.scene --clean minimap`).

A gap of a few frames is the bug. A long one is the game hiding the minimap (a menu, a loading screen, a
cutscene); the report lists those apart.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

from tools.dev import scene
from tools.dev.run import run
from tools.yw4 import config

TASK = "minimap"
SHORT = 90  # frames: a gap up to this long is a suspect, longer is the game hiding the minimap


def load(folder: Path) -> tuple[list[dict], dict[tuple[int, str], dict]]:
    """The watch's events, and its rows by (frame, image): the counts joined with the picture's numbers."""
    events = []
    for f in sorted(folder.glob("perf_*.jsonl")):
        for line in f.read_text(errors="ignore").splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue  # a line cut short when the game was stopped
            if e.get("kind") == "minimap":
                events.append(e)
    rows: dict[tuple[int, str], dict] = {}
    for name in ("minimap_draws.csv", "minimap_pixels.csv"):
        p = folder / name
        if not p.is_file():
            continue
        with p.open(newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    key = (int(r["frame"]), r["target"])
                except (KeyError, ValueError, TypeError):
                    continue
                rows.setdefault(key, {}).update({k: v for k, v in r.items() if k not in ("frame", "target")})
    return events, rows


def gaps(events: list[dict]) -> list[dict]:
    """Each drop with its recovery: {frame, target, of (pixels or draws), frames (None: never came back), event}."""
    out, open_ = [], {}
    for e in events:
        what, target = e.get("what"), e.get("target")
        if what in ("back", "stays"):
            g = open_.pop((target, e.get("of")), None)
            if g:
                g["frames"] = e.get("frames")
                g["stayed"] = what == "stays"
        else:
            of = "pixels" if what == "pixels" else "draws"
            g = {"frame": e["frame"], "target": target, "of": of, "what": what, "frames": None, "stayed": False, "event": e}
            if what != "missing":  # a missing texture is logged once; it has no recovery
                open_[(target, of)] = g
            out.append(g)
    return out


def describe(g: dict) -> str:
    e = g["event"]
    if g["what"] == "pixels":
        return f"picture went blank: lit {e['lit'][0]} -> {e['lit'][1]}, opaque {e['opaque'][0]} -> {e['opaque'][1]} per mille"
    parts = [f"draws into it {e['draws'][0]} -> {e['draws'][1]}", f"on screen {e['shown'][0]} -> {e['shown'][1]}"]
    if e["missing"][1] > e["missing"][0]:
        parts.append(f"missing textures {e['missing'][0]} -> {e['missing'][1]}")
    if e.get("fresh"):
        parts.append(f"{e['fresh']} textures uploaded in that frame")
    return ", ".join(parts)


def table(rows: dict[tuple[int, str], dict], target: str, first: int, last: int) -> list[str]:
    cols = ("draws", "clears", "shown", "missing", "fresh", "verts", "lit_pm", "opaque_pm")
    out = ["      frame " + " ".join(f"{c.removesuffix('_pm'):>7}" for c in cols)]
    for f in range(first, last + 1):
        r = rows.get((f, target))
        if r:
            out.append(f"    {f:7} " + " ".join(f"{r.get(c, '-'):>7}" for c in cols))
    return out


def report(folder: Path) -> int:
    events, rows = load(folder)
    if not rows:
        print(f"no minimap watch files in {folder} (was the game run with YW4_MINIMAP_WATCH=1, and did it reach the world?)")
        return 1
    targets = sorted({t for _, t in rows})
    frames = sorted({f for f, _ in rows})
    print(f"{folder}\nwatched frames {frames[0]} to {frames[-1]}; minimap images: {', '.join(targets)}")
    for t in targets:
        mine = [r for (_, tt), r in rows.items() if tt == t]
        drawn = sorted(int(r["draws"]) for r in mine if "draws" in r and int(r["draws"]))
        lit = sorted(int(r["lit_pm"]) for r in mine if "lit_pm" in r)
        print(f"  {t}: drawn in {len(drawn)} frames" + (f", usually {drawn[len(drawn) // 2]} draws" if drawn else "") +
              (f", usually {lit[len(lit) // 2]} per mille lit" if lit else ", picture never read"))
    all_gaps = gaps(events)
    suspects = [g for g in all_gaps if g["frames"] is not None and g["frames"] <= SHORT and not g["stayed"]]
    other = [g for g in all_gaps if g not in suspects]
    print(f"\n{len(suspects)} short gaps (the bug, if any):" if suspects else "\nno short gap caught.")
    for g in suspects:
        print(f"  frame {g['frame']}, image {g['target']}, {g['frames']} frames: {describe(g)}")
        print("\n".join(table(rows, g["target"], g["frame"] - 3, g["frame"] + g["frames"] + 2)))
        files = sorted(p.name for p in folder.glob(f"minimap_{g['frame']:06d}_*"))
        if files:
            print("    files: " + ", ".join(files))
    if other:
        print(f"\n{len(other)} other changes (long: a menu, a loading screen, a cutscene; or still open at the end):")
        for g in other[:30]:
            how = "to the end of the run" if g["frames"] is None else f"{g['frames']} frames"
            print(f"  frame {g['frame']}, image {g['target']}, {how}: {describe(g)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="a scene of scenes.toml with the watch on, then the report")
    r.add_argument("scene")
    r.add_argument("--settings", default="owner", help="none (30 frames/s), owner (default: a copy of theirs), or a file")
    r.add_argument("--frames", type=int)
    r.add_argument("--seconds", type=int)
    r.add_argument("env", nargs="*", help="ENV=value for the game")
    p = sub.add_parser("play", help="the owner plays with the watch on until they close the game, then the report")
    p.add_argument("--save", default="save_base_lategame", help="folder in <work>/testsaves (copied first)")
    p.add_argument("--replay", default="load_save.txt", help="file in <work>/inputs that gets into the save")
    p.add_argument("env", nargs="*", help="ENV=value for the game")
    t = sub.add_parser("report", help="the report of a folder again")
    t.add_argument("folder", nargs="?", type=Path)
    a = ap.parse_args(argv)
    cfg = config.load()
    task_dir = cfg.work_dir / f"shots_{TASK}"

    if a.cmd == "report":
        folder = a.folder
        if not folder:
            done = sorted((d for d in task_dir.glob("*") if (d / "minimap_draws.csv").is_file()),
                          key=lambda d: (d / "minimap_draws.csv").stat().st_mtime)
            if not done:
                print(f"nothing in {task_dir} yet: run or play first")
                return 1
            folder = done[-1]
        return report(folder)

    if a.cmd == "run":
        out = task_dir / a.scene
        args = [a.scene, "--task", TASK, "--settings", a.settings, "YW4_MINIMAP_WATCH=1", f"YW4_MINIMAP_DIR={out}", *a.env]
        if a.frames:
            args += ["--frames", str(a.frames)]
        if a.seconds:
            args += ["--seconds", str(a.seconds)]
        code = scene.main(args)
        if code == 2:  # refused before starting
            return code
        print()
        return report(out) or code

    out = task_dir / "play"
    save = cfg.work_dir / "testsaves" / a.save
    replay = cfg.work_dir / "inputs" / a.replay
    settings = cfg.work_dir / "build" / "settings.ini"
    missing = [str(x) for x in (save, replay, settings) if not x.exists()]
    if missing:
        print("not started: missing " + ", ".join(missing), file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    for f in out.glob("*"):  # the last session's files: one session per folder
        if f.is_file():
            f.unlink()
    print(f"play from {a.save}; close the game when the minimap has gone blank a few times. Files in {out}", flush=True)
    env = [f"YW4_SETTINGS_FILE={settings}", "YW4_MOUSE=1", "YW4_AUDIO=1", "YW4_MINIMAP_WATCH=1", f"YW4_MINIMAP_DIR={out}",
           f"YW4_PERF_DIR={out}", *a.env]
    run(cfg, save, replay, 0, False, env, None, None, out / "run.log")
    print()
    return report(out)


if __name__ == "__main__":
    sys.exit(main())
