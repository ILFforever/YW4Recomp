"""Summarize how main uses NVN, from the Ghidra NvnCallers trace.

    python -m tools.yw4.nvn_report

Needs a seeded Ghidra project (python -m tools.yw4.ghidra --seed). Runs
tools/ghidra/NvnCallers.java, then adds what Ghidra doesn't track:

- NVN entry points live in a table of pointers filled at startup. A function
  that reads a slot is that entry point's *wrapper*.
- Wrappers for draws are mostly not called directly: their addresses are built
  in code (ADRP+ADD) and stored, e.g. into render commands. Functions that take
  a wrapper's address are its *producers*, which is where the engine's render
  layer starts.
- Functions that sit in an RTTI vtable get their class name.

Writes <work_dir>/recon/nvn.md (and nvn.json).
"""

from __future__ import annotations

import bisect
import collections
import json
import re
import struct
import sys

from . import config, ghidra, recon, reloc, symbols

DRAW_WORDS = ("Draw", "Dispatch", "QueueSubmit", "PresentTexture", "BindProgram")


def adrp_add_targets(words: tuple[int, ...]):
    """Yield (pc, target) for every ADRP Xd followed within 5 instructions by ADD Xd', Xd, #imm."""
    for i, w in enumerate(words):
        if w & 0x9F000000 != 0x90000000:
            continue
        rd = w & 31
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        page = ((i * 4) & ~0xFFF) + (imm << 12)
        for j in range(1, 6):
            if i + j >= len(words):
                break
            w2 = words[i + j]
            if w2 & 0xFFC00000 == 0x91000000 and (w2 >> 5) & 31 == rd:
                yield i * 4, page + ((w2 >> 10) & 0xFFF)
                break


def vtable_members(image: bytes, info: dict) -> dict[int, list[tuple[str, int]]]:
    """function entry -> [(demangled class, vtable index)] for primary RTTI vtables."""
    text = info["segments"]["text"]["size"]
    names = recon.dynsym_names(image, info)
    internal, external = reloc.pointer_map(reloc.read_relocs(image))
    typeinfo = {s: recon.c_string(image, internal[s + 8]) for s, (sym, _) in external.items()
                if names[sym] in recon.TYPEINFO_VTABLES and s + 8 in internal}
    dm = symbols.demangle(["_ZTS" + n for n in set(typeinfo.values())])
    members: dict[int, list[tuple[str, int]]] = collections.defaultdict(list)
    for slot, target in internal.items():
        if target not in typeinfo or struct.unpack_from("<q", image, slot - 8)[0] != 0:
            continue
        cls = dm["_ZTS" + typeinfo[target]].removeprefix("typeinfo name for ")
        pos, k = slot + 8, 0
        while pos in internal and internal[pos] < text:
            members[internal[pos]].append((cls, k))
            pos += 8
            k += 1
    return members


def main() -> int:
    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    image, mods = recon.load_main(cfg)
    info = mods["main"]
    gdir = cfg.work_dir / "ghidra"

    names_file = gdir / "nvn_names.txt"
    names_file.write_text("".join(f"{m.start():#x} {m.group().decode()}\n" for m in
                                  re.finditer(rb"(?<=\0)nvn[A-Z][A-Za-z0-9_]+(?=\0)", image)))
    trace_file = gdir / "nvn_callers.json"
    if ghidra.main(["--script", "NvnCallers.java", str(names_file), str(trace_file)]) != 0:
        return 1
    entries = json.loads(trace_file.read_text())["entries"]

    text = info["segments"]["text"]["size"]
    words = struct.unpack(f"<{text // 4}I", image[:text // 4 * 4])
    funcs = sorted((f["entry"], f["entry"] + f["size"])
                   for f in json.loads((gdir / "main.functions.json").read_text())["functions"])
    starts = [f[0] for f in funcs]

    def owner(addr: int) -> int | None:
        j = bisect.bisect_right(starts, addr) - 1
        return funcs[j][0] if j >= 0 and addr < funcs[j][1] else None

    wrappers: dict[int, set[str]] = collections.defaultdict(set)
    for e in entries:
        for entry, _ in e["callers"]:
            wrappers[entry].add(e["name"])
    producers: dict[int, set[str]] = collections.defaultdict(set)
    for pc, target in adrp_add_targets(words):
        if target in wrappers:
            o = owner(pc)
            if o is not None:
                producers[o] |= wrappers[target]
    members = vtable_members(image, info)

    def where(fn: int) -> str:
        return ", ".join(f"`{c}`[{k}]" for c, k in members.get(fn, [])[:2]) or "-"

    used = [e for e in entries if e["callers"]]
    by_object = collections.Counter(re.match(r"nvn([A-Z][a-z]+(?:Builder)?)", e["name"]).group(1) for e in used)
    lines = [
        f"- NVN names in main: {len(entries)}; table slots resolved: {sum(e['slot'] is not None for e in entries)}",
        f"- NVN functions actually read by code: **{len(used)}**",
        f"- Wrapper functions: {len(wrappers)}; functions that take a wrapper's address (producers): "
        f"**{len(producers)}**",
        "", "Used functions by object:", "", "| Object | Used |", "|---|---|",
    ]
    lines += [f"| {k} | {v} |" for k, v in by_object.most_common()]
    lines += ["", "Draw / dispatch / submit / present entry points:", "",
              "| NVN function | Wrapper | Wrapper's class | Producers |", "|---|---|---|---|"]
    for e in entries:
        if not any(k in e["name"] for k in DRAW_WORDS):
            continue
        for entry, _ in e["callers"]:
            n = sum(1 for p in producers.values() if e["name"] in p)
            lines.append(f"| `{e['name']}` | `{entry:#x}` | {where(entry)} | {n} |")
    lines += ["", "Largest producers:", "", "| Function | Class | NVN wrappers referenced |", "|---|---|---|"]
    for fn, ws in sorted(producers.items(), key=lambda kv: -len(kv[1]))[:25]:
        short = sorted(w.removeprefix("nvn") for w in ws)
        lines.append(f"| `{fn:#x}` | {where(fn)} | {len(ws)}: {', '.join(short[:5])}{' ...' if len(short) > 5 else ''} |")

    out = cfg.work_dir / "recon"
    out.mkdir(parents=True, exist_ok=True)
    (out / "nvn.md").write_text("\n".join(lines) + "\n")
    (out / "nvn.json").write_text(json.dumps({
        "used": sorted(e["name"] for e in used),
        "wrappers": {hex(k): sorted(v) for k, v in wrappers.items()},
        "producers": {hex(k): sorted(v) for k, v in producers.items()},
    }, indent=1))
    print(f"used NVN functions: {len(used)}  wrappers: {len(wrappers)}  producers: {len(producers)}")
    print(f"wrote {out / 'nvn.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
