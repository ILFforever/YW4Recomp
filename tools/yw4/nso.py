"""Parse an NSO module and turn it into a flat memory image.

NSO is the Switch's executable format for ExeFS modules (rtld, main, sdk,
subsdk*). Layout of the 0x100-byte header (all little-endian u32 unless noted):

    0x00 magic "NSO0"           0x04 version        0x0C flags
    0x10 text  {file_off, mem_off, size}            0x1C module name offset
    0x20 ro    {file_off, mem_off, size}            0x2C module name size
    0x30 data  {file_off, mem_off, size}            0x3C bss size
    0x40 module_id (build ID, 0x20 bytes)
    0x60 text/ro/data size *in the file* (compressed size, 3 x u32)
    0x88 .api_info, 0x90 .dynstr, 0x98 .dynsym  {offset, size} relative to ro
    0xA0 SHA-256 of decompressed text, ro, data (3 x 0x20 bytes)

flags: bits 0-2 = text/ro/data is LZ4-compressed, bits 3-5 = check its hash.

The "flat image" is what the loader puts in memory: each segment at its
mem_off, gaps zero-filled, .bss not included. We hash that image to identify
the build, because it does not depend on how the NSO happened to be
compressed or which tool converted it.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

from . import lz4_block

HEADER_SIZE = 0x100
SEGMENT_NAMES = ("text", "ro", "data")


class NsoError(Exception):
    pass


@dataclass(frozen=True)
class Segment:
    name: str
    file_offset: int
    mem_offset: int
    size: int            # decompressed size
    file_size: int       # size in the file (compressed size if compressed)
    compressed: bool
    check_hash: bool
    sha256: bytes        # expected hash of decompressed bytes, from the header


@dataclass(frozen=True)
class Nso:
    version: int
    flags: int
    segments: tuple[Segment, Segment, Segment]
    bss_size: int
    build_id: bytes
    api_info: tuple[int, int]
    dynstr: tuple[int, int]
    dynsym: tuple[int, int]


def parse_header(blob: bytes) -> Nso:
    if len(blob) < HEADER_SIZE:
        raise NsoError("file is smaller than an NSO header")
    if blob[:4] != b"NSO0":
        raise NsoError(f"bad magic {blob[:4]!r}, expected b'NSO0'")

    version, _, flags = struct.unpack_from("<3I", blob, 0x04)
    file_sizes = struct.unpack_from("<3I", blob, 0x60)
    segments = []
    for idx, name in enumerate(SEGMENT_NAMES):
        file_off, mem_off, size = struct.unpack_from("<3I", blob, 0x10 + idx * 0x10)
        segments.append(Segment(
            name=name,
            file_offset=file_off,
            mem_offset=mem_off,
            size=size,
            file_size=file_sizes[idx],
            compressed=bool(flags & (1 << idx)),
            check_hash=bool(flags & (1 << (idx + 3))),
            sha256=blob[0xA0 + idx * 0x20:0xC0 + idx * 0x20],
        ))

    return Nso(
        version=version,
        flags=flags,
        segments=tuple(segments),
        bss_size=struct.unpack_from("<I", blob, 0x3C)[0],
        build_id=blob[0x40:0x60],
        api_info=struct.unpack_from("<2I", blob, 0x88),
        dynstr=struct.unpack_from("<2I", blob, 0x90),
        dynsym=struct.unpack_from("<2I", blob, 0x98),
    )


def decompress_segment(blob: bytes, seg: Segment) -> bytes:
    raw = blob[seg.file_offset:seg.file_offset + seg.file_size]
    if len(raw) != seg.file_size:
        raise NsoError(f".{seg.name} runs past end of file")
    try:
        data = lz4_block.decompress(raw, seg.size) if seg.compressed else raw
    except ValueError as e:
        raise NsoError(f".{seg.name}: {e}") from None
    if len(data) != seg.size:
        raise NsoError(f".{seg.name} is {len(data)} bytes, header says {seg.size}")
    if seg.check_hash and hashlib.sha256(data).digest() != seg.sha256:
        raise NsoError(f".{seg.name} SHA-256 does not match the NSO header (corrupt file?)")
    return data


def flat_image(blob: bytes, nso: Nso | None = None) -> bytes:
    """Decompress all segments and lay them out at their memory offsets."""
    nso = nso or parse_header(blob)
    end = max(s.mem_offset + s.size for s in nso.segments)
    image = bytearray(end)
    placed: list[tuple[int, int, str]] = []
    for seg in nso.segments:
        lo, hi = seg.mem_offset, seg.mem_offset + seg.size
        for plo, phi, pname in placed:
            if lo < phi and plo < hi:
                raise NsoError(f".{seg.name} overlaps .{pname} in memory")
        image[lo:hi] = decompress_segment(blob, seg)
        placed.append((lo, hi, seg.name))
    return bytes(image)


def build_id_hex(build_id: bytes) -> str:
    """Build ID as hex with trailing zero bytes trimmed (it is usually 0x14 bytes
    of GNU build-id padded to 0x20)."""
    return build_id.rstrip(b"\0").hex().upper()
