"""Name the samples of a YW4_PROFILE=1 run (runtime/src/profile.c).

    python -m tools.yw4.profile [work/perf/profile_<time>.txt] [--top N]

Reads the newest profile by default, maps each yw4.exe offset to the function containing it
with llvm-nm (from the configured llvm-mingw), and prints the share of samples per group
(recompiled main / sdk code, runtime, outside = system DLLs and waits) and the top functions.
f_<hex> is main+<hex>, s_<hex> is sdk+<hex>.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import subprocess
import sys
from pathlib import Path

from recomp import check

IMAGE_BASE = 0x140000000  # the executable's preferred base; profile offsets are relative to it


def symbols(exe: Path) -> tuple[list[int], list[str]]:
    nm = Path(check.host_cc()).with_name("llvm-nm.exe" if sys.platform == "win32" else "llvm-nm")
    out = subprocess.run([str(nm), "--defined-only", str(exe)], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "Tt":
            syms.append((int(parts[0], 16) - IMAGE_BASE, parts[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def group(name: str) -> str:
    if name.startswith("f_"):
        return "game code (main)"
    if name.startswith("s_"):
        return "game code (sdk)"
    if name.startswith(("gpu_", "nvn", "tegra_", "vk", "image_of", "pixel")):
        return "runtime: graphics"
    if name.startswith(("hle_", "musl_")) or name in ("memcpy", "memset", "memmove"):
        return "runtime: C library / HLE"
    return "runtime: other"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("profile", nargs="?")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--exe", default="work/build/yw4.exe")
    args = ap.parse_args()
    path = Path(args.profile) if args.profile else max(Path("work/perf").glob("profile_*.txt"), key=lambda p: p.stat().st_mtime)
    addrs, names = symbols(Path(args.exe))
    total = outside = 0
    per_fn: collections.Counter[str] = collections.Counter()
    per_via: collections.Counter[str] = collections.Counter()  # system DLL time, by the yw4.exe caller
    for line in path.read_text().splitlines():
        if line.startswith("#"):
            parts = line.split()
            total, outside = int(parts[2]), int(parts[4])
            continue
        off, n = line.split()
        key = int(off, 16)
        via = key >> 63  # the sample was in a system DLL called from here
        key &= (1 << 63) - 1
        i = bisect.bisect_right(addrs, key) - 1
        name = names[i] if i >= 0 else f"exe+{key:x}"
        if via:
            per_via[name] += int(n)
        else:
            per_fn[name] += int(n)
    print(f"{path}: {total} samples")
    groups: collections.Counter[str] = collections.Counter()
    for fn, n in per_fn.items():
        groups[group(fn)] += n
    groups["outside yw4.exe (system DLLs, waits)"] = outside
    for g, n in groups.most_common():
        print(f"  {100 * n / total:5.1f}%  {g}")
    print(f"top {args.top} functions:")
    for fn, n in per_fn.most_common(args.top):
        print(f"  {100 * n / total:5.1f}%  {fn}")
    print(f"time in system DLLs, by the yw4.exe function that called them (top {args.top}):")
    for fn, n in per_via.most_common(args.top):
        print(f"  {100 * n / total:5.1f}%  {fn}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
