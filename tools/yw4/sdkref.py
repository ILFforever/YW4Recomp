"""Reference results from the sdk's own functions, run natively on the Pi (decision 0013).

    python -m tools.yw4.sdkref                 run every function, write work/sdkref/
    python -m tools.yw4.sdkref sin pow         only these
    python -m tools.yw4.sdkref --cases 5000    inputs per function (default 20000)
    python -m tools.yw4.sdkref --check         compare the runtime's versions (no Pi needed)

Native versions of sdk functions whose exact results depend on the implementation (libm
first) must match the Switch bit for bit. The reference is the owner's sdk itself: its flat
image is copied to the ARM64 reference machine (decision 0004), relocated there, and each
function is called directly (tools/yw4/pi/sdkrun.c). Results stay in work/sdkref/: they are
produced by the game's sdk, so they are kept with the other derived data.

The sdk is version-locked like main: [sdk] image_sha256 in target.toml.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import random
import struct
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

from recomp import build, check, golden
from tools.yw4 import config, ingest, recon, reloc

PI_SRC = Path(__file__).resolve().parent / "pi" / "sdkrun.c"
REMOTE_DIR = "yw4-sdkref"

# name -> signature understood by sdkrun.c (and the host runner)
FUNCTIONS = {
    **{n: "dd" for n in "acos asin atan ceil cos cosh exp fabs floor log log10 sin sinh sqrt tan tanh".split()},
    **{n: "ddd" for n in "atan2 fmod pow".split()},
    **{n: "ff" for n in "acosf asinf atanf ceilf cosf exp2f expf fabsf floorf log10f logf sinf sqrtf tanf".split()},
    **{n: "fff" for n in "atan2f fmodf powf".split()},
    "frexp": "frexp", "ldexp": "ldexp", "modf": "modf", "modff": "modff",
}


def load_sdk(cfg: config.Config) -> tuple[bytes, dict]:
    """The sdk's flat image and module info. Refuses an sdk that doesn't match target.toml."""
    exefs = cfg.work_dir / "exefs"
    info = {m["name"]: m for m in json.loads((exefs / "modules.json").read_text())}["sdk"]
    image = (exefs / "sdk.bin").read_bytes()
    with ingest.TARGET_PATH.open("rb") as f:
        want = tomllib.load(f).get("sdk", {}).get("image_sha256", "")
    if not want or hashlib.sha256(image).hexdigest() != want:
        raise config.ConfigError(f"{exefs / 'sdk.bin'} does not match [sdk] in target.toml")
    return image, info


def function_offsets(image: bytes, info: dict) -> dict[str, int]:
    names = recon.dynsym_names(image, info)
    ro = info["segments"]["ro"]["mem_offset"]
    start, size = info["dynsym"]
    out = {}
    for i in range(size // 24):
        _, st_info, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", image, ro + start + 24 * i)
        if shndx and value and st_info & 0xF == 2:
            out[names[i]] = value
    return out


def reloc_table(image: bytes, info: dict) -> tuple[bytes, int]:
    """sdkrun's relocation records and the memory size (image + .bss)."""
    names = recon.dynsym_names(image, info)
    ro = info["segments"]["ro"]["mem_offset"]
    start, size = info["dynsym"]
    defined = {}
    for i in range(size // 24):
        _, _, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", image, ro + start + 24 * i)
        if shndx:
            defined[i] = value
    out = bytearray()
    for r in reloc.read_relocs(image):
        if r.type == reloc.R_AARCH64_RELATIVE:
            out += struct.pack("<QQQ", r.offset, r.addend, 0)
        elif r.type in (reloc.R_AARCH64_ABS64, reloc.R_AARCH64_GLOB_DAT, reloc.R_AARCH64_JUMP_SLOT):
            if r.sym in defined:
                out += struct.pack("<QQQ", r.offset, defined[r.sym] + r.addend, 0)
            else:
                out += struct.pack("<QQQ", r.offset, 0, 1)
        else:
            raise RuntimeError(f"sdk relocation type {r.type} not handled")
    mem = (max(len(image), reloc.read_mod0(image).bss_end) + 0xFFF) & ~0xFFF
    return bytes(out), mem


def _f64(x: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def _f32(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def interesting_f64(rng: random.Random, name: str) -> int:
    """Inputs near the places each function is hard: argument reduction points for trig,
    overflow edges for exp, 1 for log, integers and halves for rounding."""
    r = rng.random()
    if r < 0.5:
        return golden.random_f64(rng)
    if name in ("sin", "cos", "tan", "atan2"):
        k = rng.randrange(-200, 200) if r < 0.8 else rng.randrange(-10**15, 10**15)
        return _f64(k * math.pi / 4 * (1 + rng.uniform(-1e-15, 1e-15)))
    if name in ("exp", "cosh", "sinh", "tanh", "pow"):
        return _f64(rng.uniform(-750, 750))
    if name in ("log", "log10", "acos", "asin"):
        return _f64(1 + rng.uniform(-1e-6, 1e-6) if r < 0.75 else rng.uniform(-1.5, 1.5))
    return _f64(rng.randrange(-1000, 1000) / 2 + rng.choice((0, 0, 1e-9, -1e-9)))


def interesting_f32(rng: random.Random, name: str) -> int:
    r = rng.random()
    if r < 0.5:
        return golden.random_f32(rng)
    base = name.rstrip("f")
    if base in ("sin", "cos", "tan", "atan2"):
        return _f32(rng.randrange(-100000, 100000) * math.pi / 4 * (1 + rng.uniform(-1e-7, 1e-7)))
    if base in ("exp", "exp2", "pow"):
        return _f32(rng.uniform(-160, 130))
    if base in ("log", "log10", "acos", "asin"):
        return _f32(1 + rng.uniform(-1e-3, 1e-3) if r < 0.75 else rng.uniform(-1.5, 1.5))
    return _f32(rng.randrange(-1000, 1000) / 2)


def make_cases(name: str, sig: str, offset: int, count: int, rng: random.Random) -> list[str]:
    cases = []
    for _ in range(count):
        if sig in ("dd", "frexp", "modf"):
            args = [interesting_f64(rng, name)]
        elif sig == "ddd":
            a, b = interesting_f64(rng, name), interesting_f64(rng, name)
            if name == "pow" and rng.random() < 0.3:  # integer and half-integer exponents
                b = _f64(rng.randrange(-80, 80) / rng.choice((1, 2)))
            args = [a, b]
        elif sig == "ldexp":
            args = [interesting_f64(rng, name), rng.choice((rng.randrange(-1100, 1100), rng.getrandbits(32)))]
        elif sig in ("ff", "modff"):
            args = [interesting_f32(rng, name)]
        else:  # fff
            a, b = interesting_f32(rng, name), interesting_f32(rng, name)
            if name == "powf" and rng.random() < 0.3:
                b = _f32(rng.randrange(-40, 40) / rng.choice((1, 2)))
            args = [a, b]
        cases.append(f"{sig} {offset:x} " + " ".join(f"{a:x}" for a in args))
    return cases


def run_on_pi(cfg: config.Config, image: bytes, relocs: bytes, mem: int, cases: list[str]) -> list[str]:
    opts = golden.ssh_base(cfg)
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        (t / "sdk.bin").write_bytes(image)
        (t / "relocs.bin").write_bytes(relocs)
        subprocess.run(["ssh", *opts, cfg.pi_host, f"mkdir -p {REMOTE_DIR}"], check=True)
        subprocess.run(["scp", "-q", *opts, str(t / "sdk.bin"), str(t / "relocs.bin"), str(PI_SRC),
                        f"{cfg.pi_host}:{REMOTE_DIR}/"], check=True)
    subprocess.run(["ssh", *opts, cfg.pi_host, f"cd {REMOTE_DIR} && gcc -O1 -Wall -o sdkrun sdkrun.c"], check=True)
    proc = subprocess.run(["ssh", *opts, cfg.pi_host, f"cd {REMOTE_DIR} && ./sdkrun sdk.bin relocs.bin {mem:#x}"],
                          input="\n".join(cases) + "\n", capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"sdkrun failed ({proc.returncode}): {proc.stderr.strip()[-500:]}")
    out = proc.stdout.splitlines()
    if len(out) != len(cases):
        raise RuntimeError(f"sdkrun returned {len(out)} lines for {len(cases)} cases")
    return out


HOST_DRIVER = r"""
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "hle.h"
%(protos)s
static const struct { const char *name; GuestFn fn; } table[] = {
%(table)s
};
int main(void) {
    static CPUState c;
    char line[256], sig[16], name[32];
    uint64_t a, b, extra[2];
    while (fgets(line, sizeof line, stdin)) {
        a = b = 0;
        if (sscanf(line, "%%15s %%31s %%" SCNx64 " %%" SCNx64, sig, name, &a, &b) < 3) continue;
        GuestFn fn = NULL;
        for (size_t i = 0; i < sizeof table / sizeof *table; i++)
            if (!strcmp(table[i].name, name)) fn = table[i].fn;
        if (!fn) { fprintf(stderr, "no %%s\n", name); return 1; }
        int w32 = !strcmp(sig, "ff") || !strcmp(sig, "fff") || !strcmp(sig, "modff");
        memset(&c, 0, sizeof c);
        c.v[0].d[0] = a;
        c.v[1].d[0] = b;
        c.x[0] = !strcmp(sig, "ldexp") ? b : (uint64_t)(uintptr_t)extra;
        extra[0] = 0x7eadbeef;
        fn(&c);
        if (w32) printf("%%08" PRIx32, c.v[0].s[0]); else printf("%%016" PRIx64, c.v[0].d[0]);
        if (!strcmp(sig, "frexp")) printf(" %%08x", (unsigned)(uint32_t)extra[0]);
        if (!strcmp(sig, "modf")) printf(" %%016" PRIx64, extra[0]);
        if (!strcmp(sig, "modff")) printf(" %%08" PRIx32, (uint32_t)extra[0]);
        putchar('\n');
    }
    return 0;
}
"""


def build_host_runner(tmp: Path) -> Path:
    """The runtime's own hle_math.c + musl, compiled with the flags recomp.build uses."""
    cc = check.host_cc()
    names = sorted(FUNCTIONS)
    (tmp / "drv.c").write_text(HOST_DRIVER % {
        "protos": "".join(f"void hle_math_{n}(CPUState *ctx);\n" for n in names),
        "table": "".join(f'    {{"{n}", hle_math_{n}}},\n' for n in names)})
    rt = build.RUNTIME
    inc = [f"-I{rt / 'include'}", f"-I{rt / 'src'}"]
    jobs = [([cc, *build.CFLAGS, *inc, "-c", str(tmp / "drv.c"), "-o", str(tmp / "drv.o")], tmp / "drv.o"),
            ([cc, *build.CFLAGS, *inc, "-c", str(rt / "src" / "hle_math.c"), "-o", str(tmp / "m.o")], tmp / "m.o")]
    jobs += [([cc, *build.MUSL_CFLAGS, "-c", str(src), "-o", str(tmp / f"musl_{src.stem}.o")], tmp / f"musl_{src.stem}.o")
             for src in build.musl_sources()]
    with concurrent.futures.ThreadPoolExecutor() as pool:
        for proc in pool.map(lambda j: subprocess.run(j[0], capture_output=True, text=True), jobs):
            if proc.returncode:
                raise RuntimeError(proc.stderr[-2000:])
    exe = tmp / "mathcheck.exe"
    subprocess.run([cc, "-o", str(exe), *(str(o) for _, o in jobs)], check=True, capture_output=True)
    return exe


def check_runtime(cfg: config.Config, names: list[str]) -> int:
    ref = cfg.work_dir / "sdkref"
    missing = [n for n in names if not (ref / f"{n}.txt").is_file()]
    if missing:
        print(f"no reference results for {missing}: run python -m tools.yw4.sdkref first", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as t:
        exe = build_host_runner(Path(t))
        failed = 0
        for n in names:
            lines = (ref / f"{n}.txt").read_text().splitlines()
            sig, cases = lines[0], [l.split(" -> ") for l in lines[1:]]
            proc = subprocess.run([str(exe)], input="".join(f"{sig} {n} {a}\n" for a, _ in cases),
                                  capture_output=True, text=True)
            got = proc.stdout.splitlines()
            bad = [(a, w, g) for (a, w), g in zip(cases, got) if w != g] + [("?", "?", "missing")] * (len(cases) - len(got))
            if bad:
                failed += 1
                a, w, g = bad[0]
                print(f"{n}: {len(bad)} of {len(cases)} differ, e.g. {a}: sdk {w}, runtime {g}")
        print(f"{len(names) - failed} of {len(names)} functions match the sdk bit for bit")
        return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="functions (default: all)")
    ap.add_argument("--cases", type=int, default=20000)
    ap.add_argument("--check", action="store_true", help="compare the runtime against stored results")
    args = ap.parse_args(argv)
    try:
        cfg = config.load()
        image, info = load_sdk(cfg)
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    names = args.names or sorted(FUNCTIONS)
    unknown = [n for n in names if n not in FUNCTIONS]
    if unknown:
        print(f"unknown functions: {unknown}", file=sys.stderr)
        return 1
    if args.check:
        return check_runtime(cfg, names)
    offsets = function_offsets(image, info)
    relocs, mem = reloc_table(image, info)
    cases = []
    for n in names:
        cases += make_cases(n, FUNCTIONS[n], offsets[n], args.cases, random.Random(f"sdkref-{n}"))
    results = run_on_pi(cfg, image, relocs, mem, cases)
    outdir = cfg.work_dir / "sdkref"
    outdir.mkdir(parents=True, exist_ok=True)
    for i, n in enumerate(names):
        lines = [f"{c.split(' ', 2)[2]} -> {r}" for c, r in
                 zip(cases[i * args.cases:(i + 1) * args.cases], results[i * args.cases:(i + 1) * args.cases])]
        (outdir / f"{n}.txt").write_text(f"{FUNCTIONS[n]}\n" + "\n".join(lines) + "\n")
    print(f"{len(names)} functions x {args.cases} cases -> {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
