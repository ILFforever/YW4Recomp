"""BC7 textures without an image library (players' builds have no Pillow): the Switch's block-linear layout undone,
and BC7 blocks decoded to RGBA in plain Python. Slow (a few thousand blocks a second), so callers decode only the
blocks they need (tools.yw4.wheel_icons).

The format is Khronos' ARB_texture_compression_bptc; the partition and anchor tables below are the format's own
(as in Pillow's src/libImaging/BcnDecode.c, which this follows; tests check the two agree pixel for pixel).
"""

from __future__ import annotations


def deswizzle_bc7(raw: bytes, w: int, h: int) -> bytearray:
    """Convert Switch Tegra block-linear BC7 texture into linear BC7 rows."""
    bw, bh_texel, bpb = 4, 4, 16
    wb = (w + bw - 1) // bw
    hb = (h + bh_texel - 1) // bh_texel
    gobs = (hb + 7) // 8
    bh = 0
    while (1 << bh) < gobs and bh < 4:
        bh += 1
    while bh > 0 and (8 << (bh - 1)) >= hb:
        bh -= 1
    width_gobs = (wb * bpb + 63) // 64
    block_size = 512 << bh

    linear = bytearray(wb * hb * bpb)
    row_bytes = wb * bpb

    for y in range(hb):
        gob_y = y >> 3
        y_in_block = (gob_y & ((1 << bh) - 1)) * 512
        block_row_off = (gob_y >> bh) * width_gobs * block_size
        yi = y & 7
        y_sector = ((yi >> 1) << 6) | ((yi & 1) << 4)

        for xb in range(0, row_bytes, 16):
            off = block_row_off + (xb >> 6) * block_size + y_in_block
            xi = xb & 63
            src_off = off + (((xi >> 5) << 8) | y_sector | (((xi >> 4) & 1) << 5) | (xi & 15))
            dst_off = y * row_bytes + xb
            linear[dst_off : dst_off + 16] = raw[src_off : src_off + 16]

    return linear


# per mode: subsets, partition bits, rotation bits, index-selection bits, colour bits, alpha bits,
# endpoint p-bits, shared p-bits, index bits, second index bits
MODES = [
    (3, 4, 0, 0, 4, 0, 1, 0, 3, 0),
    (2, 6, 0, 0, 6, 0, 0, 1, 3, 0),
    (3, 6, 0, 0, 5, 0, 0, 0, 2, 0),
    (2, 6, 0, 0, 7, 0, 1, 0, 2, 0),
    (1, 0, 2, 1, 5, 6, 0, 0, 2, 3),
    (1, 0, 2, 0, 7, 8, 0, 0, 2, 2),
    (1, 0, 0, 0, 7, 7, 1, 0, 4, 0),
    (2, 6, 0, 0, 5, 5, 1, 0, 2, 0),
]
SUBSETS2 = [
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec,
    0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce,
    0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0,
    0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a,
    0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4,
    0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718,
    0xccf0, 0x0fcc, 0x7744, 0xee22,
]
SUBSETS3 = [
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0,
    0x5a5a5050, 0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4,
    0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454,
    0x6a6a4040, 0xa4a45000, 0x1a1a0500, 0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400,
    0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050,
    0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444,
    0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600, 0xaa444444,
    0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44,
    0x2a4a5254,
]
ANCHOR2 = [15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8,
           2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8,
           15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15]
ANCHOR3A = [3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8,
            8, 6, 8, 5, 15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5,
            5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3]
ANCHOR3B = [15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15,
            6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15,
            15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8]
WEIGHTS = {2: [0, 21, 43, 64], 3: [0, 9, 18, 27, 37, 46, 55, 64],
           4: [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]}


def decode_block(block: bytes) -> list[tuple[int, int, int, int]]:
    """One 16-byte BC7 block: its 16 pixels as (r, g, b, a), row by row."""
    bits = int.from_bytes(block, "little")
    if not bits & 0xff:
        return [(0, 0, 0, 255)] * 16  # no mode bit (reserved): as Pillow, opaque black
    mode = (bits & -bits).bit_length() - 1
    pos = mode + 1

    def take(n: int) -> int:
        nonlocal pos
        v = (bits >> pos) & ((1 << n) - 1)
        pos += n
        return v

    ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2 = MODES[mode]
    partition, rotation, index_sel = take(pb), take(rb), take(isb)
    n = ns * 2
    ep = [[0, 0, 0, 255] for _ in range(n)]
    for ch in range(3):
        for e in ep:
            e[ch] = take(cb)
    if ab:
        for e in ep:
            e[3] = take(ab)
    chans = 4 if ab else 3
    if epb:
        cb += 1
        ab += 1 if ab else 0
        for e in ep:
            p = take(1)
            for ch in range(chans):
                e[ch] = e[ch] << 1 | p
    if spb:
        cb += 1
        ab += 1 if ab else 0
        for i in range(0, n, 2):
            p = take(1)
            for e in ep[i:i + 2]:
                for ch in range(chans):
                    e[ch] = e[ch] << 1 | p
    for e in ep:
        for ch in range(3):
            v = e[ch] << (8 - cb) & 0xff
            e[ch] = v | v >> cb
        if ab:
            v = e[3] << (8 - ab) & 0xff
            e[3] = v | v >> ab

    cw, aw = WEIGHTS[ib], WEIGHTS[ib2 if ab and ib2 else ib]
    cpos = pos
    apos = cpos + 16 * ib - ns
    out = []
    for i in range(16):
        s = 0
        if ns == 2:
            s = SUBSETS2[partition] >> i & 1
        elif ns == 3:
            s = SUBSETS3[partition] >> (2 * i) & 3
        anchor = i == 0 or (ns == 2 and i == ANCHOR2[partition]) or \
            (ns == 3 and i in (ANCHOR3A[partition], ANCHOR3B[partition]))
        bits_i = ib - 1 if anchor else ib
        i0 = (bits >> cpos) & ((1 << bits_i) - 1)
        cpos += bits_i
        e0, e1 = ep[2 * s], ep[2 * s + 1]
        if ab and ib2:
            bits_a = ib2 - 1 if i == 0 else ib2
            i1 = (bits >> apos) & ((1 << bits_a) - 1)
            apos += bits_a
            wc, wa = (aw[i1], cw[i0]) if index_sel else (cw[i0], aw[i1])
        else:
            wc = wa = cw[i0]
        px = [(e0[ch] * (64 - wc) + e1[ch] * wc + 32) >> 6 for ch in range(3)]
        px.append((e0[3] * (64 - wa) + e1[3] * wa + 32) >> 6)
        if rotation:
            px[rotation - 1], px[3] = px[3], px[rotation - 1]
        out.append(tuple(px))
    return out


def decode_rect(linear: bytes, tex_w: int, x: int, y: int, w: int, h: int) -> bytes:
    """The (x, y, w, h) part of a linear BC7 texture tex_w pixels wide, as straight RGBA rows."""
    wb = (tex_w + 3) // 4
    cache: dict[tuple[int, int], list] = {}
    out = bytearray(w * h * 4)
    for j in range(h):
        for i in range(w):
            px, py = x + i, y + j
            key = (px >> 2, py >> 2)
            if key not in cache:
                off = (key[1] * wb + key[0]) * 16
                cache[key] = decode_block(linear[off:off + 16])
            r, g, b, a = cache[key][(py & 3) * 4 + (px & 3)]
            o = (j * w + i) * 4
            out[o:o + 4] = bytes((r, g, b, a))
    return bytes(out)
