"""A steady, low-priority CPU load, so an always-on relay server doesn't count as idle (decision 0082).

Oracle Cloud's Always Free machines can be reclaimed when they look idle for a week: CPU use under about 20%,
with low network and memory use too (Oracle's docs; the owner chose this load over the Pay As You Go upgrade,
2026-11-19). A relay waiting for players uses almost no CPU, so this keeps every CPU busy for a set share of the
time: busy for `percent` of each 100 ms, asleep for the rest.

It steps aside while people play (owner, 2026-11-20): the machine's network traffic (/proc/net/dev, every 2 s) over
16 KB/s (a 2-player Blasters mission is about 240 KB/s at the server; a trade or a room waiting, a few KB/s) pauses
the load at once, and it starts again after 30 s under that. On the US East machine (1/8 of a CPU) the load took
from the same small CPU allowance the relay needs; players busy the machine on their own anyway.

It never takes CPU from the relay otherwise: setup_server.sh runs it with the idle scheduling class (Linux gives it
only time no other program wants), and it measures time in wall clock, so when the relay needs the CPU this simply
gets less done. One process per CPU, because the provider's figure is the machine's average.

    python3 keepalive.py --percent 25        (Python's standard library only)
"""
from __future__ import annotations

import argparse
import multiprocessing
import os
import time

PERIOD = 0.1          # seconds: short, so the load is even rather than in bursts a monitor could average away
CHECK = 2.0           # seconds between looks at the network
PLAYING_BPS = 16_000  # bytes a second in and out together: people are playing
RESUME_AFTER = 30.0   # seconds under that before the load comes back


def burn(percent: float, paused) -> None:
    busy = PERIOD * percent / 100.0
    while True:
        if paused.value:
            time.sleep(0.5)
            continue
        start = time.monotonic()
        x = 0
        while time.monotonic() - start < busy:  # busy for its share of the period
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        rest = PERIOD - (time.monotonic() - start)
        if rest > 0:
            time.sleep(rest)


def net_bytes() -> int | None:
    """Bytes received and sent on every interface but loopback, since boot (None if /proc can't be read)."""
    try:
        total = 0
        with open("/proc/net/dev") as f:
            for line in f.readlines()[2:]:
                name, _, rest = line.partition(":")
                if name.strip() == "lo":
                    continue
                fields = rest.split()
                total += int(fields[0]) + int(fields[8])
        return total
    except (OSError, ValueError, IndexError):
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--percent", type=float, default=25.0, help="share of each CPU kept busy (default 25)")
    args = ap.parse_args()
    percent = max(1.0, min(args.percent, 90.0))
    cpus = os.cpu_count() or 1
    print(f"keepalive: {percent:.0f}% of each of {cpus} CPU(s), at the lowest priority; paused while people play "
          f"(over {PLAYING_BPS // 1000} KB/s)", flush=True)
    paused = multiprocessing.Value("b", 0, lock=False)
    workers = [multiprocessing.Process(target=burn, args=(percent, paused), daemon=True) for _ in range(cpus)]
    for w in workers:
        w.start()
    last, last_t, quiet_since = net_bytes(), time.monotonic(), time.monotonic()
    while True:
        time.sleep(CHECK)
        now_b, now_t = net_bytes(), time.monotonic()
        if now_b is None or last is None:  # can't tell: keep the load as it is
            last, last_t = now_b, now_t
            continue
        rate = (now_b - last) / max(now_t - last_t, 0.001)
        last, last_t = now_b, now_t
        if rate > PLAYING_BPS:
            quiet_since = now_t
            if not paused.value:
                paused.value = 1
                print(f"keepalive: people are playing ({rate / 1000:.0f} KB/s): paused", flush=True)
        elif paused.value and now_t - quiet_since >= RESUME_AFTER:
            paused.value = 0
            print(f"keepalive: quiet for {RESUME_AFTER:.0f} s: running again", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
