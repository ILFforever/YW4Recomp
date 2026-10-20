"""Generate golden test results for lifted instruction forms on a real ARM64 machine.

    python -m recomp.golden                 all forms
    python -m recomp.golden addsub_imm dp3  only these forms
    python -m recomp.golden --cases 2000    encodings per form (default 300)

For each form: random valid encodings, each run with a few register states
mixing edge-case and random values. For loads/stores the base register is
pointed into a scratch buffer (pi/testcase.h) so the access is safe and the
result doesn't depend on the buffer's address. The cases go to the Pi
(decision 0004), run in recomp/pi/harness, and come back as
tests/golden/<form>.in.xz / <form>.out.xz (line format: pi/testcase.h).
They are synthetic (random encodings, not the game's code), so they are committed:
recomp.check then works without the Pi.
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys

from tools.yw4 import config

from . import check, lift

PI_DIR = check.PI_DIR
REMOTE_DIR = "yw4-golden"

EDGE_VALUES = [
    0, 1, 2, 3, 0x1F, 0x20, 0x3F, 0x40, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
    0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000, 0x7FFFFFFFFFFFFFFF,
    0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE, 0xFFFFFFFF80000000,
]


def random_value(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.35:
        return rng.choice(EDGE_VALUES)
    if r < 0.55:
        return rng.getrandbits(32)
    if r < 0.65:
        return rng.randrange(0, 64)
    return rng.getrandbits(64)


MEM_SIZE = 0x20000  # must match TEST_MEM_SIZE in pi/testcase.h
# Accesses land in [MEM_LO, MEM_HI) plus the immediate: at most +32760 (scaled imm12) or
# -512 (pair imm7), plus 16 bytes, which stays inside the buffer.
MEM_LO, MEM_HI = 0x8000, 0x17FF0


def testable(f: lift.Form, enc: int) -> bool:
    a = f.mem(enc) if f.mem else None
    if a is None:
        return True
    if not a.load and a.n != 31 and a.n in a.regs:
        return False  # would store the buffer's absolute address, which differs per machine
    if a.rm and a.rm[0] == a.n != 31:
        return False  # base and offset in one register: can't rebase the base alone
    return True


def random_encodings(f: lift.Form, count: int, rng: random.Random) -> list[int]:
    free = ~f.mask & 0xFFFFFFFF
    out: set[int] = set()
    tries = 0
    while len(out) < count and tries < count * 200:
        tries += 1
        enc = (rng.getrandbits(32) & free) | f.value
        if f.valid(enc) and testable(f, enc):
            out.add(enc)
    return sorted(out)


def extend_value(v: int, option: int, shift: int) -> int:
    """ExtendReg, the same as the lifter's extend_expr but on a Python int."""
    width = 8 << (option & 3)
    v &= lift.ones(width)
    if option & 4:
        v = lift.sign_extend(v, width)
    return (v << shift) & lift.ones(64)


def place_memory(a: lift.MemAccess, regs: list[int], rng: random.Random) -> tuple[int, int]:
    """Point the base register (regs[31] = SP) into the scratch buffer.

    Returns (inmask, outmask) for the harness: which registers hold buffer offsets
    before and after the instruction."""
    target = rng.randrange(MEM_LO, MEM_HI)
    if rng.random() < 0.6:
        target &= ~(a.esize - 1)  # mostly aligned, some unaligned
    offset = extend_value(regs[a.rm[0]], a.rm[1], a.rm[2]) if a.rm and a.rm[0] != 31 else 0
    base = (target - offset) & lift.ones(64)
    if a.n == 31:
        base &= ~15  # Linux faults on SP-based accesses when SP isn't 16-byte aligned
    regs[a.n] = base
    bit = 1 << a.n
    loaded_over = a.load and a.n in a.regs and a.n != 31
    return bit, 0 if loaded_over else bit


# Operand pairs that random states almost never produce, in both orders:
# signed overflow (INT_MIN / -1, INT_MIN - 1, MAX + 1), carries, zero and all-ones,
# 32-bit values with junk or sign bits in the upper half.
_MIN64, _MAX64, _ALL = 0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF
CRITICAL_PAIRS = [
    (_MIN64, _ALL), (0x80000000, 0xFFFFFFFF), (0xFFFFFFFF80000000, _ALL), (_MAX64, 1),
    (0x7FFFFFFF, 1), (_MIN64, 1), (_ALL, _ALL), (0, 0), (0, _ALL), (_MIN64, _MIN64),
    (0xFFFFFFFF, 1), (0x100000000, 0xFFFFFFFF),
]
CRITICAL_PAIRS += [(b, a) for a, b in CRITICAL_PAIRS if a != b]


def pair_state(rng: random.Random) -> list[int]:
    """Even registers get one value of a critical pair, odd registers the other, so any
    instruction reading two registers of different parity sees that exact pair."""
    v1, v2 = rng.choice(CRITICAL_PAIRS)
    return [v1 if i % 2 == 0 else v2 for i in range(31)] + [random_value(rng)]


def make_cases(f: lift.Form, count: int, states: int, pairs: int, rng: random.Random) -> list[str]:
    lines = []
    for enc in random_encodings(f, count, rng):
        a = f.mem(enc) if f.mem else None
        for k in range(states + pairs):
            # x0..x30, SP
            regs = [random_value(rng) for _ in range(32)] if k < states else pair_state(rng)
            inmask = outmask = 0
            if a:
                inmask, outmask = place_memory(a, regs, rng)
            nzcv = rng.randrange(16) << 28
            lines.append(" ".join([f"{enc:08x}"] + [f"{v:x}" for v in regs] +
                                  [f"{nzcv:x}", f"{inmask:x}", f"{outmask:x}"]))
    return lines


def ssh_base(cfg: config.Config) -> list[str]:
    if not cfg.pi_host:
        raise config.ConfigError("[tools] pi_host is not set")
    base = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
    if cfg.pi_key:
        base = ["-i", str(cfg.pi_key)] + base
    return base


def build_harness(cfg: config.Config) -> None:
    opts = ssh_base(cfg)
    subprocess.run(["ssh", *opts, cfg.pi_host, f"mkdir -p {REMOTE_DIR}"], check=True)
    subprocess.run(["scp", "-q", *opts, str(PI_DIR / "harness.c"), str(PI_DIR / "trampoline.S"),
                    str(PI_DIR / "testcase.h"),
                    f"{cfg.pi_host}:{REMOTE_DIR}/"], check=True)
    subprocess.run(["ssh", *opts, cfg.pi_host,
                    f"cd {REMOTE_DIR} && gcc -O1 -Wall -o harness harness.c trampoline.S"], check=True)


def run_on_pi(cfg: config.Config, cases: list[str]) -> list[str]:
    proc = subprocess.run(["ssh", *ssh_base(cfg), cfg.pi_host, f"cd {REMOTE_DIR} && ./harness"],
                          input="\n".join(cases) + "\n", capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"harness failed ({proc.returncode}): {proc.stderr.strip()[-500:]}")
    out = proc.stdout.splitlines()
    if len(out) != len(cases):
        raise RuntimeError(f"harness returned {len(out)} lines for {len(cases)} cases "
                           "(an instruction may have crashed it)")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("forms", nargs="*", help="form names (default: all)")
    ap.add_argument("--cases", type=int, default=300, help="encodings per form")
    ap.add_argument("--states", type=int, default=4, help="random register states per encoding")
    ap.add_argument("--pairs", type=int, default=4, help="critical-pair register states per encoding")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args(argv)

    try:
        cfg = config.load()
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    forms = [f for f in lift.FORMS if not args.forms or f.name in args.forms]
    unknown = set(args.forms) - {f.name for f in lift.FORMS}
    if unknown:
        print(f"unknown forms: {', '.join(sorted(unknown))}", file=sys.stderr)
        return 1

    out_dir = check.GOLDEN
    out_dir.mkdir(parents=True, exist_ok=True)
    build_harness(cfg)
    rng = random.Random(args.seed)
    for f in forms:
        cases = make_cases(f, args.cases, args.states, args.pairs, rng)
        results = run_on_pi(cfg, cases)
        check.write_golden(out_dir / f"{f.name}.in.xz", cases)
        check.write_golden(out_dir / f"{f.name}.out.xz", results)
        print(f"{f.name:15} {len(cases):6} cases")
    print(f"wrote {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
