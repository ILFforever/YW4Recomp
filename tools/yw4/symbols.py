"""Read a module's dynamic symbol table from its flat image.

The NSO header tells us where .dynsym and .dynstr are, as offsets relative to
the start of the ro segment (see nso.py). Each .dynsym entry is an ELF64 Sym:

    u32 name (offset into .dynstr)  u8 info (bind << 4 | type)  u8 other
    u16 shndx (0 = undefined, i.e. imported)  u64 value  u64 size

An undefined symbol (shndx 0) is an import: the dynamic loader (rtld) fills it
in at boot from another module's exports. For main, those imports are the
complete list of SDK functions and data the game uses.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
from dataclasses import dataclass

STT_NAMES = {0: "notype", 1: "object", 2: "func", 3: "section", 4: "file", 6: "tls"}
STB_NAMES = {0: "local", 1: "global", 2: "weak"}


@dataclass(frozen=True)
class Symbol:
    name: str
    value: int
    size: int
    type: str
    bind: str
    defined: bool


def read_dynsym(image: bytes, module_info: dict) -> list[Symbol]:
    """module_info is one entry of work/exefs/modules.json."""
    ro = module_info["segments"]["ro"]["mem_offset"]
    sym_off, sym_size = module_info["dynsym"]
    str_off, str_size = module_info["dynstr"]
    strtab = image[ro + str_off:ro + str_off + str_size]

    syms = []
    for pos in range(ro + sym_off, ro + sym_off + sym_size, 24):
        name_off, info, _, shndx, value, size = struct.unpack_from("<IBBHQQ", image, pos)
        end = strtab.find(b"\0", name_off)
        name = strtab[name_off:end].decode("utf-8", "replace")
        if not name:
            continue  # entry 0 and section symbols
        syms.append(Symbol(
            name=name,
            value=value,
            size=size,
            type=STT_NAMES.get(info & 0xF, str(info & 0xF)),
            bind=STB_NAMES.get(info >> 4, str(info >> 4)),
            defined=shndx != 0,
        ))
    return syms


def demangle(names: list[str]) -> dict[str, str]:
    """Demangle Itanium C++ names with c++filt if it is on PATH; otherwise return them unchanged."""
    exe = shutil.which("c++filt") or shutil.which("llvm-cxxfilt")
    if not exe or not names:
        return {n: n for n in names}
    out = subprocess.run([exe], input="\n".join(names), capture_output=True,
                         text=True, check=True).stdout.splitlines()
    if len(out) != len(names):
        return {n: n for n in names}
    return dict(zip(names, out))
