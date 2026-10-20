"""Check lifted instructions against golden results from real hardware.

    python -m recomp.check                  every form with golden files
    python -m recomp.check addsub_imm       only these forms

For each form, translates every test encoding with recomp.lift, builds one C
test program with the host compiler (CC, default gcc), feeds it the same input
lines the Pi got, and compares output line by line. Mismatches are printed
with the disassembly-free details needed to debug: encoding, the registers
that differ, expected vs actual.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.yw4 import config

from . import lift

REPO = Path(__file__).resolve().parents[1]
INCLUDE = REPO / "runtime" / "include"

PROGRAM = r"""
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "yw4cpu.h"

static int run(uint32_t enc, CPUState *ctx) {
    switch (enc) {
%s
    default: return 0;
    }
    return 1;
}

int main(void) {
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        CPUState s = {0};
        char *p = line;
        uint32_t enc = (uint32_t)strtoul(p, &p, 16);
        for (int i = 0; i < 31; i++) s.x[i] = strtoull(p, &p, 16);
        s.nzcv = strtoull(p, &p, 16);
        if (!run(enc, &s)) { printf("%%08" PRIx32 " UNSUPPORTED\n", enc); continue; }
        printf("%%08" PRIx32, enc);
        for (int i = 0; i < 31; i++) printf(" %%" PRIx64, s.x[i]);
        printf(" %%" PRIx64 "\n", s.nzcv);
    }
    return 0;
}
"""


def build_program(encodings: list[int], exe: Path) -> None:
    cases = []
    for enc in encodings:
        cases.append(f"    case 0x{enc:08x}u: {{ {lift.lift(enc)} }} break;")
    src = exe.with_suffix(".c")
    src.write_text(PROGRAM % "\n".join(cases))
    cc = os.environ.get("CC", "gcc")
    proc = subprocess.run([cc, "-std=c11", "-O1", "-w", f"-I{INCLUDE}", "-o", str(exe), str(src)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"compile failed:\n{proc.stderr[-3000:]}")


def describe_diff(inp: str, want: str, got: str) -> str:
    i, w, g = inp.split(), want.split(), got.split()
    names = ["enc"] + [f"x{k}" for k in range(31)] + ["nzcv"]
    diffs = [f"{names[k]}: in={i[k]} want={w[k]} got={g[k] if k < len(g) else '?'}"
             for k in range(1, len(w)) if k >= len(g) or w[k] != g[k]]
    return f"  {w[0]}  " + "; ".join(diffs)


def check_form(name: str, golden: Path, tmp: Path) -> tuple[int, int, list[str]]:
    inputs = (golden / f"{name}.in").read_text().splitlines()
    expected = (golden / f"{name}.out").read_text().splitlines()
    encodings = sorted({int(line.split()[0], 16) for line in inputs})
    exe = tmp / f"check_{name}.exe"
    build_program(encodings, exe)
    proc = subprocess.run([str(exe)], input="\n".join(inputs) + "\n", capture_output=True, text=True)
    actual = proc.stdout.splitlines()
    bad = [describe_diff(i, w, a) for i, w, a in zip(inputs, expected, actual) if w != a]
    if len(actual) != len(expected):
        bad.append(f"  output has {len(actual)} lines, expected {len(expected)}")
    return len(inputs), len(bad), bad


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("forms", nargs="*")
    ap.add_argument("--show", type=int, default=8, help="mismatches to print per form")
    args = ap.parse_args(argv)
    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    golden = cfg.work_dir / "golden"
    names = args.forms or sorted(p.stem for p in golden.glob("*.in"))
    if not names:
        print(f"no golden files in {golden}; run python -m recomp.golden first", file=sys.stderr)
        return 1

    failed = 0
    with tempfile.TemporaryDirectory() as t:
        for name in names:
            total, nbad, bad = check_form(name, golden, Path(t))
            status = "ok" if nbad == 0 else f"{nbad} MISMATCHES"
            print(f"{name:15} {total:6} cases  {status}")
            for line in bad[:args.show]:
                print(line)
            failed += nbad > 0
    print("all forms match" if not failed else f"{failed} form(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
