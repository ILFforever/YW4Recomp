"""Where the frame time goes: GPU time per pass and CPU time per thread for every scene of the `bench` group.

    python -m tools.dev.bench                    # run every bench scene twice (GPU, CPU), then write the results
    python -m tools.dev.bench lag school         # only those scenes (the others' results are kept)
    python -m tools.dev.bench --parse            # write the results again from the last runs

Each scene runs twice (tools.dev.scene, so the usual checks apply):
- GPU: the owner's settings (a copy), YW4_GPU_PASSES=1; each pass's milliseconds per frame are averaged over the
  run's second half. The bamboo forest runs a third time with YW4_MASK_BREAKDOWN=1 (the shadow mask in four forms).
- CPU: no settings file (1600x900, no anti-aliasing) and YW4_FPS=0, so the GPU isn't the limit, with
  YW4_PROFILE=hitch and a 0 ms threshold: every game thread sampled each millisecond from frame 3000. A sample in a
  wait (fences, condition variables, the GPU worker idling, presents) counts as idle; the rest is split per thread
  by kind of code (functions named with llvm-nm, runtime functions grouped by module from docs/advanced/code-map.js).
  The profile is written at exit, so these runs end at a frame (--frames), not on a timer.

Writes work/bench/bench.json (everything), work/bench/*.csv (one table each) and docs/advanced/bench.js (the
site's snapshot: frame rates, GPU time per pass and the shadow mask's split, with the date, game version, GPU and
settings; nothing per thread or per function). Run it with nothing else open: a second game spoils the timings,
and tools.dev.scene refuses to start while one runs.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import datetime
import json
import re
import statistics
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "work"
OUT = WORK / "bench"
SITE = ROOT / "docs" / "advanced" / "bench.js"
SCENES_TOML = ROOT / "tools" / "dev" / "scenes.toml"
CPU_FROM = 3000
# the bench scenes' names on the site and in the tables (scenes.toml's `about` is for us)
SHORT = {"busy": "Busy town spot", "lag": "Evening street", "lag2": "Town street 2", "lag3": "Town street 3",
         "lag4": "Town street 4", "lategame": "Bamboo forest", "yokaiworld": "Yo-kai World street",
         "walk": "Town walk (moving)", "school": "Schoolyard", "forest": "Forest cutscene", "snowbridge": "Snow bridge",
         "fence": "Fence", "pole": "Pole", "cutscene": "Cutscene", "diary": "Owner's save (Nov 17)"}
CPU_FRAMES = {"walk": 10000}  # the town walk's replay is longer; the rest 7000
MASK_SCENE = "lategame"


def bench_scenes() -> tuple[list[str], dict[str, str]]:
    t = tomllib.loads(SCENES_TOML.read_text(encoding="utf-8"))
    return t["groups"]["bench"], {k: v.get("about", k) for k, v in t.items() if isinstance(v, dict) and "save" in v}


def scene(task: str, name: str, *args: str) -> bool:
    cmd = [sys.executable, "-m", "tools.dev.scene", name, "--task", task, *args]
    print(">>", name, task, " ".join(args), flush=True)
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout + r.stderr).strip()
    print("  " + "\n  ".join(out.splitlines()[-4:]), flush=True)
    if "not started" in out:
        print("bench: a run refused to start (above); stopping", flush=True)
        return False
    return True


def run(names: list[str]) -> bool:
    for s in names:
        if not scene("bench_gpu", s, "--settings", "owner", "YW4_GPU_PASSES=1"):
            return False
        if not scene("bench_cpu", s, "--frames", str(CPU_FRAMES.get(s, 7000)), "YW4_FPS=0", "YW4_PROFILE=hitch",
                     "YW4_PROFILE_HITCH_MS=0", f"YW4_PROFILE_FROM={CPU_FROM}"):
            return False
    if MASK_SCENE in names:
        return scene("bench_mask", MASK_SCENE, "--settings", "owner", "YW4_GPU_PASSES=1", "YW4_MASK_BREAKDOWN=1")
    return True


# ---- GPU passes

FS_NAMES: dict[str, str] = {}


def load_names() -> None:
    src = (ROOT / "runtime/src/gpu_draw.c").read_text(encoding="utf-8")
    for h, n in re.findall(r'\{0x([0-9a-f]{16})ull, ST_FRAG, "([a-z_0-9]+)', src):
        FS_NAMES[h] = n.replace("_", " ")
    # named in docs/frame.md
    FS_NAMES.update({"06d03ad703b4bee5": "shadow mask", "6b4575fb57b97548": "depth-only copy",
                     "17a09ecce84525a7": "occlusion boxes"})


def pass_label(rest: str) -> str:
    m = re.search(r"\s+vs ([0-9a-f]{16}) fs ([0-9a-f]{16})\s*$", rest)
    target = rest[: m.start()].strip() if m else rest.strip()
    fs = m.group(2) if m else ""
    if not fs or fs == "0" * 16:
        return target  # our own passes are named in the log
    if fs in FS_NAMES:
        return FS_NAMES[fs]
    if "240x135" in target:
        return "occlusion boxes"
    if re.match(r"(400x225|200x112|120x67)", target):
        return "bloom chain (game's)"
    if re.match(r"1600x900 f(17 1600x900 f37|63 1600x900 f17)", target):
        return "G-buffer (other materials)"
    if re.match(r"1600x900 f63 \+ depth 1600x900", target):
        return "scene colour (other materials)"
    return f"other ({target[:22]})"


def who(name: str) -> str:
    if name.startswith("DLSS"):
        return "NVIDIA DLSS"
    if name.startswith("our ") or name in ("shadow mask", "anti-aliasing: DLSS"):
        return "ours"
    return "game's"


def parse_gpu(log: Path) -> dict:
    blocks: list[dict] = []
    for ln in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"yw4: gpu passes: ([\d.]+) ms of GPU time per frame over (\d+) frames", ln)
        if m:
            blocks.append({"total": float(m.group(1)), "passes": []})
            continue
        m = re.match(r"yw4: gpu passes:\s+([\d.]+) ms\s+([\d.]+) passes \(\s*[\d.]+ ms each\)\s+([\d.]+) draws\s+[\d.]+"
                     r" restarts\s+[\d.]+ uploads\s+(\d+)k vertices\s+(.*)$", ln)
        if m and blocks:
            blocks[-1]["passes"].append({"name": pass_label(m.group(5)), "ms": float(m.group(1)),
                                         "draws": float(m.group(3)), "kverts": int(m.group(4))})
    use = blocks[len(blocks) // 2:] or blocks
    agg: dict[str, dict] = collections.defaultdict(lambda: {"ms": 0.0, "draws": 0.0, "kverts": 0.0})
    for b in use:
        for p in b["passes"]:
            a = agg[p["name"]]
            for k in ("ms", "draws", "kverts"):
                a[k] += p[k] / len(use)
    passes = sorted(({"name": k, "who": who(k), **v} for k, v in agg.items()), key=lambda p: -p["ms"])
    return {"total_ms": statistics.mean(b["total"] for b in use) if use else None, "passes": passes}


def parse_mask(log: Path) -> dict[str, float]:
    forms: dict[str, list[float]] = collections.defaultdict(list)
    for ln in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"gpu passes:.*\(\s*([\d.]+) ms each\).*shadow mask: (.+?)\s{2,}", ln)
        if m:
            forms[m.group(2)].append(float(m.group(1)))
    return {k: round(statistics.mean(v), 2) for k, v in forms.items()}


def summary(folder: Path) -> dict:
    newest = max(folder.glob("perf_*.txt"), key=lambda p: p.stat().st_mtime)  # a respawned run can leave two
    txt = newest.read_text(encoding="utf-8", errors="replace")

    def num(pat: str) -> float | None:
        m = re.search(pat, txt)
        return float(m.group(1)) if m else None

    m = re.search(r"picture now: (.*)", txt)
    return {"fps": num(r"([\d.]+) frames/s average"), "work_ms": num(r"game work ms: avg ([\d.]+)"),
            "gpu_busy_ms": num(r"GPU busy ms: avg ([\d.]+)"), "gpu_wait_ms": num(r"GPU waits avg ([\d.]+)"),
            "game_code_ms": num(r"game code avg ([\d.]+)"), "process_cpu_pct": num(r"process CPU: (\d+)%"),
            "picture": m.group(1).strip() if m else ""}


# ---- CPU profile

WAIT = re.compile(r"wait|sleep|cond_|park|futex|SleepConditionVariable|svc$|^worker$|^present_image$")


def code_map() -> tuple[dict[str, str], dict[str, str]]:
    t = (ROOT / "docs/advanced/code-map.js").read_text(encoding="utf-8")
    j = json.loads(t[t.index("{"): t.rindex("}") + 1])
    mods: dict[str, set] = collections.defaultdict(set)
    for f in j["funcs"]:
        mods[f["n"]].add(f["m"])
    return {k: next(iter(v)) for k, v in mods.items() if len(v) == 1}, {n["id"]: n["group"] for n in j["nodes"]}


def category(name: str, fn_mod: dict, groups: dict) -> tuple[str, str]:
    if name.startswith(("f_", "s_")):
        return "Game code (recompiled)", name
    mod = fn_mod.get(name)
    if mod in ("gpu", "gpu_draw", "gpu_queue", "gpu_pcache", "gpu_query", "vk", "window") or (mod or "").startswith("nvn"):
        return "Our graphics translation", f"{name} [{mod}]"
    if mod and groups.get(mod) == "passes":
        return "Our render passes (bones, shadows, G-buffer)", f"{name} [{mod}]"
    return "Our other runtime", f"{name} [{mod}]" if mod else name


def thread_roles(log: Path) -> dict[int, dict]:
    text = log.read_text(encoding="utf-8", errors="replace")
    out = {}
    for ln in text[text.rfind("threads, last 600 frames:"):].splitlines()[1:40]:
        m = re.match(r"\s+thread (\d+):\s+(\d+)% of a core.*?(\[(.*)\])?$", ln)
        if not m:
            break
        out[int(m.group(1))] = {"core_pct": int(m.group(2)), "role": m.group(4) or ""}
    return out


def thread_name(tid: int, role: str) -> str:
    if tid == 0:
        return "Our host threads (GPU worker, window)"
    if role == "presents":
        return "Main game thread"
    if role.startswith("submits"):
        return f"Render thread {tid}"
    return f"Game worker {tid}"


def parse_cpu(folder: Path, addrs, names, fn_mod, groups) -> dict | None:
    log = folder / "run.log"
    m = re.search(r"yw4: profile \((\d+) samples\) in (\S+)", log.read_text(encoding="utf-8", errors="replace"))
    if not m:
        return None
    roles = thread_roles(log)
    th: dict[int, dict] = collections.defaultdict(lambda: {"total": 0, "idle": 0, "cats": collections.Counter(),
                                                            "fns": collections.Counter(), "sys": collections.Counter()})
    for ln in (ROOT / m.group(2)).read_text().splitlines():
        if ln.startswith("#"):
            continue
        k, n = ln.split()
        key, n = int(k, 16), int(n)
        via, tid, off = key >> 63, (key >> 48) & 0x7FFF, key & ((1 << 48) - 1)  # profile.c: caller bit, thread tag
        i = bisect.bisect_right(addrs, off) - 1
        name = names[i] if i >= 0 else f"exe+{off:x}"
        t = th[tid]
        t["total"] += n
        if via and WAIT.search(name):
            t["idle"] += n
        elif via:
            t["sys"][name] += n
            t["cats"]["Windows / driver calls"] += n
        else:
            cat, label = category(name, fn_mod, groups)
            t["cats"][cat] += n
            t["fns"][label] += n
    threads = []
    for tid, t in th.items():
        role = roles.get(tid, {}).get("role", "")
        threads.append({"thread": thread_name(tid, role), "busy_pct": round(100 * (t["total"] - t["idle"]) / t["total"], 1),
                        "cats": {c: round(100 * v / t["total"], 2) for c, v in t["cats"].items()},
                        "top": [(f, round(100 * v / t["total"], 2)) for f, v in t["fns"].most_common(15)],
                        "sys": [(f, round(100 * v / t["total"], 2)) for f, v in t["sys"].most_common(8)]})
    return {"samples": int(m.group(1)), "threads": sorted(threads, key=lambda t: -t["busy_pct"])}


# ---- output

def gpu_name() -> str:
    for log in sorted((WORK / "shots_bench_gpu").glob("*/run.log")):
        m = re.search(r"yw4: window: Vulkan on (.+?) \(API", log.read_text(encoding="utf-8", errors="replace"))
        if m:
            return m.group(1)
    return ""


def write_csv(name: str, rows: list[dict]) -> None:
    if not rows:
        return
    with open(OUT / name, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def parse(names: list[str], about: dict[str, str]) -> None:
    load_names()
    sys.path.insert(0, str(ROOT))
    from tools.yw4.profile import symbols
    addrs, syms = symbols(WORK / "build" / ("yw4.exe" if sys.platform == "win32" else "yw4"))
    fn_mod, groups = code_map()
    res = {"date": datetime.date.today().isoformat(), "version": (ROOT / "VERSION").read_text().strip(),
           "gpu_name": gpu_name(), "scenes": {}, "mask": {}}
    for s in names:
        g, c = WORK / "shots_bench_gpu" / s, WORK / "shots_bench_cpu" / s
        e = {"about": SHORT.get(s, about.get(s, s))}
        if (g / "run.log").exists() and list(g.glob("perf_*.txt")):
            e["gpu"] = {"summary": summary(g), **parse_gpu(g / "run.log")}
        if (c / "run.log").exists() and list(c.glob("perf_*.txt")):
            e["cpu"] = {"summary": summary(c), "profile": parse_cpu(c, addrs, syms, fn_mod, groups)}
        if len(e) > 1:
            res["scenes"][s] = e
    mlog = WORK / "shots_bench_mask" / MASK_SCENE / "run.log"
    if mlog.exists():
        res["mask"] = parse_mask(mlog)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "bench.json").write_text(json.dumps(res, indent=1), encoding="utf-8")

    sc = res["scenes"]
    write_csv("scenes.csv", [{"scene": v["about"], **{f"{k}_4k": v["gpu"]["summary"][k] for k in ("fps", "gpu_busy_ms", "work_ms", "gpu_wait_ms")},
                              **{f"{k}_uncapped": (v.get("cpu") or {}).get("summary", {}).get(k) for k in ("fps", "work_ms", "game_code_ms", "process_cpu_pct")}}
                             for v in sc.values() if "gpu" in v])
    write_csv("gpu_passes.csv", [{"scene": v["about"], "pass": p["name"], "who": p["who"], "ms": round(p["ms"], 3),
                                  "draws": round(p["draws"], 1), "kvertices": round(p["kverts"])}
                                 for v in sc.values() if "gpu" in v for p in v["gpu"]["passes"]])
    write_csv("shadow_mask.csv", [{"form": k, "ms": v} for k, v in res["mask"].items()])
    write_csv("cpu_threads.csv", [{"scene": v["about"], "thread": t["thread"], "busy_pct": t["busy_pct"], "category": cat, "pct_of_core": pct}
                                  for v in sc.values() if (v.get("cpu") or {}).get("profile")
                                  for t in v["cpu"]["profile"]["threads"] if t["busy_pct"] >= 3 for cat, pct in t["cats"].items()])
    fns: dict[str, list] = collections.defaultdict(list)
    ncpu = sum(1 for v in sc.values() if (v.get("cpu") or {}).get("profile"))
    for v in sc.values():
        for t in ((v.get("cpu") or {}).get("profile") or {}).get("threads", []):
            for f, pct in t["top"]:
                fns[f].append((pct, t["thread"]))
    write_csv("cpu_functions.csv", sorted(({"function": f, "pct_of_core": round(sum(p for p, _ in v) / max(ncpu, 1), 2),
                                            "mostly_on": collections.Counter(t for _, t in v).most_common(1)[0][0]}
                                           for f, v in fns.items()), key=lambda r: -r["pct_of_core"])[:40])

    # the site's snapshot: no per-thread or per-function detail
    def top_passes(passes: list[dict], n: int = 10) -> list[dict]:
        top = [{"name": p["name"], "who": p["who"], "ms": round(p["ms"], 2)} for p in passes[:n]]
        rest = sum(p["ms"] for p in passes[n:])
        return top + ([{"name": f"{len(passes) - n} smaller passes", "who": "", "ms": round(rest, 2)}] if rest else [])

    first = next((v["gpu"]["summary"]["picture"] for v in sc.values() if "gpu" in v), "")
    site = {"date": res["date"], "version": res["version"], "gpu": res["gpu_name"], "settings": first,
            "scenes": [{"name": v["about"], "fps": v["gpu"]["summary"]["fps"], "gpu_ms": v["gpu"]["summary"]["gpu_busy_ms"],
                        "fps_uncapped": (v.get("cpu") or {}).get("summary", {}).get("fps"), "passes": top_passes(v["gpu"]["passes"])}
                       for v in sc.values() if "gpu" in v],
            "shadow_mask": res["mask"]}
    SITE.write_text("// Written by python -m tools.dev.bench (a snapshot; see its date). Read by docs/advanced/index.html.\n"
                    "window.BENCH = " + json.dumps(site, ensure_ascii=False) + ";\n", encoding="utf-8")
    print(f"bench: {len(sc)} scenes -> {OUT / 'bench.json'}, {SITE.relative_to(ROOT)}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("scenes", nargs="*", help="bench scenes to run (default: the whole bench group)")
    ap.add_argument("--parse", action="store_true", help="only write the results from the last runs")
    a = ap.parse_args(argv)
    group, about = bench_scenes()
    if not a.parse and not run(a.scenes or group):
        return 1
    parse(group, about)
    return 0


if __name__ == "__main__":
    sys.exit(main())
