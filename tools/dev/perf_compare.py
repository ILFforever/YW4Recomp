"""Performance of two settings, alternating runs of one replay; averages from the logs' whole-run summaries.

    python -m tools.dev.perf_compare --a YW4_SHADOWS=game --b YW4_SHADOWS=native
                                     [--pairs 3] [--save DIR] [--replay FILE] [--seconds N]

Run-to-run noise is about +-1 ms of game work: compare the averages over all pairs, never one run, and
make sure nothing else is running (a forgotten game spoiled a whole comparison once).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from tools.dev.run import run
from tools.yw4 import config


def summary(log: Path) -> dict[str, float]:
    text = log.read_text(errors="ignore")
    i = text.rfind("perf, whole run")
    if i < 0:
        return {}
    part = text[i:i + 2000]
    out = {}
    for key, label in (("work", "game work ms"), ("gpu", "GPU busy ms")):
        m = re.search(label + r": avg ([\d.]+)\s+p50 [\d.]+\s+p95 ([\d.]+)", part)
        if m:
            out[key], out[key + "_p95"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"process CPU: (\d+)%", part)
    if m:
        out["cpu"] = float(m.group(1))
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--a", nargs="+", required=True, help="ENV=value settings of the first")
    ap.add_argument("--b", nargs="+", required=True, help="... and of the second")
    ap.add_argument("--pairs", type=int, default=3)
    ap.add_argument("--save", type=Path, default=Path("work/testsaves/save_base_walk"))
    ap.add_argument("--replay", type=Path, default=Path("work/inputs/walk.txt"))
    ap.add_argument("--seconds", type=int, default=600)
    a = ap.parse_args(argv)
    cfg = config.load()
    results = {"a": [], "b": []}
    for i in range(a.pairs):
        for side in ("a", "b"):
            log = run(cfg, a.save, a.replay, a.seconds, True, getattr(a, side))
            s = summary(log)
            results[side].append(s)
            print(f"{side} run {i + 1}: {s}  ({log})")
    for side in ("a", "b"):
        rs = [r for r in results[side] if r]
        if rs:
            avg = {k: sum(r.get(k, 0) for r in rs) / len(rs) for k in rs[0]}
            print(side, " ".join(f"{k} {v:.2f}" for k, v in avg.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
