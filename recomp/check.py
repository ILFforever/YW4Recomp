"""Check lifted instructions against golden results from real hardware.

    python -m recomp.check                  every form with golden files
    python -m recomp.check addsub_imm       only these forms
    python -m recomp.check --opt=-O0        another optimisation level (default -O2)
    python -m recomp.check --ubsan          trap on undefined behaviour in the lifted C

Golden files live in tests/golden/ (xz-compressed, made by recomp.golden), so
this needs neither the Pi nor the game.

For each form, translates every test encoding with recomp.lift, builds one C
test program with the host compiler (CC, default gcc), feeds it the same input
lines the Pi got, and compares output line by line. Mismatches are printed
with the disassembly-free details needed to debug: encoding, the registers
that differ, expected vs actual.
"""

from __future__ import annotations

import argparse
import hashlib
import lzma
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from . import lift

REPO = Path(__file__).resolve().parents[1]
INCLUDE = REPO / "runtime" / "include"
PI_DIR = REPO / "recomp" / "pi"
GOLDEN = REPO / "tests" / "golden"

PROGRAM = r"""
#include "yw4cpu.h"
#include "testcase.h"

static int run(uint32_t enc, CPUState *ctx) {
    switch (enc) {
%s
    default: return 0;
    }
    return 1;
}

int main(void) {
    test_mem_init();
    uint64_t base = (uint64_t)(uintptr_t)test_mem;
    TestState t;
    uint32_t enc;
    uint64_t inmask, outmask;
    while (test_read(stdin, &enc, &t, &inmask, &outmask)) {
        test_rebase(&t, inmask, base);
        CPUState s = {0};
        memcpy(s.x, t.x, sizeof t.x);
        s.sp = t.sp;
        s.nzcv = t.nzcv;
        memcpy(s.v, t.v, sizeof t.v);
        s.fpcr = (uint32_t)t.fpcr;
        if (!run(enc, &s)) { printf("%%08" PRIx32 " UNSUPPORTED\n", enc); continue; }
        memcpy(t.x, s.x, sizeof t.x);
        t.sp = s.sp;
        t.nzcv = s.nzcv;
        memcpy(t.v, s.v, sizeof t.v);
        test_rebase(&t, outmask, 0 - base);
        test_write(stdout, enc, &t);
    }
    return 0;
}
"""


# Case lines (pi/testcase.h): field 0 enc, 1-31 x0-x30, 32 sp, 33 nzcv, 34-65 v0-v31, then
# fpcr, inmask, outmask (input) or the memory changes (output).
#
# Stored compactly in tests/golden/: a register the test doesn't care about holds filler(),
# a fixed pseudo-random value per (encoding, field), written as "*"; an output line lists
# only the fields that differ from its input ("k=value,..." or "=" for none).
STATE = range(1, 66)


def filler(enc: int, k: int) -> str:
    v = int.from_bytes(hashlib.blake2b(f"{enc:08x}/{k}".encode(), digest_size=16).digest(), "little")
    return f"{v:032x}" if 34 <= k < 66 else f"{v & 0xFFFFFFFFFFFFFFFF:x}"


def compact_in(line: str) -> str:
    f = line.split()
    enc = int(f[0], 16)
    return " ".join("*" if k in STATE and f[k] == filler(enc, k) else f[k] for k in range(len(f)))


def expand_in(line: str) -> str:
    f = line.split()
    enc = int(f[0], 16)
    return " ".join(filler(enc, k) if x == "*" else x for k, x in enumerate(f))


def compact_out(inp: str, out: str) -> str:
    i, o = inp.split(), out.split()
    if len(o) != 67:
        return out  # malformed: keep as is
    diff = ",".join(f"{k}={o[k]}" for k in STATE if o[k] != i[k])
    return f"{o[0]} {diff or '='} {o[66]}"


def expand_out(inp: str, line: str) -> str:
    c = line.split()
    if len(c) != 3:
        return line
    o = inp.split()[:66]
    if c[1] != "=":
        for item in c[1].split(","):
            k, v = item.split("=")
            o[int(k)] = v
    o[0] = c[0]
    return " ".join(o + [c[2]])


def read_golden(path: Path) -> list[str]:
    return lzma.decompress(path.read_bytes()).decode().splitlines()


def write_golden(path: Path, lines: list[str]) -> None:
    path.write_bytes(lzma.compress(("\n".join(lines) + "\n").encode(), preset=9))


def load_cases(name: str) -> tuple[list[str], list[str]]:
    """Full (expanded) input and expected output lines of a form."""
    inputs = [expand_in(x) for x in read_golden(GOLDEN / f"{name}.in.xz")]
    expected = [expand_out(i, x) for i, x in zip(inputs, read_golden(GOLDEN / f"{name}.out.xz"))]
    return inputs, expected


def build_program(encodings: list[int], exe: Path, flags: list[str]) -> None:
    """Encodings the lifter rejects are left out; they then print UNSUPPORTED and mismatch."""
    cases = []
    for enc in encodings:
        try:
            cases.append(f"    case 0x{enc:08x}u: {{ {lift.lift(enc)} }} break;")
        except lift.LiftError:
            pass
    src = exe.with_suffix(".c")
    src.write_text(PROGRAM % "\n".join(cases))
    cc = os.environ.get("CC", "gcc")
    proc = subprocess.run([cc, "-std=c11", *flags, "-ffp-contract=off", "-Wall", "-Werror", f"-I{INCLUDE}", f"-I{PI_DIR}",
                           "-o", str(exe), str(src)], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"compile failed:\n{proc.stderr[-3000:]}")


def describe_diff(inp: str, want: str, got: str) -> str:
    i, w, g = inp.split(), want.split(), got.split()
    names = ["enc"] + [f"x{k}" for k in range(31)] + ["sp", "nzcv"] + [f"v{k}" for k in range(32)] + ["mem"]
    diffs = [f"{names[k]}: in={i[k] if k < 66 else '-'} want={w[k]} got={g[k] if k < len(g) else '?'}"
             for k in range(1, len(w)) if k >= len(g) or w[k] != g[k]]
    return f"  {w[0]}  " + "; ".join(diffs)


def check_form(name: str, tmp: Path, flags: list[str]) -> tuple[int, int, list[str]]:
    inputs, expected = load_cases(name)
    encodings = sorted({int(line.split()[0], 16) for line in inputs})
    exe = tmp / f"check_{name}.exe"
    build_program(encodings, exe, flags)
    proc = subprocess.run([str(exe)], input="\n".join(inputs) + "\n", capture_output=True, text=True)
    actual = proc.stdout.splitlines()
    bad = [describe_diff(i, w, a) for i, w, a in zip(inputs, expected, actual) if w != a]
    if len(actual) != len(expected):
        bad.append(f"  output has {len(actual)} lines, expected {len(expected)} "
                   f"(exit code {proc.returncode}: the lifted code may have crashed)")
    return len(inputs), len(bad), bad


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("forms", nargs="*")
    ap.add_argument("--show", type=int, default=8, help="mismatches to print per form")
    ap.add_argument("--opt", default="-O2", help="host compiler optimisation level")
    ap.add_argument("--ubsan", action="store_true", help="trap on undefined behaviour in the lifted C")
    args = ap.parse_args(argv)
    names = args.forms or sorted(p.name[:-len(".in.xz")] for p in GOLDEN.glob("*.in.xz"))
    if not names:
        print(f"no golden files in {GOLDEN}; run python -m recomp.golden first", file=sys.stderr)
        return 1

    flags = [args.opt] + (["-fsanitize=undefined", "-fsanitize-undefined-trap-on-error"] if args.ubsan else [])
    failed = 0
    with tempfile.TemporaryDirectory() as t:
        for name in names:
            total, nbad, bad = check_form(name, Path(t), flags)
            status = "ok" if nbad == 0 else f"{nbad} MISMATCHES"
            print(f"{name:15} {total:6} cases  {status}")
            for line in bad[:args.show]:
                print(line)
            failed += nbad > 0
    print("all forms match" if not failed else f"{failed} form(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
