"""Main-view map from one run's log: which engine function draws into which target.
    python -m tools.yw4.mainview <log with YW4_TRACE_DRAW_CALLERS=1 and YW4_GPU_TRACE=n>
Part 1: per distinct draw caller, the function that issued the draw (third stack frame,
named f_<start> from work/recomp/func_table.c), its shaders and target.
Part 2: the traced draws in order, grouped into passes (consecutive draws with the same colour and
depth target addresses), each with its draw count, target, shaders and the issuing functions."""
import bisect
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repo
starts = sorted(int(m.group(1), 16) for m in re.finditer(r"\{\s*0x([0-9a-fA-F]+),",
                                                           (ROOT / "work/recomp/func_table.c").read_text())
                if int(m.group(1), 16) < 0x1100000)


def func_of(off):
    i = bisect.bisect_right(starts, off) - 1
    return f"f_{starts[i]:x}" if i >= 0 else f"?{off:x}"


log = Path(sys.argv[1]).read_text(errors="ignore").splitlines()

# part 1: callers
callers = {}  # (vs, fs, target) -> set of issuing functions
i = 0
while i < len(log):
    m = re.search(r"draw caller: vs (\w+) fs (\w+) target (\S+) \(([^)]*)\)", log[i])
    if m:
        frames = []
        j = i + 1
        while j < len(log) and log[j].startswith("       "):
            frames.append(log[j].strip())
            j += 1
        # the chain after the two NVN wrapper frames, up to the render-job dispatcher (main+0x979850)
        chain = []
        for f in frames[2:]:
            if not f.startswith("main+") or f == "main+0x979850":
                break
            chain.append(func_of(int(f[7:], 16) - 4))
        fn = " < ".join(chain[:5]) or "?"
        callers.setdefault((m.group(1), m.group(2), m.group(3)), set()).add(fn)
        i = j
    else:
        i += 1

# part 2: traced draws, grouped by target
draws = []
cur = None
for ln in log:
    m = re.search(r"gpu draw: vs (\w+) fs (\w+) prim \d+ count (\d+) target (\S+) \(([^)]*)\).*blend (\S+)", ln)
    if m:
        cur = {"vs": m.group(1), "fs": m.group(2), "count": int(m.group(3)), "dims": m.group(4), "desc": m.group(5),
               "blend": m.group(6), "colour": [], "depth": None}
        draws.append(cur)
        continue
    if cur is None:
        continue
    m = re.search(r"colour target (\d+) at (0x\w+).*format (0x\w+)", ln)
    if m:
        cur["colour"].append(f"{m.group(2)}:{m.group(3)}")
    m = re.search(r"depth target at (0x\w+).*format (0x\w+)", ln)
    if m:
        cur["depth"] = f"{m.group(1)}:{m.group(2)}"

passes = []
for d in draws:
    key = (tuple(d["colour"]), d["depth"], d["dims"])
    if not passes or passes[-1]["key"] != key:
        passes.append({"key": key, "draws": []})
    passes[-1]["draws"].append(d)

print(f"{len(callers)} distinct callers, {len(draws)} traced draws, {len(passes)} passes\n")
for n, p in enumerate(passes):
    ds = p["draws"]
    fns = Counter()
    for d in ds:
        for f in callers.get((d["vs"], d["fs"], d["dims"]), {"?"}):
            fns[f] += 1
    progs = Counter(f"{d['vs'][:6]}/{d['fs'][:6]}" for d in ds)
    col, dep, dims = p["key"]
    print(f"pass {n:3}: {len(ds):4} draws  {dims:10} {ds[0]['desc']:24} colour {' '.join(c.split(':')[1] for c in col) or '-':18}"
          f" depth {dep.split(':')[1] if dep else '-':5}  blend {Counter(d['blend'] for d in ds).most_common(1)[0][0]}")
    print(f"           by {', '.join(f'{f} x{c}' for f, c in fns.most_common(3))}")
    print(f"           programs {', '.join(f'{k} x{c}' for k, c in progs.most_common(4))}{' ...' if len(progs) > 4 else ''}")
