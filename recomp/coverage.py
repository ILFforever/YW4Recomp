"""How much of main's code the lifter can translate.

    python -m recomp.coverage

Walks every 4-byte word in .text (only inside functions from the Ghidra
export, so padding and data islands don't count) and reports:
- instructions handled by a lifter form, per form
- unhandled instructions by top-level ARM64 encoding group, and the most
  common unhandled sub-groups, to decide what to implement next
Counts only, no game data is written.
"""

from __future__ import annotations

import collections
import json
import struct
import sys

from tools.yw4 import config, recon

from . import lift


def group(enc: int) -> str:
    """Top-level ARM64 encoding group (Arm ARM C4.1, op0 = bits 28:25)."""
    op0 = (enc >> 25) & 0xF
    if op0 in (0b1000, 0b1001):
        return "data processing (immediate)"
    if op0 in (0b1010, 0b1011):
        return "branches / exceptions / system"
    if op0 & 0b0101 == 0b0100:
        return "loads and stores"
    if op0 & 0b0111 == 0b0101:
        return "data processing (register)"
    if op0 & 0b0111 == 0b0111:
        return "SIMD and floating point"
    return "unallocated / reserved"


def subgroup(enc: int) -> str:
    """Rough finer grouping for the unhandled list (top 10 bits, with a readable hint)."""
    g = group(enc)
    if g == "branches / exceptions / system":
        if enc & 0x7C000000 == 0x14000000:
            return "B / BL"
        if enc & 0xFE000000 == 0xD6000000:
            return "BR / BLR / RET"
        if enc & 0x7E000000 == 0x34000000:
            return "CBZ / CBNZ"
        if enc & 0x7E000000 == 0x36000000:
            return "TBZ / TBNZ"
        if enc & 0xFF000010 == 0x54000000:
            return "B.cond"
        if enc & 0xFFC00000 == 0xD5000000:
            return "system (MRS/MSR/barriers/hints)"
        return "exceptions / other"
    if g == "loads and stores":
        if enc & 0x3B000000 == 0x39000000:
            return "load/store unsigned offset"
        if enc & 0x3A000000 == 0x28000000:
            return "load/store pair"
        if enc & 0x3B200C00 == 0x38200800:
            return "load/store register offset"
        if enc & 0x3B200000 == 0x38000000:
            return "load/store unscaled / pre / post index"
        if enc & 0x3B000000 == 0x18000000:
            return "load literal"
        if enc & 0x3F000000 == 0x08000000:
            return "exclusive / acquire / release"
        return "SIMD load/store and other"
    if g == "data processing (immediate)" and enc & 0x1F000000 == 0x10000000:
        return "ADR / ADRP"
    return g


def main() -> int:
    try:
        cfg = config.load()
        image, mods = recon.load_main(cfg)
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    funcs = json.loads((cfg.work_dir / "ghidra" / "main.functions.json").read_text())["functions"]
    text_size = mods["main"]["segments"]["text"]["size"]

    covered: set[int] = set()
    for f in funcs:
        for lo, hi in f.get("ranges", [[f["entry"], f["entry"] + f["size"]]]):
            covered.update(range(lo & ~3, min(hi, text_size), 4))

    handled = collections.Counter()
    groups = collections.Counter()
    unhandled = collections.Counter()
    invalid = 0
    for off in sorted(covered):
        enc = struct.unpack_from("<I", image, off)[0]
        g = group(enc)
        groups[g] += 1
        form = lift.find_form(enc)
        if form and form.valid(enc):
            handled[form.name] += 1
        else:
            if form:
                invalid += 1
            unhandled[subgroup(enc)] += 1

    total = len(covered)
    done = sum(handled.values())
    print(f"instructions in functions: {total:,}")
    print(f"handled by the lifter:     {done:,} ({100 * done / total:.1f}%)")
    print(f"matched a form but invalid: {invalid:,}")
    print("\n== by top-level group")
    for g, n in groups.most_common():
        print(f"{n:10,} {100 * n / total:5.1f}%  {g}")
    print("\n== handled, per form")
    for name, n in handled.most_common():
        print(f"{n:10,}  {name}")
    print("\n== not yet handled")
    for name, n in unhandled.most_common():
        print(f"{n:10,} {100 * n / total:5.1f}%  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
