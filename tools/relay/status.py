"""Our relay servers at a glance: is the service up, since when, and its last summary line (rooms, players,
traffic, refusals; relay.py prints one every 5 minutes), over SSH with the servers' key.

    python tools/relay/status.py [--key ~/.ssh/yw4_relay]

Standard library only. The servers are those the game knows (runtime/src/settings.c); add one with --server.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

SERVERS = {"US East": "129.153.179.140", "Singapore": "161.118.234.155"}
REMOTE = ("systemctl is-active yw4-relay; systemctl show yw4-relay -p ActiveEnterTimestamp --value; "
          "journalctl -u yw4-relay --since -15min --no-pager -o cat | grep -E 'status:|refused|dropped' | tail -3")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", default=str(Path.home() / ".ssh" / "yw4_relay"))
    ap.add_argument("--server", action="append", default=[], help="name=address, more than once for more")
    args = ap.parse_args()
    servers = dict(SERVERS)
    for s in args.server:
        name, _, addr = s.partition("=")
        servers[name] = addr
    bad = 0
    for name, addr in servers.items():
        try:
            out = subprocess.run(["ssh", "-i", args.key, "-o", "ConnectTimeout=8", "-o", "BatchMode=yes",
                                  f"ubuntu@{addr}", REMOTE], capture_output=True, text=True, timeout=30)
            lines = out.stdout.strip().splitlines()
        except (OSError, subprocess.TimeoutExpired) as e:
            lines, out = [], None
            print(f"{name}: can't reach it ({e})")
            bad += 1
            continue
        state = lines[0] if lines else "unknown"
        print(f"{name}: {state}, running since {lines[1] if len(lines) > 1 else '?'}")
        for line in lines[2:]:
            print(f"    {line}")
        if len(lines) <= 2:
            print("    (no summary in the last 15 minutes yet)")
        if state != "active":
            bad += 1
            if out is not None and out.stderr.strip():
                print(f"    {out.stderr.strip()}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
