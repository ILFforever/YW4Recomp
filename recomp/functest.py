"""Whole-function tests: real compiled ARM64 code vs. its translation (decision 0007).

    python -m recomp.functest               build, run on the Pi, translate, compare
    python -m recomp.functest --cases 200   argument sets per test function (default 40)

The test functions are our own code (tests/functest/). The Pi compiles them as
ARMv8.0 without SIMD and links them into a flat image like main (flat.ld), then
runs every t_* function natively (pi/funcrun.c). The same image is translated by
recomp.function, compiled on the host, and run on the same inputs. Return values
and every changed byte of the scratch buffer must match.
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.yw4 import config

from . import check, function, golden

SRC = check.REPO / "tests" / "functest"
PI_DIR, INCLUDE = check.PI_DIR, check.INCLUDE
DISPATCH = check.REPO / "runtime" / "src" / "dispatch.c"
REMOTE_DIR = "yw4-functest"

CFLAGS = ("-O2 -mcpu=cortex-a57 -mgeneral-regs-only -fno-jump-tables -ffreestanding -fno-builtin "
          "-fno-tree-loop-distribute-patterns -fPIE -fno-stack-protector "
          "-fno-asynchronous-unwind-tables -fno-unwind-tables")
BUILD = f"""set -e
gcc {CFLAGS} -c funcs.c -o funcs.o
gcc -c jumptable.S -o jumptable.o
gcc -c atomics.S -o atomics.o
if readelf -rW funcs.o jumptable.o atomics.o | grep -q -E 'ABS64|GOT|TLS'; then
    echo 'test code needs load-time relocations; the flat image cannot have them' >&2; exit 1
fi
ld -T flat.ld -o funcs.elf funcs.o jumptable.o atomics.o
objcopy -O binary funcs.elf funcs.bin
nm funcs.elf > funcs.sym
gcc -O1 -Wall -o funcrun funcrun.c
"""

DRIVER = r"""
#include "yw4rt.h"
#include "testcase.h"

%(protos)s

%(functions)s

static const FuncEntry funcs[] = {
%(table)s
};
uint64_t yw4_image_base, yw4_sdk_base;

static uint8_t image[1 << 22] __attribute__((aligned(4096)));

const Yw4CodeModule yw4_code[] = {{"test", &yw4_image_base, sizeof image, funcs, %(nfuncs)d}};
const size_t yw4_ncode = 1;
static uint8_t stack[1 << 20] __attribute__((aligned(16)));

int main(int argc, char **argv) {
    FILE *img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 1; }
    fread(image, 1, sizeof image, img);
    fclose(img);
    yw4_image_base = (uint64_t)(uintptr_t)image;
    test_mem_init();
    uint64_t mem = (uint64_t)(uintptr_t)test_mem;
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        CPUState ctx = {0};
        uint64_t off = strtoull(p, &p, 16);
        for (int i = 0; i < 4; i++) ctx.x[i] = strtoull(p, &p, 16);
        uint64_t inmask = strtoull(p, &p, 16);
        for (int i = 0; i < 4; i++)
            if (inmask >> i & 1) ctx.x[i] += mem;
        ctx.sp = (uint64_t)(uintptr_t)(stack + sizeof stack);
        yw4_resolve(yw4_image_base + off)(&ctx);
        printf("%%" PRIx64 " %%" PRIx64 " ", off, ctx.x[0]);
        test_write_mem(stdout);
    }
    return 0;
}
"""


def build_on_pi(cfg: config.Config, out: Path) -> None:
    opts = golden.ssh_base(cfg)
    files = [SRC / "funcs.c", SRC / "jumptable.S", SRC / "atomics.S", SRC / "flat.ld", PI_DIR / "funcrun.c", PI_DIR / "testcase.h"]
    subprocess.run(["ssh", *opts, cfg.pi_host, f"mkdir -p {REMOTE_DIR}"], check=True)
    subprocess.run(["scp", "-q", *opts, *map(str, files), f"{cfg.pi_host}:{REMOTE_DIR}/"], check=True)
    subprocess.run(["ssh", *opts, cfg.pi_host, f"cd {REMOTE_DIR} && sh -s"], input=BUILD.encode(), check=True)
    subprocess.run(["scp", "-q", *opts, f"{cfg.pi_host}:{REMOTE_DIR}/funcs.bin",
                    f"{cfg.pi_host}:{REMOTE_DIR}/funcs.sym", str(out)], check=True)


def read_symbols(path: Path) -> dict[str, int]:
    syms = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def make_cases(tests: dict[str, int], count: int, rng: random.Random) -> list[str]:
    lines = []
    for off in tests.values():
        for _ in range(count):
            buf = 0x8000 + 8 * rng.randrange(0x200)
            args = [buf] + [golden.random_value(rng) for _ in range(3)]
            lines.append(" ".join(f"{v:x}" for v in [off, *args, 1]))
    return lines


def translate(image: bytes, syms: dict[str, int]) -> tuple[str, list[int]]:
    code = function.Code(image, syms["__text_end"])
    entries = {v for k, v in syms.items() if code.is_code(v) and not k.startswith("__")}
    for off in range(0, code.text_end, 4):
        f = function.decode_flow(code.word(off), off)
        if f and f.kind == "bl":
            entries.add(f.target)
    funcs = [function.discover(code, e, entries) for e in sorted(entries)]
    problems = [p for fn in funcs for p in fn.problems]
    if problems:
        raise RuntimeError("flow problems: " + "; ".join(problems))
    stats = function.EmitStats()
    src = "\n\n".join(function.emit_function(code, fn, entries, lambda e: f"{e:08x}", stats) for fn in funcs)
    if stats.unimplemented:
        raise RuntimeError(f"untranslated instructions in the test image: {stats.unimplemented}")
    order = sorted(entries)
    return DRIVER % {
        "protos": "\n".join(f"void {function.fname(e)}(CPUState *ctx);" for e in order),
        "functions": src,
        "table": ",\n".join(f"    {{{e:#x}, {function.fname(e)}}}" for e in order),
        "nfuncs": len(order),
    }, sorted(fn.entry for fn in funcs if fn.jump_tables)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cases", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--opt", default="-O2", help="host compiler optimisation level")
    args = ap.parse_args(argv)
    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1

    out = cfg.work_dir / "functest"
    out.mkdir(parents=True, exist_ok=True)
    build_on_pi(cfg, out)
    image = (out / "funcs.bin").read_bytes()
    syms = read_symbols(out / "funcs.sym")
    tests = {k: v for k, v in sorted(syms.items()) if k.startswith("t_")}
    cases = make_cases(tests, args.cases, random.Random(args.seed))

    proc = subprocess.run(["ssh", *golden.ssh_base(cfg), cfg.pi_host, f"cd {REMOTE_DIR} && ./funcrun funcs.bin"],
                          input="\n".join(cases) + "\n", capture_output=True, text=True)
    if proc.returncode != 0 or len(proc.stdout.splitlines()) != len(cases):
        print(f"funcrun failed ({proc.returncode}): {proc.stderr.strip()[-500:]}", file=sys.stderr)
        return 1
    expected = proc.stdout.splitlines()

    src, with_tables = translate(image, syms)
    with tempfile.TemporaryDirectory() as t:
        c, exe = Path(t) / "functest.c", Path(t) / "functest.exe"
        c.write_text(src)
        cc = check.host_cc()
        build = subprocess.run([cc, "-std=c11", args.opt, "-Wall", "-Wno-unused-label", f"-I{INCLUDE}",
                                f"-I{PI_DIR}", "-o", str(exe), str(c), str(DISPATCH)],
                               capture_output=True, text=True)
        if build.returncode != 0:
            print(f"host compile failed:\n{build.stderr[-3000:]}", file=sys.stderr)
            return 1
        run = subprocess.run([str(exe), str(out / "funcs.bin")], input="\n".join(cases) + "\n",
                             capture_output=True, text=True)
        actual = run.stdout.splitlines()

    names = {v: k for k, v in tests.items()}
    bad: dict[str, list[str]] = {}
    for i, want in enumerate(expected):
        got = actual[i] if i < len(actual) else "(no output: the translated code crashed)"
        if got != want:
            off = int(want.split()[0], 16)
            bad.setdefault(names[off], []).append(f"    in:   {cases[i]}\n    want: {want[:200]}\n    got:  {got[:200]}")
    if run.returncode != 0:
        print(f"translated program exited with {run.returncode}: {run.stderr.strip()[-300:]}")
    for name in tests:
        status = f"{len(bad[name])} MISMATCHES" if name in bad else "ok"
        print(f"{name:20} {args.cases:4} cases  {status}")
        for line in bad.get(name, [])[:2]:
            print(line)
    print(f"functions with recognised jump tables: {', '.join(f'{e:#x}' for e in with_tables) or 'none'}")
    print("all functions match" if not bad else f"{len(bad)} function(s) failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
