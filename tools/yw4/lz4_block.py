"""LZ4 *block* format decoder (no frame header), as used by NSO segments.

Written by hand instead of depending on the `lz4` package: the format is
small, and owning it means we can explain every byte of the input pipeline.

A block is a list of sequences. Each sequence is:
    token        1 byte: high nibble = literal length, low nibble = match length - 4
    [lit ext]    if literal nibble == 15: extra bytes added until one is != 255
    literals     copied straight to the output
    offset       2 bytes little-endian: how far back in the output the match starts
    [match ext]  same extension scheme as literals
The last sequence stops after its literals (no offset, no match).
A match may overlap the bytes it is producing (offset < length); that is how
LZ4 encodes runs, e.g. offset 1 repeats the previous byte.
"""

from __future__ import annotations


def _read_length(src: bytes, i: int, base: int) -> tuple[int, int]:
    """Apply the 255-continuation extension to a nibble value of 15."""
    length = base
    if base == 15:
        while True:
            b = src[i]
            i += 1
            length += b
            if b != 255:
                break
    return length, i


def decompress(src: bytes, out_size: int) -> bytes:
    dst = bytearray()
    i = 0
    n = len(src)
    try:
        while i < n:
            token = src[i]
            i += 1

            lit_len, i = _read_length(src, i, token >> 4)
            if i + lit_len > n:
                raise ValueError("literal run past end of input")
            dst += src[i:i + lit_len]
            i += lit_len
            if i == n:
                break  # last sequence: literals only

            offset = src[i] | (src[i + 1] << 8)
            i += 2
            if offset == 0 or offset > len(dst):
                raise ValueError(f"bad match offset {offset} at output {len(dst)}")

            match_len, i = _read_length(src, i, token & 0x0F)
            match_len += 4

            start = len(dst) - offset
            if offset >= match_len:
                dst += dst[start:start + match_len]
            else:
                # Overlapping copy: the match repeats the last `offset` bytes.
                pattern = bytes(dst[start:])
                reps, rem = divmod(match_len, offset)
                dst += pattern * reps + pattern[:rem]
    except IndexError:
        raise ValueError("truncated LZ4 block") from None

    if len(dst) != out_size:
        raise ValueError(f"decompressed to {len(dst)} bytes, expected {out_size}")
    return bytes(dst)
