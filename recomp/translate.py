"""Translate all of main to C (decision 0007).

    python -m recomp.translate              write work/recomp/
    python -m recomp.translate --report     statistics only, write nothing

Entry points: .eh_frame_hdr starts, address-taken targets, Ghidra's function
entries and every BL target. Output (derived from the game's code, so it stays
in work/ and is never committed):

    work/recomp/funcs.h          prototypes of every f_<offset>
    work/recomp/main_NNN.c       the functions, CHUNK per file (for a parallel build)
    work/recomp/func_table.c     yw4_funcs[], the sorted table yw4_resolve searches
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import struct
import sys

from tools.yw4 import config, recon, reloc, symbols

from . import coverage, function

CHUNK = 500


def load_code(cfg: config.Config) -> tuple[function.Code, dict]:
    image, mods = recon.load_main(cfg)  # refuses a build that doesn't match target.toml
    return function.Code(image, mods["main"]["segments"]["text"]["size"]), mods


def entry_points(cfg: config.Config, code: function.Code) -> set[int]:
    entries = set(recon.eh_frame_function_starts(code.data))
    entries |= recon.address_taken_functions(code.data, code.text_end)
    ghidra = json.loads((cfg.work_dir / "ghidra" / "main.functions.json").read_text())["functions"]
    entries |= {f["entry"] for f in ghidra}
    for off in range(0, code.text_end, 4):  # BL targets
        f = function.decode_flow(code.word(off), off)
        if f and f.kind == "bl" and code.is_code(f.target):
            entries.add(f.target)
    return {e for e in entries if code.is_code(e)}


R_RELATIVE, SYMBOL_RELOCS = reloc.R_AARCH64_RELATIVE, (reloc.R_AARCH64_ABS64, reloc.R_AARCH64_GLOB_DAT,
                                                   reloc.R_AARCH64_JUMP_SLOT)
STT_OBJECT, STT_FUNC = 1, 2
DT_INIT = 0xC


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


def image_info(cfg: config.Config, code: function.Code, mods: dict) -> str:
    """Loader tables (decision 0010): image size, relocations, imports, entry points."""
    image, info = code.data, mods["main"]
    syms = raw_dynsym(image, info)
    sdk = (cfg.work_dir / "exefs" / "sdk.bin").read_bytes()
    sdk_exports = {s.name for s in symbols.read_dynsym(sdk, mods["sdk"]) if s.defined}
    relocs = reloc.read_relocs(image)
    unknown = {r.type for r in relocs} - {R_RELATIVE, *SYMBOL_RELOCS}
    if unknown:
        raise RuntimeError(f"unsupported relocation types {unknown}")
    used = sorted({r.sym for r in relocs if r.type in SYMBOL_RELOCS})
    index = {sym: i for i, sym in enumerate(used)}
    imports = []
    for sym in used:
        name, typ, shndx, _ = syms[sym]
        if shndx != 0:
            raise RuntimeError(f"symbol relocation against a defined symbol {name}: not handled yet")
        if name not in sdk_exports:
            kind = "YW4_IMPORT_NULL"  # weak and undefined everywhere: resolves to 0
        else:
            kind = "YW4_IMPORT_DATA" if typ == STT_OBJECT else "YW4_IMPORT_FUNC"
        imports.append((name, kind))
    defined = {name: value for name, _, shndx, value in syms if shndx != 0}
    mod0 = reloc.read_mod0(image)
    mem_size = (max(len(image), mod0.bss_end) + 0xFFF) & ~0xFFF
    init = reloc.read_dynamic(image, mod0)[DT_INIT]

    rel = ",\n".join(f"    {{{r.offset:#x}, {r.addend:#x}}}" for r in relocs if r.type == R_RELATIVE)
    srel = ",\n".join(f"    {{{r.offset:#x}, {index[r.sym]}, {r.type}, {r.addend:#x}}}"
                       for r in relocs if r.type in SYMBOL_RELOCS)
    imp = ",\n".join(f'    {{"{n}", {k}}}' for n, k in imports)
    stubs = "\n".join(f"static void imp_{i}(CPUState *ctx) {{ yw4_import_call(ctx, {i}); }}" for i in range(len(imports)))
    fns = ", ".join(f"imp_{i}" for i in range(len(imports)))
    return (f'#include "yw4load.h"\n\n'
            f'const char yw4_image_sha256[65] = "{hashlib.sha256(image).hexdigest()}";\n'
            f"const uint64_t yw4_image_file_size = {len(image)}, yw4_image_mem_size = {mem_size:#x};\n"
            f"const uint64_t yw4_entry_startup = {defined['nninitStartup']:#x}, yw4_entry_init = {init:#x}, "
            f"yw4_entry_main = {defined['nnMain']:#x};\n\n"
            f"const Yw4Relative yw4_relative[] = {{\n{rel}\n}};\n"
            f"const size_t yw4_nrelative = {sum(r.type == R_RELATIVE for r in relocs)};\n\n"
            f"const Yw4SymReloc yw4_symrelocs[] = {{\n{srel}\n}};\n"
            f"const size_t yw4_nsymrelocs = {sum(r.type in SYMBOL_RELOCS for r in relocs)};\n\n"
            f"const Yw4Import yw4_imports[] = {{\n{imp}\n}};\n"
            f"const size_t yw4_nimports = {len(imports)};\n\n"
            f"{stubs}\n\nconst GuestFn yw4_import_fns[] = {{{fns}}};\n")


def write_if_changed(path, text: str) -> None:
    """Keeps the timestamp of unchanged files, so recomp.build only recompiles what changed."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", action="store_true", help="print statistics, write nothing")
    args = ap.parse_args(argv)
    try:
        cfg = config.load()
        code, mods = load_code(cfg)
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1

    entries = entry_points(cfg, code)
    funcs = [function.discover(code, e, entries) for e in sorted(entries)]
    stats = function.EmitStats()
    sources = [function.emit_function(code, fn, entries, coverage.subgroup, stats) for fn in funcs]

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
    print(f"functions:               {len(funcs):,}")
    print(f"instructions translated: {translated:,} ({unique:,} unique, "
          f"{translated - unique:,} duplicated into more than one function)")
    print(f"untranslated (traps):    {unimpl:,} ({100 * unimpl / translated:.1f}%)")
    for g, n in sorted(stats.unimplemented.items(), key=lambda kv: -kv[1]):
        print(f"    {n:10,}  {g}")
    print(f"BR: {dict(brs)}")
    print(f"flow problems: {len(problems)}")
    for p in problems[:10]:
        print(f"    {p}")
    if args.report:
        return 0

    out = cfg.work_dir / "recomp"
    out.mkdir(parents=True, exist_ok=True)
    nfiles = (len(sources) + CHUNK - 1) // CHUNK
    for old in out.glob("main_*.c"):
        if int(old.stem[5:]) >= nfiles:
            old.unlink()
    header = "/* Generated by recomp.translate from the owner's copy of main. Do not commit. */\n"
    protos = "\n".join(f"void {function.fname(fn.entry)}(CPUState *ctx);" for fn in funcs)
    write_if_changed(out / "funcs.h", f"{header}#pragma once\n#include \"yw4rt.h\"\n{protos}\n")
    for i in range(0, len(sources), CHUNK):
        write_if_changed(out / f"main_{i // CHUNK:03}.c",
                         f"{header}#include \"funcs.h\"\n\n" + "\n\n".join(sources[i:i + CHUNK]) + "\n")
    table = ",\n".join(f"    {{{fn.entry:#x}, {function.fname(fn.entry)}}}" for fn in funcs)
    write_if_changed(out / "func_table.c",
                     f"{header}#include \"funcs.h\"\n\nconst FuncEntry yw4_funcs[] = {{\n{table}\n}};\n"
                     f"const size_t yw4_nfuncs = {len(funcs)};\n")
    write_if_changed(out / "image_info.c", header + image_info(cfg, code, mods))
    print(f"wrote {out} ({nfiles} source files + func_table.c, image_info.c)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
