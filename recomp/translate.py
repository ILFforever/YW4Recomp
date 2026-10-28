"""Translate main and the sdk to C (decisions 0007, 0014).

    python -m recomp.translate              write work/recomp/
    python -m recomp.translate --report     statistics only, write nothing

Both modules are version-locked (main by target.toml [main], the sdk by [sdk]).
Entry points per module: .eh_frame_hdr starts, address-taken targets, defined
function symbols, every BL target, and for main also Ghidra's function entries.
Output (derived from the game's code, so it stays in work/ and is never committed):

    work/recomp/funcs.h          prototypes: f_<offset> (main), s_<offset> (sdk)
    work/recomp/main_NNN.c       main's functions, CHUNK per file (for a parallel build)
    work/recomp/sdk_NNN.c        the sdk's functions
    work/recomp/func_table.c     yw4_code[]: each module's sorted function table (yw4_resolve)
    work/recomp/image_info.c     yw4_modules[]: what the loader needs (sizes, hashes, relocations)

Functions listed in recomp/overrides.toml get a body that calls native runtime code
instead (decision 0016).

Symbols are resolved here, not at run time: both modules are known, so every symbolic
relocation becomes "module + offset" in load order (main first, then the sdk, as the
Switch's dynamic loader searches), or 0 for weak symbols nobody defines.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import struct
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

from tools.yw4 import config, recon, reloc, sdkref

from . import coverage, function

CHUNK = 500

R_RELATIVE, SYMBOL_RELOCS = reloc.R_AARCH64_RELATIVE, (reloc.R_AARCH64_ABS64, reloc.R_AARCH64_GLOB_DAT,
                                                   reloc.R_AARCH64_JUMP_SLOT)
STT_FUNC = 2
DT_INIT = 0xC
NO_MODULE = 0xFFFFFFFF
INIT_START = "_ZN2nn4init5StartEmmPFvvES2_"  # nn::init::Start(u64, u64, void (*)(), void (*)())
OVERRIDES = Path(__file__).with_name("overrides.toml")


@dataclass
class Module:
    name: str            # also the file in work/exefs/ (<name>.bin) and the C file prefix
    prefix: str          # function names
    base: str            # C variable holding the load address
    preferred: int       # where the runtime tries to put it (inside the 39-bit space)
    image: bytes
    info: dict
    code: function.Code

    @property
    def mem_size(self) -> int:
        return (max(len(self.image), reloc.read_mod0(self.image).bss_end) + 0xFFF) & ~0xFFF


def load_modules(cfg: config.Config) -> list[Module]:
    """main, then the sdk: load order, which is also symbol search order. The sdk goes right
    after main, 2 MiB aligned; main stays at its Switch address (decision 0010)."""
    main_image, mods = recon.load_main(cfg)  # each refuses a build that doesn't match target.toml
    sdk_image, sdk_info = sdkref.load_sdk(cfg)
    out = []
    for name, prefix, base, image, info in (("main", "f_", "yw4_image_base", main_image, mods["main"]),
                                            ("sdk", "s_", "yw4_sdk_base", sdk_image, sdk_info)):
        code = function.Code(image, info["segments"]["text"]["size"], base, prefix)
        preferred = 0x7100000000 if not out else (out[-1].preferred + out[-1].mem_size + 0x1FFFFF) & ~0x1FFFFF
        out.append(Module(name, prefix, base, preferred, image, info, code))
    return out


def raw_dynsym(image: bytes, info: dict) -> list[tuple[str, int, int, int]]:
    """(name, type, section index, value) for every .dynsym entry, in index order."""
    names = recon.dynsym_names(image, info)
    ro = info["segments"]["ro"]["mem_offset"]
    start, size = info["dynsym"]
    out = []
    for i in range(size // 24):
        _, st_info, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", image, ro + start + 24 * i)
        out.append((names[i], st_info & 0xF, shndx, value))
    return out


def load_overrides(modules: list[Module]) -> dict[str, dict[int, str]]:
    """Per module: function offset -> the native function that replaces its body."""
    out: dict[str, dict[int, str]] = {m.name: {} for m in modules}
    for o in tomllib.loads(OVERRIDES.read_text()).get("override", []):
        m = next((m for m in modules if m.name == o["module"]), None)
        if m is None:
            raise RuntimeError(f"overrides.toml: no module {o['module']!r}")
        funcs = {name: v for name, typ, shndx, v in raw_dynsym(m.image, m.info) if shndx and typ == STT_FUNC}
        if o["symbol"] not in funcs:
            raise RuntimeError(f"overrides.toml: {m.name} defines no function {o['symbol']}")
        out[m.name][funcs[o["symbol"]]] = o["native"]
    return out


def load_hooks(modules: list[Module]) -> dict[str, dict[int, str]]:
    """Per module: function offset -> the native hook that runs before its body (decision 0024).
    Hooks name game functions by offset (game code has no symbols); only the offset is stored."""
    out: dict[str, dict[int, str]] = {m.name: {} for m in modules}
    for h in tomllib.loads(OVERRIDES.read_text()).get("hook", []):
        if h["module"] not in out:
            raise RuntimeError(f"overrides.toml: no module {h['module']!r}")
        out[h["module"]][int(h["offset"], 16)] = h["native"]
    return out


def guest_symbols(modules: list[Module]) -> list[tuple[str, int, int]]:
    """(symbol, module index, offset) for each [[guest_symbol]] in overrides.toml."""
    out = []
    for g in tomllib.loads(OVERRIDES.read_text()).get("guest_symbol", []):
        i = next((i for i, m in enumerate(modules) if m.name == g["module"]), None)
        if i is None:
            raise RuntimeError(f"overrides.toml: no module {g['module']!r}")
        m = modules[i]
        funcs = {name: v for name, typ, shndx, v in raw_dynsym(m.image, m.info) if shndx and typ == STT_FUNC}
        if g["symbol"] not in funcs:
            raise RuntimeError(f"overrides.toml: {m.name} defines no function {g['symbol']}")
        out.append((g["symbol"], i, funcs[g["symbol"]]))
    return out


def entry_points(cfg: config.Config, m: Module) -> set[int]:
    code = m.code
    entries = set(recon.eh_frame_function_starts(code.data))
    entries |= recon.address_taken_functions(code.data, code.text_end)
    entries |= {v for _, typ, shndx, v in raw_dynsym(m.image, m.info) if shndx and typ == STT_FUNC}
    if m.name == "main":
        ghidra = json.loads((cfg.work_dir / "ghidra" / "main.functions.json").read_text())["functions"]
        entries |= {f["entry"] for f in ghidra}
    for off in range(0, code.text_end, 4):  # BL targets
        f = function.decode_flow(code.word(off), off)
        if f and f.kind == "bl" and code.is_code(f.target):
            entries.add(f.target)
    return {e for e in entries if code.is_code(e)}


def resolve_relocations(modules: list[Module]) -> tuple[list[list], list[list], collections.Counter]:
    """Per module: RELATIVE (offset, addend) and symbolic (offset, module index, value)."""
    syms = [raw_dynsym(m.image, m.info) for m in modules]
    defined = [{name: value for name, _, shndx, value in s if shndx} for s in syms]
    unresolved: collections.Counter = collections.Counter()
    relative, symbolic = [], []
    for i, m in enumerate(modules):
        rel, sym = [], []
        for r in reloc.read_relocs(m.image):
            if r.type == R_RELATIVE:
                rel.append((r.offset, r.addend))
            elif r.type in SYMBOL_RELOCS:
                name = syms[i][r.sym][0]
                where = next((j for j, d in enumerate(defined) if name in d), None)
                if where is None:  # weak and defined nowhere (linker markers): 0
                    unresolved[name] += 1
                    sym.append((r.offset, NO_MODULE, 0))
                else:
                    sym.append((r.offset, where, (defined[where][name] + r.addend) & (2**64 - 1)))
            else:
                raise RuntimeError(f"{m.name}: unsupported relocation type {r.type}")
        relative.append(rel)
        symbolic.append(sym)
    return relative, symbolic, unresolved


def image_info(modules: list[Module], relative: list[list], symbolic: list[list], npdm: bytes,
               gsyms: list[tuple[str, int, int]]) -> str:
    """Loader tables (decisions 0010, 0014): per module its size, hash, relocations, DT_INIT."""
    out = ['#include "yw4load.h"\n']
    rows = []
    for i, m in enumerate(modules):
        rel = ",\n".join(f"    {{{o:#x}, {a:#x}}}" for o, a in relative[i]) or "    {0, 0}"
        sym = ",\n".join(f"    {{{o:#x}, {mod:#x}, {v:#x}}}" for o, mod, v in symbolic[i]) or "    {0, 0, 0}"
        out.append(f"static const Yw4Relative relative_{m.name}[] = {{\n{rel}\n}};\n"
                   f"static const Yw4SymReloc symrelocs_{m.name}[] = {{\n{sym}\n}};\n")
        init = reloc.read_dynamic(m.image, reloc.read_mod0(m.image)).get(DT_INIT, 0)
        rows.append(f'    {{"{m.name}", "{m.name}.bin", "{hashlib.sha256(m.image).hexdigest()}", {len(m.image)}, '
                    f"{m.mem_size:#x}, {m.preferred:#x}, &{m.base},\n"
                    f"     relative_{m.name}, {len(relative[i])}, symrelocs_{m.name}, {len(symbolic[i])}, {init:#x}, "
                    f"{m.info['segments']['ro']['mem_offset']:#x}, {m.info['segments']['data']['mem_offset']:#x}}}")
    start = {name: v for name, _, shndx, v in raw_dynsym(modules[1].image, modules[1].info) if shndx}[INIT_START]
    if npdm[:4] != b"META":
        raise RuntimeError("main.npdm: no META header")
    stack = struct.unpack_from("<I", npdm, 0x1C)[0]
    out.append("const Yw4Module yw4_modules[] = {\n" + ",\n".join(rows) + "\n};\n"
               f"const size_t yw4_nmodules = {len(modules)};\n"
               f"const uint64_t yw4_entry_init_start = {start:#x};  /* nn::init::Start, in the sdk */\n"
               f"const uint64_t yw4_main_stack_size = {stack:#x};  /* main thread, from the NPDM */\n"
               f"const uint32_t yw4_main_priority = {npdm[0xE]};\n")
    rows = ",\n".join(f'    {{"{name}", {i}, {off:#x}}}' for name, i, off in gsyms)
    out.append(f"const Yw4GuestSymbol yw4_guest_symbols[] = {{\n{rows or '    {0, 0, 0}'}\n}};\n"
               f"const size_t yw4_nguest_symbols = {len(gsyms)};\n")
    return "\n".join(out)


def write_if_changed(path, text: str) -> None:
    """Keeps the timestamp of unchanged files, so recomp.build only recompiles what changed."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


SETJMP = ("_setjmp", "setjmp", "__setjmp")


def plt_slot(code: function.Code, e: int) -> int | None:
    """The GOT slot a PLT stub at e jumps through (adrp x16; ldr x17, [x16, #o]; add x16, x16, #o;
    br x17), or None."""
    if e + 16 > code.text_end:
        return None
    w = [code.word(e + 4 * i) for i in range(4)]
    if (w[0] & 0x9F00001F != 0x90000010 or w[1] & 0xFFC003FF != 0xF9400211
            or w[2] & 0xFFC003FF != 0x91000210 or w[3] != 0xD61F0220):
        return None
    page = ((w[0] >> 29) & 3 | ((w[0] >> 5) & 0x7FFFF) << 2) << 12
    if page & (1 << 32):
        page -= 1 << 33
    return (e & ~0xFFF) + page + ((w[1] >> 10) & 0xFFF) * 8


def setjmp_entries(modules: list[Module], symbolic: list[list], entries: list[set[int]]) -> list[frozenset[int]]:
    """Per module, the functions that are setjmp: its definition, and PLT stubs to it (decision 0018)."""
    where = set()
    for i, m in enumerate(modules):
        for name, typ, shndx, v in raw_dynsym(m.image, m.info):
            if shndx and typ == STT_FUNC and name in SETJMP:
                where.add((i, v))
    out = []
    for i, m in enumerate(modules):
        got = {o: (mod, v) for o, mod, v in symbolic[i]}
        found = {v for mod, v in where if mod == i}
        found |= {e for e in entries[i] if got.get(plt_slot(m.code, e)) in where}
        out.append(frozenset(found))
    return out


def translate(cfg: config.Config, m: Module, overrides: dict[int, str], entries: set[int],
              setjmps: frozenset[int], hooks: dict[int, str]) -> tuple[list[function.Function], list[str]]:
    code = m.code
    funcs = [function.discover(code, e, entries) for e in sorted(entries)]
    stats = function.EmitStats()
    sources = []
    for fn in funcs:
        native = overrides.get(fn.entry)
        if native:  # the address stays a function; its body is the native code
            name = function.fname(fn.entry, m.prefix)
            sources.append(f"void {native}(CPUState *ctx);  /* override (recomp/overrides.toml) */\n"
                           f"void {name}(CPUState *ctx) {{ {native}(ctx); }}")
        else:
            sources.append(function.emit_function(code, fn, entries, coverage.subgroup, stats, setjmps,
                                                  hooks.get(fn.entry)))
    missing = (set(overrides) | set(hooks)) - {fn.entry for fn in funcs}
    if missing:
        raise RuntimeError(f"{m.name}: overridden or hooked functions not found: {sorted(map(hex, missing))}")

    brs = collections.Counter()
    for fn in funcs:
        for off in fn.body:
            f = function.decode_flow(code.word(off), off)
            if f and f.kind == "br":
                brs["jump table" if off in fn.jump_tables else "indirect tail call"] += 1
    translated = sum(len(fn.body) for fn in funcs)
    unique = len(set().union(*(fn.body for fn in funcs)))
    problems = [p for fn in funcs for p in fn.problems]
    unimpl = sum(stats.unimplemented.values())
    print(f"== {m.name} (preferred base {m.preferred:#x}, {m.mem_size:#x} bytes)")
    print(f"functions:               {len(funcs):,}")
    print(f"instructions translated: {translated:,} ({unique:,} unique, "
          f"{translated - unique:,} duplicated into more than one function)")
    print(f"untranslated (traps):    {unimpl:,} ({100 * unimpl / translated:.2f}%)")
    for g, n in sorted(stats.unimplemented.items(), key=lambda kv: -kv[1]):
        print(f"    {n:10,}  {g}")
    print(f"BR: {dict(brs)}")
    print(f"overridden by native code: {len(overrides)}; hooked: {len(hooks)}; setjmp functions: {len(setjmps)}")
    print(f"registers in locals: {stats.cached:,} functions; in the CPUState: {stats.not_cached:,} (decision 0035)")
    print(f"flow problems: {len(problems)}")
    for p in problems[:10]:
        print(f"    {p}")
    return funcs, sources


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", action="store_true", help="print statistics, write nothing")
    args = ap.parse_args(argv)
    try:
        cfg = config.load()
        modules = load_modules(cfg)
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1

    overrides = load_overrides(modules)
    hooks = load_hooks(modules)
    relative, symbolic, unresolved = resolve_relocations(modules)
    entries = [entry_points(cfg, m) for m in modules]
    setjmps = setjmp_entries(modules, symbolic, entries)
    results = [translate(cfg, m, overrides[m.name], entries[i], setjmps[i], hooks[m.name])
               for i, m in enumerate(modules)]
    print(f"== relocations: " + ", ".join(f"{m.name} {len(relative[i]):,} relative + {len(symbolic[i]):,} symbolic"
                                          for i, m in enumerate(modules)))
    print(f"   weak symbols defined nowhere (-> 0): {dict(unresolved)}")
    if args.report:
        return 0

    out = cfg.work_dir / "recomp"
    out.mkdir(parents=True, exist_ok=True)
    header = "/* Generated by recomp.translate from the owner's copy of the game. Do not commit. */\n"
    protos, tables, entries = [], [], []
    nfiles_total = 0
    for m, (funcs, sources) in zip(modules, results):
        nfiles = (len(sources) + CHUNK - 1) // CHUNK
        nfiles_total += nfiles
        for old in out.glob(f"{m.name}_*.c"):
            if old.stem[len(m.name) + 1:].isdigit() and int(old.stem[len(m.name) + 1:]) >= nfiles:
                old.unlink()
        for i in range(0, len(sources), CHUNK):
            write_if_changed(out / f"{m.name}_{i // CHUNK:03}.c",
                             f"{header}#include \"funcs.h\"\n\n" + "\n\n".join(sources[i:i + CHUNK]) + "\n")
        protos += [f"void {function.fname(fn.entry, m.prefix)}(CPUState *ctx);" for fn in funcs]
        rows = ",\n".join(f"    {{{fn.entry:#x}, {function.fname(fn.entry, m.prefix)}}}" for fn in funcs)
        tables.append(f"static const FuncEntry funcs_{m.name}[] = {{\n{rows}\n}};\n")
        entries.append(f'    {{"{m.name}", &{m.base}, {m.mem_size:#x}, funcs_{m.name}, {len(funcs)}}}')
    write_if_changed(out / "funcs.h", f"{header}#pragma once\n#include \"yw4rt.h\"\n" + "\n".join(protos) + "\n")
    write_if_changed(out / "func_table.c",
                     f"{header}#include \"funcs.h\"\n\n" + "\n".join(tables) +
                     "\nconst Yw4CodeModule yw4_code[] = {\n" + ",\n".join(entries) + "\n};\n"
                     f"const size_t yw4_ncode = {len(modules)};\n")
    npdm = (cfg.exefs_dir / "main.npdm").read_bytes()
    write_if_changed(out / "image_info.c", header + image_info(modules, relative, symbolic, npdm, guest_symbols(modules)))
    print(f"wrote {out} ({nfiles_total} source files + func_table.c, image_info.c)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
