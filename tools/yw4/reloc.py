"""Find and apply a module's dynamic relocations.

Switch modules are position-independent. In the flat image most pointers
(vtable entries, typeinfo links, function-pointer tables) are 0; the loader
writes them at boot from the relocation tables. To see what a pointer points
at, we have to replay those relocations ourselves.

How to find the tables:
    image[4:8]  u32 offset of the MOD0 header (from image start)
    MOD0        "MOD0", then i32 offsets relative to MOD0: dynamic, bss start,
                bss end, eh_frame_hdr start, eh_frame_hdr end, module object
    .dynamic    (tag u64, value u64) pairs; we need DT_RELA/DT_RELASZ (data
                relocations) and DT_JMPREL/DT_PLTRELSZ (PLT slots)
    Elf64_Rela  u64 offset, u64 info (symbol index << 32 | type), i64 addend

Relocation types that matter on AArch64:
    R_AARCH64_RELATIVE (1027)  *offset = module base + addend   (internal pointer)
    R_AARCH64_ABS64    (257)   *offset = symbol + addend
    R_AARCH64_GLOB_DAT (1025)  *offset = symbol                 (GOT entry)
    R_AARCH64_JUMP_SLOT(1026)  *offset = symbol                 (PLT slot)
We relocate as if the module were loaded at address 0, so a RELATIVE pointer's
value is simply its addend = an offset into the flat image.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027

DT_NULL, DT_PLTRELSZ, DT_RELA, DT_RELASZ, DT_JMPREL = 0, 2, 7, 8, 23


class RelocError(Exception):
    pass


@dataclass(frozen=True)
class Mod0:
    offset: int
    dynamic: int
    bss_start: int
    bss_end: int
    eh_frame_hdr: tuple[int, int]


@dataclass(frozen=True)
class Reloc:
    offset: int
    type: int
    sym: int      # index into .dynsym (0 for RELATIVE)
    addend: int


def read_mod0(image: bytes) -> Mod0:
    base = struct.unpack_from("<I", image, 4)[0]
    if image[base:base + 4] != b"MOD0":
        raise RelocError(f"no MOD0 header at 0x{base:x}")
    dyn, bss_s, bss_e, eh_s, eh_e = struct.unpack_from("<5i", image, base + 4)
    return Mod0(base, base + dyn, base + bss_s, base + bss_e, (base + eh_s, base + eh_e))


def read_dynamic(image: bytes, mod0: Mod0) -> dict[int, int]:
    tags: dict[int, int] = {}
    pos = mod0.dynamic
    while pos + 16 <= len(image):
        tag, val = struct.unpack_from("<QQ", image, pos)
        if tag == DT_NULL:
            break
        tags.setdefault(tag, val)
        pos += 16
    return tags


def read_relocs(image: bytes) -> list[Reloc]:
    dyn = read_dynamic(image, read_mod0(image))
    relocs = []
    for start_tag, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
        if start_tag not in dyn:
            continue
        start, size = dyn[start_tag], dyn[size_tag]
        for pos in range(start, start + size, 24):
            off, info, addend = struct.unpack_from("<QQq", image, pos)
            relocs.append(Reloc(off, info & 0xFFFFFFFF, info >> 32, addend))
    return relocs


def pointer_map(relocs: list[Reloc]) -> tuple[dict[int, int], dict[int, tuple[int, int]]]:
    """Return (internal, external).

    internal: slot offset -> target offset in this module (RELATIVE).
    external: slot offset -> (symbol index, addend) for slots filled from another module.
    """
    internal: dict[int, int] = {}
    external: dict[int, tuple[int, int]] = {}
    for r in relocs:
        if r.type == R_AARCH64_RELATIVE:
            internal[r.offset] = r.addend
        elif r.type in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
            external[r.offset] = (r.sym, r.addend)
    return internal, external
