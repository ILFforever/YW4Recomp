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


# --------------------------------------------------------------------------- SIMD/FP values

F32_EDGES = [
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40490FDB,  # 0, -0, 1, -1, 0.5, pi
    0x7F800000, 0xFF800000,                                                  # +-inf
    0x7FC00000, 0xFFC00000, 0x7FC12345, 0x7FA00000, 0x7F800001, 0xFF812345,  # quiet / signalling NaNs
    0x00000001, 0x807FFFFF, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF,              # denormals, min normal, max
    0x4F000000, 0xCF000000, 0x5F000000, 0xDF000000, 0x4F800000, 0x4B000000,  # 2^31, -2^31, 2^63, 2^32, 2^23
    0x3EFFFFFF, 0x3FC00000, 0xBFC00000, 0x40200000,                          # rounding: 0.49.., 1.5, -1.5, 2.5
]
F64_EDGES = [
    0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000,
    0x7FF0000000000000, 0xFFF0000000000000,
    0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000012345, 0x7FF4000000000000, 0x7FF0000000000001,
    0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF,
    0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0xC3E0000000000000, 0x41F0000000000000,
    0x3810000000000000, 0x47EFFFFFE0000000, 0x36A0000000000000,  # float min normal, float max, float denorm
    0x3FF8000000000000, 0xBFF8000000000000, 0x4004000000000000,  # 1.5, -1.5, 2.5
]


def random_f32(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.3:
        return rng.choice(F32_EDGES)
    if r < 0.85:  # an ordinary number of moderate size
        return rng.getrandbits(1) << 31 | rng.randrange(127 - 24, 127 + 24) << 23 | rng.getrandbits(23)
    return rng.getrandbits(32)


def random_f64(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.3:
        return rng.choice(F64_EDGES)
    if r < 0.85:
        return rng.getrandbits(1) << 63 | rng.randrange(1023 - 60, 1023 + 60) << 52 | rng.getrandbits(52)
    return rng.getrandbits(64)


def random_vreg(rng: random.Random) -> int:
    """A 128-bit SIMD register value."""
    r = rng.random()
    if r < 0.4:
        return sum(random_f32(rng) << (32 * i) for i in range(4))
    if r < 0.7:
        return random_f64(rng) | random_f64(rng) << 64
    if r < 0.85:
        return sum((random_value(rng) & 0xFFFFFFFF) << (32 * i) for i in range(4))  # integer lanes
    return rng.getrandbits(128)


def _rep32(v: int) -> int:
    return v * 0x00000001_00000001_00000001_00000001


def _rep64(v: int) -> int:
    return v | v << 64


F_PAIRS_32 = [
    (0x7F800000, 0xFF800000), (0x7F800000, 0x00000000), (0x00000000, 0x80000000), (0x7FC12345, 0x3F800000),
    (0x7FA00000, 0x7FC00000), (0x3F800000, 0xFF812345), (0x00000001, 0x00800000), (0x7F7FFFFF, 0x7F7FFFFF),
    (0x7F7FFFFF, 0xFF7FFFFF), (0x4F000000, 0xCF000000), (0x3F000000, 0x3FC00000), (0x7F800000, 0x7F800000),
    (0x7FC12345, 0xFFC54321), (0x7FA00001, 0xFFB00002), (0x80000000, 0x80000000), (0x80000000, 0x3F800000),
    # products close to 2 and 3: FRECPS/FRSQRTS (2 - ab, (3 - ab)/2) expose a missing fused rounding
    (0x3FB504F3, 0x3FB504F3), (0x3F8CCCCD, 0x3FE8BA2F), (0x3FDDB3D7, 0x3FDDB3D7), (0xBFB504F3, 0xBFB504F4),
]
F_PAIRS_64 = [
    (0x7FF0000000000000, 0xFFF0000000000000), (0x0000000000000000, 0x8000000000000000),
    (0x7FF8000000012345, 0x3FF0000000000000), (0x7FF4000000000000, 0x7FF8000000000000),
    (0x0000000000000001, 0x0010000000000000), (0x7FEFFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF),
    (0x41E0000000000000, 0xC3E0000000000000), (0x47EFFFFFE0000000, 0x36A0000000000000),
    (0x7FF8000000012345, 0xFFF8000000054321), (0x7FF4000000000001, 0xFFF4000000000002),
    (0x8000000000000000, 0x8000000000000000),
    (0x3FF6A09E667F3BCD, 0x3FF6A09E667F3BCD), (0x3FFBB67AE8584CAA, 0x3FFBB67AE8584CAA),
]
V_PAIRS = [(_rep32(a), _rep32(b)) for a, b in F_PAIRS_32] + [(_rep64(a), _rep64(b)) for a, b in F_PAIRS_64]
V_PAIRS += [(b, a) for a, b in V_PAIRS]


MEM_SIZE = 0x20000  # must match TEST_MEM_SIZE in pi/testcase.h
# Effective addresses land in [MEM_LO, MEM_HI); with up to 32 bytes accessed (a Q pair)
# and post-index offsets of at most +-1024, everything stays inside the buffer.
MEM_LO, MEM_HI = 0x8000, 0x17FC0


def testable(f: lift.Form, enc: int) -> bool:
    if f.vregs and 31 in f.vregs(enc):
        return False  # the harness parks its state in v31
    a = f.mem(enc) if f.mem else None
    if a is None:
        return True
    if a.vec and 31 in a.regs:
        return False
    if f.name.startswith("simd_ldst_"):
        st = lift._decode_struct(enc)
        if st.m is not None and st.m == st.n != 31:
            return False  # post-index by the base register itself: can't rebase
    if f.name == "excl" and not a.load:
        return False  # a lone STXR's result is IMPLEMENTATION DEFINED; functest covers LDXR/STXR loops
    if not a.load and not a.vec and a.n != 31 and a.n in a.regs:
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
    if a.aligned or rng.random() < 0.6:
        target &= ~(a.esize - 1)  # mostly aligned, some unaligned (never for exclusives/acquire/release)
    if a.rm:
        offset = extend_value(regs[a.rm[0]], a.rm[1], a.rm[2]) if a.rm[0] != 31 else 0
    else:
        offset = 0 if a.index == "post" else a.imm
    base = (target - offset) & lift.ones(64)
    if a.n == 31:
        base &= ~15  # Linux faults on SP-based accesses when SP isn't 16-byte aligned
    regs[a.n] = base
    bit = 1 << a.n
    loaded_over = a.load and not a.vec and a.n in a.regs and a.n != 31
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


def pair_vregs(rng: random.Random) -> list[int]:
    v1, v2 = rng.choice(V_PAIRS)
    return [v1 if i % 2 == 0 else v2 for i in range(31)] + [0]


# SIMD forms that read X registers keep random X values (edge cases matter there).
X_READERS = {"fp_int", "simd_copy"}


def apply_filler(f: lift.Form, enc: int, a: lift.MemAccess | None, regs: list[int], vregs: list[int]) -> None:
    """Registers the instruction can't read get check.filler() values, which the golden
    files store as "*" (keeps them small; the harness still loads and compares them)."""
    if f.vregs:
        used_v = set(f.vregs(enc))
    elif a and a.vec:
        used_v = set(a.regs)
    else:
        used_v = set()
    for i in range(31):
        if i not in used_v:
            vregs[i] = int(check.filler(enc, 34 + i), 16)
        if f.vregs and f.name not in X_READERS:
            regs[i] = int(check.filler(enc, 1 + i), 16)


def make_cases(f: lift.Form, count: int, states: int, pairs: int, rng: random.Random) -> list[str]:
    lines = []
    for enc in random_encodings(f, count, rng):
        a = f.mem(enc) if f.mem else None
        npairs = pairs * 3 if f.vregs or (a and a.vec) else pairs  # FP edge pairs are the point there
        for k in range(states + npairs):
            # x0..x30, SP
            regs = [random_value(rng) for _ in range(32)] if k < states else pair_state(rng)
            vregs = [random_vreg(rng) for _ in range(31)] + [0] if k < states else pair_vregs(rng)
            apply_filler(f, enc, a, regs, vregs)
            inmask = outmask = 0
            if a:
                inmask, outmask = place_memory(a, regs, rng)
            nzcv = rng.randrange(16) << 28
            lines.append(" ".join([f"{enc:08x}"] + [f"{v:x}" for v in regs] + [f"{nzcv:x}"] +
                                  [f"{v:032x}" for v in vregs] + ["0", f"{inmask:x}", f"{outmask:x}"]))
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
        check.write_golden(out_dir / f"{f.name}.in.xz", [check.compact_in(c) for c in cases])
        check.write_golden(out_dir / f"{f.name}.out.xz",
                           [check.compact_out(c, r) for c, r in zip(cases, results)])
        print(f"{f.name:15} {len(cases):6} cases")
    print(f"wrote {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
