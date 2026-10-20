"""Read PFS0 archives (the container format of an NSP and of ExeFS).

Layout (little-endian):
    0x00 magic "PFS0"   0x04 u32 file count   0x08 u32 string table size   0x0C reserved
    then per file (0x18 bytes): u64 data offset, u64 size, u32 name offset, u32 reserved
    then the string table (NUL-terminated names)
    then the file data; data offsets are relative to the end of the string table.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO


class Pfs0Error(Exception):
    pass


@dataclass(frozen=True)
class Entry:
    name: str
    offset: int  # absolute offset in the archive
    size: int


def read_entries(f: BinaryIO) -> list[Entry]:
    f.seek(0)
    head = f.read(0x10)
    if len(head) < 0x10 or head[:4] != b"PFS0":
        raise Pfs0Error("not a PFS0 archive")
    count, strtab_size = struct.unpack_from("<II", head, 4)
    raw_entries = f.read(count * 0x18)
    strtab = f.read(strtab_size)
    if len(raw_entries) != count * 0x18 or len(strtab) != strtab_size:
        raise Pfs0Error("truncated PFS0 header")
    data_start = 0x10 + count * 0x18 + strtab_size

    entries = []
    for i in range(count):
        offset, size, name_off, _ = struct.unpack_from("<QQII", raw_entries, i * 0x18)
        end = strtab.find(b"\0", name_off)
        name = strtab[name_off:end if end >= 0 else None].decode("utf-8")
        entries.append(Entry(name, data_start + offset, size))
    return entries


def extract(f: BinaryIO, entry: Entry, dest: Path) -> None:
    f.seek(entry.offset)
    with dest.open("wb") as out:
        remaining = entry.size
        while remaining:
            chunk = f.read(min(remaining, 16 << 20))
            if not chunk:
                raise Pfs0Error(f"{entry.name} runs past end of archive")
            out.write(chunk)
            remaining -= len(chunk)
