"""A steady, low-priority CPU load, so an always-on relay server doesn't count as idle (decision 0082).

Oracle Cloud's Always Free machines can be reclaimed when they look idle for a week: CPU use under about 20%,
with low network and memory use too (Oracle's docs; the owner chose this load over the Pay As You Go upgrade,
2026-11-19). A relay waiting for players uses almost no CPU, so this keeps every CPU busy for a set share of the
time: busy for `percent` of each 100 ms, asleep for the rest.

It never takes CPU from the relay: setup_server.sh runs it with the idle scheduling class (Linux gives it only
time no other program wants), and it measures time in wall clock, so when the relay needs the CPU this simply
gets less done. One process per CPU, because the provider's figure is the machine's average.

    python3 keepalive.py --percent 25        (Python's standard library only)
"""
from __future__ import annotations

import argparse
import multiprocessing
import os
import time

PERIOD = 0.1  # seconds: short, so the load is even rather than in bursts a monitor could average away


def burn(percent: float) -> None:
    busy = PERIOD * percent / 100.0
    while True:
        start = time.monotonic()
        x = 0
        while time.monotonic() - start < busy:  # busy for its share of the period
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        rest = PERIOD - (time.monotonic() - start)
        if rest > 0:
            time.sleep(rest)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--percent", type=float, default=25.0, help="share of each CPU kept busy (default 25)")
    args = ap.parse_args()
    percent = max(1.0, min(args.percent, 90.0))
    cpus = os.cpu_count() or 1
    print(f"keepalive: {percent:.0f}% of each of {cpus} CPU(s), at the lowest priority", flush=True)
    workers = [multiprocessing.Process(target=burn, args=(percent,), daemon=True) for _ in range(cpus)]
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
