"""Phase 0 recon: measure main and write tables for docs/recon.md.

    python -m tools.yw4.recon

Needs `python -m tools.yw4.ingest` first. Writes to <work_dir>/recon/:
    imports.json / imports.md   main's imports from sdk, grouped by library
    middleware.md               SDK MW+ tags and engine/middleware string hits
    rtti.json / rtti.md         C++ typeinfo + vtable survey, namespace counts
    code.md                     instruction counts (direct vs indirect branches), unwind-table functions
    render_classes.txt          RTTI class names that look renderer-related
    romfs.md                    RomFS layout and file types, including inside CPK archives

Outputs contain names and counts derived from the game, so they stay in
work/ (gitignored). docs/recon.md quotes the summaries.
"""

from __future__ import annotations

import collections
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

from . import config, cpk, ingest, reloc, symbols

TYPEINFO_VTABLES = {
    "_ZTVN10__cxxabiv117__class_type_infoE": "base",
    "_ZTVN10__cxxabiv120__si_class_type_infoE": "single",
    "_ZTVN10__cxxabiv121__vmi_class_type_infoE": "multiple",
}

MIDDLEWARE_KEYWORDS = {
    "CRI (ADX2 / Atom audio)": [b"CRIWARE", b"#CRIATOM", b"ADX2"],
    "CRI Sofdec / Mana (video)": [b"Sofdec", b"CriMana"],
    "CRI File System": [b"CRIFSMEM", b"CriFs"],
    "NVIDIA PhysX": [b"PhysX"],
    "Lua": [b"Lua 5.", b"lua_"],
    "zlib": [b"zlib"],
    "libcurl": [b"libcurl", b"curl_"],
    "Nintendo NEX": [b"nex::"],
    "Nintendo Pia": [b"pia::"],
    "NVN": [b"nvnDevice", b"nvnQueue"],
    "Havok": [b"Havok", b"hkBase"],
    "Wwise": [b"Wwise", b"AkSoundEngine"],
    "FMOD": [b"FMOD"],
    "Unity": [b"UnityEngine", b"il2cpp"],
    "Unreal": [b"UnrealEngine", b"UE4"],
    "Bink": [b"Bink"],
    "Effekseer": [b"Effekseer"],
    "FreeType": [b"FreeType"],
    "libpng": [b"libpng"],
}

RENDER_WORDS = re.compile(
    r"Render|Draw|Shader|Material|Mesh|Model|Camera|Texture|Light|Shadow|Post|Bloom|"
    r"Gfx|Graphic|Nvn|NVN|Frame|Sprite|Particle|Effect|Fog|Blur|Viewport|Scene|Layer|Gpu|GPU",
)


def load_main(cfg: config.Config) -> tuple[bytes, dict]:
    """main's flat image from work/exefs/ and the module info. Refuses (ConfigError) if the
    image doesn't match target.toml, so no tool can analyse a different build (rule 3)."""
    exefs = cfg.work_dir / "exefs"
    mods = {m["name"]: m for m in json.loads((exefs / "modules.json").read_text())}
    image = (exefs / "main.bin").read_bytes()
    want = ingest.load_target().get("image_sha256", "")
    if not want or hashlib.sha256(image).hexdigest() != want:
        raise config.ConfigError(f"{exefs / 'main.bin'} does not match target.toml; "
                                 "run python -m tools.yw4.ingest first")
    return image, mods


def dynsym_names(image: bytes, info: dict) -> list[str]:
    """Names in .dynsym index order (including the empty ones), for relocation lookups."""
    ro = info["segments"]["ro"]["mem_offset"]
    sym_off, sym_size = info["dynsym"]
    str_off, str_size = info["dynstr"]
    strtab = image[ro + str_off:ro + str_off + str_size]
    names = []
    for pos in range(ro + sym_off, ro + sym_off + sym_size, 24):
        n = struct.unpack_from("<I", image, pos)[0]
        names.append(strtab[n:strtab.find(b"\0", n)].decode("utf-8", "replace"))
    return names


def c_string(image: bytes, off: int, limit: int = 512) -> str:
    end = image.find(b"\0", off, off + limit)
    return image[off:end].decode("utf-8", "replace") if end >= 0 else ""


# --------------------------------------------------------------------------- imports

def import_group(mangled: str, demangled: str) -> str:
    if mangled.startswith("nvn"):
        return "nvn (bootstrap)"
    if demangled.startswith("nv::"):
        return "nv (graphics init)"
    m = re.match(r"(nn::\w+)", demangled)
    if m:
        return m.group(1)
    for prefix, group in (("nnos", "nn::os"), ("nnsocket", "nn::socket"), ("nnssl", "nn::ssl"),
                          ("nnResult", "nn::result"), ("nndetailDiag", "nn::diag"),
                          ("__nnmusl", "libc (musl)")):
        if mangled.startswith(prefix):
            return group
    if demangled.startswith("std::") or "std::__1" in demangled:
        return "libc++"
    if mangled.startswith("_Z") or mangled.startswith(("__cxa", "__gxx", "__dynamic_cast", "_Unwind")):
        return "C++ runtime (new/delete, RTTI, exceptions)"
    if mangled.startswith("__rel_"):
        return "linker markers (not real imports)"
    return "libc (musl)"


def survey_imports(image: bytes, info: dict, sdk_exports: set[str]) -> dict:
    imps = [s for s in symbols.read_dynsym(image, info) if not s.defined]
    dm = symbols.demangle([s.name for s in imps])
    groups: dict[str, list[str]] = collections.defaultdict(list)
    for s in imps:
        groups[import_group(s.name, dm[s.name])].append(dm[s.name])
    unresolved = [s.name for s in imps if s.name not in sdk_exports and not s.name.startswith("__rel_")]
    return {
        "total": len(imps),
        "functions": sum(s.type == "func" for s in imps),
        "objects": sum(s.type == "object" for s in imps),
        "unresolved_by_sdk": unresolved,
        "groups": {k: sorted(v) for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))},
    }


def imports_md(imp: dict) -> str:
    lines = [f"main imports {imp['total']} symbols ({imp['functions']} functions, {imp['objects']} data). "
             f"Not provided by sdk: {len(imp['unresolved_by_sdk'])}.", "",
             "| Group | Count |", "|---|---|"]
    lines += [f"| `{g}` | {len(v)} |" for g, v in imp["groups"].items()]
    lines.append("")
    for g, v in imp["groups"].items():
        lines.append(f"### {g} ({len(v)})")
        lines += [f"- `{n}`" for n in v]
        lines.append("")
    return "\n".join(lines)


# --------------------------------------------------------------------------- middleware

def survey_middleware(image: bytes) -> str:
    tags = sorted({m.decode() for m in re.findall(rb"SDK MW\+[\x20-\x7e]{3,80}", image)})
    lines = ["Middleware tags (`SDK MW+Vendor+Library-Version`):", ""]
    lines += [f"- `{t}`" for t in tags] + ["", "| Library | Hits | Found |", "|---|---|---|"]
    for name, kws in MIDDLEWARE_KEYWORDS.items():
        hits = sum(image.count(k) for k in kws)
        lines.append(f"| {name} | {hits} | {'yes' if hits else 'no'} |")
    m = re.search(rb"Lua 5\.\d(\.\d)?", image)
    if m:
        lines += ["", f"Lua version string: `{m.group().decode()}`"]
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------- RTTI / vtables

def survey_rtti(image: bytes, info: dict) -> dict:
    names = dynsym_names(image, info)
    internal, external = reloc.pointer_map(reloc.read_relocs(image))
    text_end = info["segments"]["text"]["size"]

    # A typeinfo object starts with a pointer to (one of the __class_type_info vtables + 0x10),
    # followed by a pointer to its mangled name string.
    typeinfos: dict[int, tuple[str, str]] = {}
    for slot, (sym, _addend) in external.items():
        kind = TYPEINFO_VTABLES.get(names[sym])
        if kind and slot + 8 in internal:
            typeinfos[slot] = (kind, c_string(image, internal[slot + 8]))

    # Typeinfo objects also point at other typeinfos (their base classes). Mark every
    # byte range a typeinfo occupies so those links are not mistaken for vtables:
    #   base: vptr, name                          (0x10 bytes)
    #   single: vptr, name, base*                 (0x18)
    #   multiple: vptr, name, u32 flags, u32 n, n x {base*, offset_flags}  (0x18 + 0x10 n)
    inside_typeinfo: set[int] = set()
    for ti, (kind, _name) in typeinfos.items():
        size = {"base": 0x10, "single": 0x18}.get(kind)
        if size is None:
            size = 0x18 + 0x10 * struct.unpack_from("<I", image, ti + 0x14)[0]
        inside_typeinfo.update(range(ti, ti + size, 8))

    # A vtable is [offset_to_top][typeinfo*][fn0][fn1]...; find slots that point at a typeinfo.
    pure_virtual = {i for i, n in enumerate(names) if n == "__cxa_pure_virtual"}
    vtables = []
    for slot, target in internal.items():
        if target not in typeinfos or slot in inside_typeinfo:
            continue
        offset_to_top = struct.unpack_from("<q", image, slot - 8)[0]
        n = 0
        pos = slot + 8
        while True:
            if pos in internal and internal[pos] < text_end:
                n += 1
            elif pos in external and external[pos][0] in pure_virtual:
                n += 1
            else:
                break
            pos += 8
        vtables.append((slot - 8, typeinfos[target][1], offset_to_top, n))

    mangled = sorted({name for _, name in typeinfos.values() if name})
    dm = symbols.demangle(["_ZTS" + n for n in mangled])
    pretty = {n: dm["_ZTS" + n].removeprefix("typeinfo name for ") for n in mangled}

    ns = collections.Counter()
    for p in pretty.values():
        top = re.split(r"::|<", p, maxsplit=1)
        ns[top[0] if len(top) > 1 and "::" in p.split("<", 1)[0] else "(global)"] += 1

    primary = [v for v in vtables if v[2] == 0]
    return {
        "typeinfo_count": len(typeinfos),
        "typeinfo_by_kind": collections.Counter(k for k, _ in typeinfos.values()),
        "class_names": sorted(pretty.values()),
        "namespaces": ns.most_common(),
        "vtable_count": len(vtables),
        "primary_vtables": len(primary),
        "secondary_vtables": len(vtables) - len(primary),
        "vtable_slots_total": sum(v[3] for v in vtables),
        "vtable_slots_max": max((v[3] for v in vtables), default=0),
        "pure_virtual_slots": sum(1 for v in external.values() if v[0] in pure_virtual),
    }


def rtti_md(r: dict) -> str:
    kinds = r["typeinfo_by_kind"]
    lines = [
        f"- Classes with RTTI: **{r['typeinfo_count']}** "
        f"(base {kinds.get('base', 0)}, single inheritance {kinds.get('single', 0)}, "
        f"multiple inheritance {kinds.get('multiple', 0)})",
        f"- Vtables found: **{r['vtable_count']}** ({r['primary_vtables']} primary, "
        f"{r['secondary_vtables']} secondary from multiple inheritance)",
        f"- Virtual function slots: **{r['vtable_slots_total']}** total, largest vtable "
        f"{r['vtable_slots_max']} slots; {r['pure_virtual_slots']} pure-virtual slots",
        "", "| Top-level namespace | Classes |", "|---|---|",
    ]
    lines += [f"| `{n}` | {c} |" for n, c in r["namespaces"][:30]]
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------- code

def eh_frame_function_starts(image: bytes) -> list[int]:
    """Function start offsets from .eh_frame_hdr's binary-search table.

    Header: u8 version (1), u8 eh_frame_ptr encoding, u8 count encoding,
    u8 table encoding, then the encoded eh_frame pointer and FDE count, then
    (initial_location, fde_address) pairs. We support what this toolchain emits:
    count as udata4 (0x03) and table entries as datarel sdata4 (0x3b), i.e.
    signed 32-bit offsets from the start of .eh_frame_hdr.
    """
    start = reloc.read_mod0(image).eh_frame_hdr[0]
    version, _ptr_enc, count_enc, table_enc = image[start:start + 4]
    if version != 1 or count_enc != 0x03 or table_enc != 0x3B:
        raise reloc.RelocError(f"unsupported .eh_frame_hdr encoding {image[start:start + 4].hex()}")
    count = struct.unpack_from("<I", image, start + 8)[0]
    return [start + struct.unpack_from("<i", image, start + 12 + 8 * i)[0] for i in range(count)]


def address_taken_functions(image: bytes, text_size: int) -> set[int]:
    """Targets of internal pointers into .text (vtables, callback tables, ...)."""
    internal, _ = reloc.pointer_map(reloc.read_relocs(image))
    return {t for t in internal.values() if t < text_size}


def survey_code(image: bytes, info: dict) -> dict:
    text = image[:info["segments"]["text"]["size"]]
    words = struct.unpack(f"<{len(text) // 4}I", text[:len(text) // 4 * 4])
    c = collections.Counter()
    for w in words:
        if w & 0xFC000000 == 0x94000000:
            c["bl"] += 1                      # direct call
        elif w & 0xFC000000 == 0x14000000:
            c["b"] += 1                       # direct jump / tail call
        elif w & 0xFFFFFC1F == 0xD63F0000:
            c["blr"] += 1                     # indirect call (virtual calls, function pointers)
        elif w & 0xFFFFFC1F == 0xD61F0000:
            c["br"] += 1                      # indirect jump (switch tables, indirect tail calls)
        elif w & 0xFFFFFC1F == 0xD65F0000:
            c["ret"] += 1
        elif w & 0xFFFFFC1F == 0xD4000001:
            c["svc"] += 1                     # syscall (should be ~0 in main; sdk does them)
        elif w & 0x9F000000 == 0x90000000:
            c["adrp"] += 1
        elif w & 0xFF000000 == 0x58000000:
            c["ldr_literal"] += 1
        elif w & 0x3F000000 == 0x08000000:   # load/store exclusive and ordered
            o2, load, o1 = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1
            if o2 == 0:
                c["ldxr" if load else "stxr"] += 1        # exclusive pair (v8.0 atomics)
            elif o1 == 0:
                c["ldar" if load else "stlr"] += 1        # acquire load / release store
            else:
                c["cas"] += 1                             # v8.1 CAS (Switch A57 lacks it)
        elif w & 0x3F200C00 == 0x38200000:
            c["lse"] += 1                                 # v8.1 LSE atomics (should be 0)
        elif w & 0xFFFFF0FF == 0xD50330BF:
            c["dmb"] += 1
        elif w & 0xFFFFFFE0 == 0xD53BD040:
            c["mrs_tpidr"] += 1                           # thread-local storage pointer read
        # FP/SIMD data-processing: bits 28-25 = x111
        if (w >> 25) & 0x7 == 0x7:
            c["fp_simd"] += 1
    mod0 = reloc.read_mod0(image)
    eh = set(eh_frame_function_starts(image))
    taken = address_taken_functions(image, len(text))
    return {"instructions": len(words), "counts": dict(c),
            "eh_frame_functions": len(eh),
            "address_taken": len(taken),
            "address_taken_with_unwind": len(taken & eh),
            "bss_size": mod0.bss_end - mod0.bss_start}


def code_md(code: dict) -> str:
    c = code["counts"]
    n = code["instructions"]
    lines = [
        f"- Instructions in .text: **{n:,}**",
        f"- Functions with unwind info (.eh_frame_hdr): **{code['eh_frame_functions']:,}**",
        f"- Address-taken functions (relocations into .text): **{code['address_taken']:,}**, "
        f"of which {code['address_taken_with_unwind']:,} are also unwind-table starts",
        f"- .bss size: {code['bss_size']:,} bytes",
        "", "| Kind | Count | Per 1000 instructions |", "|---|---|---|",
    ]
    for k in ("bl", "b", "blr", "br", "ret", "svc", "adrp", "ldr_literal", "fp_simd",
              "ldxr", "stxr", "ldar", "stlr", "cas", "lse", "dmb", "mrs_tpidr"):
        lines.append(f"| `{k}` | {c.get(k, 0):,} | {1000 * c.get(k, 0) / n:.1f} |")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------- RomFS

def survey_romfs(romfs: Path) -> str:
    """File types on disk and inside every CPK archive (names and headers only)."""
    disk_n, disk_b = collections.Counter(), collections.Counter()
    inner_n, inner_b, inner_c = collections.Counter(), collections.Counter(), collections.Counter()
    top_b = collections.Counter()
    archives, errors = 0, []
    for p in romfs.rglob("*"):
        if not p.is_file():
            continue
        size = p.stat().st_size
        ext = p.suffix.lower() or "(none)"
        disk_n[ext] += 1
        disk_b[ext] += size
        rel = p.relative_to(romfs).parts
        top_b["/".join(rel[:2]) if len(rel) > 2 else rel[0]] += size
        if ext != ".cpk":
            continue
        archives += 1
        try:
            info = cpk.list_files(p)
        except cpk.CpkError as e:
            errors.append(f"{p.name}: {e}")
            continue
        for f in info.files:
            iext = Path(f.path).suffix.lower() or "(none)"
            inner_n[iext] += 1
            inner_b[iext] += f.extract_size
            inner_c[iext] += f.compressed

    mb = lambda b: f"{b / 2**20:,.1f}"
    lines = ["Top level:", "", "| Folder | MB |", "|---|---|"]
    lines += [f"| `{k}` | {mb(v)} |" for k, v in top_b.most_common()]
    lines += ["", "Files on disk:", "", "| Ext | Files | MB |", "|---|---|---|"]
    lines += [f"| `{e}` | {c} | {mb(disk_b[e])} |" for e, c in disk_n.most_common()]
    lines += ["", f"Inside {archives} CPK archives ({sum(inner_n.values()):,} files, "
              f"{len(errors)} unreadable):", "",
              "| Ext | Files | MB uncompressed | % compressed |", "|---|---|---|---|"]
    lines += [f"| `{e}` | {c} | {mb(inner_b[e])} | {100 * inner_c[e] // c}% |"
              for e, c in inner_n.most_common()]
    lines += [f"- {e}" for e in errors[:20]]
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------- main

def main() -> int:
    try:
        cfg = config.load()
        image, mods = load_main(cfg)
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    sdk_image = (cfg.work_dir / "exefs" / "sdk.bin").read_bytes()
    sdk_exports = {s.name for s in symbols.read_dynsym(sdk_image, mods["sdk"]) if s.defined}

    out = cfg.work_dir / "recon"
    out.mkdir(parents=True, exist_ok=True)

    imp = survey_imports(image, mods["main"], sdk_exports)
    (out / "imports.json").write_text(json.dumps(imp, indent=2))
    (out / "imports.md").write_text(imports_md(imp))

    (out / "middleware.md").write_text(survey_middleware(image))

    rtti = survey_rtti(image, mods["main"])
    (out / "rtti.json").write_text(json.dumps(rtti, indent=2))
    (out / "rtti.md").write_text(rtti_md(rtti))
    render = [n for n in rtti["class_names"] if RENDER_WORDS.search(n)]
    (out / "render_classes.txt").write_text("\n".join(render) + "\n")

    code = survey_code(image, mods["main"])
    (out / "code.md").write_text(code_md(code))

    (out / "romfs.md").write_text(survey_romfs(cfg.romfs_dir))

    print(f"imports: {imp['total']}  classes: {rtti['typeinfo_count']}  vtables: {rtti['vtable_count']}  "
          f"render-ish classes: {len(render)}  blr: {code['counts'].get('blr', 0):,}  "
          f"eh_frame functions: {code['eh_frame_functions']:,}")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
