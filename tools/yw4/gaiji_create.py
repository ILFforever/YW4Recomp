"""Draw the PlayStation and Xbox sheets' middle-button icons and write them as BC7 blocks for the runtime.

Xbox: the game's Xbox sheet (gaiji_xinput.g4tx) has View and Menu, right for Xbox One and Series pads; an Xbox
360 pad's buttons there are Back and Start, marked with a left and a right triangle. For those the runtime
rewrites the middle 6 x 4 blocks of the two circle icons (x 12-35, y 16-31) with the triangles drawn here; the
circle's edge stays the game's.

PlayStation:

The game ships only a PS4 icon sheet (gaiji_ps4.g4tx). Its icon for the button the Switch calls Minus
(gaiji_bt09) is the PS4 touchpad, which is the wrong button on every PlayStation pad: it is Create on a
DualSense, Share on a DualShock 4 and Select on a PS3 pad; and the Plus icon (gaiji_bt10) says OPTIONS, which a
PS3 pad calls Start. The runtime (bindings.c, bindings_patch_sheet) rewrites those cells for the pad in use, in
the style of the sheet's Options icon: a word above a small dark pill. Two halves per cell:

  rows 24-47: the pill's body, copied at run time from the Options icon in the same texture (game art stays
              in the game; nothing of it is in this repo);
  rows 0-23:  ours, drawn here: the word (Arial Narrow Bold, cream with a thin gold edge, the owner's pick
              2026-11-13) and the pill's rounded top, which the Options icon hides under its word.

Output: runtime/src/gaiji_create.h, each word's top half as 12 x 6 BC7 blocks (mode 6, linear rows), and the
fingerprint of the touchpad icon's blocks, by which the runtime knows the sheet (a hash, no game data).

    python -m tools.yw4.gaiji_create [--preview out.png]

The fingerprint is read from the owner's own font.cpk (config.local.toml).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

from tools.yw4 import config, cpk
from tools.yw4.gaiji import deswizzle_bc7, decode_bc7_to_image

FONT = "C:/Windows/Fonts/ARIALNB.TTF"
CELL = 48
TOP = 24                      # rows of the cell we draw; the rest is the Options icon's pill
TOUCHPAD_AT = (104, 104)      # gaiji_bt09 in the 256 x 256 sheet
OPTIONS_AT = (156, 0)         # gaiji_bt10
GLOW = (150, 126, 58)         # the gold edge round the letters
FILL_TOP, FILL_BOTTOM = (255, 254, 240), (232, 222, 182)
PILL, PILL_EDGE = (31, 30, 30), (29, 27, 27)   # the pill body's colours at its top row (24)


WORDS = ["CREATE", "SHARE", "SELECT", "START"]  # the order of gaiji_word_top[] in the header


def draw_top(text: str) -> Image.Image:
    """Rows 0..TOP-1 of the icon with this word, RGBA."""
    im = Image.new("RGBA", (CELL, TOP), GLOW + (0,))
    # the pill's rounded top (rows 19-23; columns 20-27, its faint white side shadow at 17-19 and 28-30)
    p = im.load()
    cap = {19: {22: 177, 23: 255, 24: 255, 25: 177},
           20: {20: 94, 27: 94, **{x: 255 for x in range(21, 27)}},
           21: {20: 177, 27: 177, **{x: 255 for x in range(21, 27)}}}
    for y in range(19, TOP):
        row = cap.get(y, {x: 255 for x in range(20, 28)})
        for x, a in row.items():
            p[x, y] = (PILL_EDGE if x in (20, 27) else PILL) + (a,)
        if y >= 22:
            for x, a in ((17, 4), (18, 12), (19, 24), (28, 24), (29, 12), (30, 4)):
                p[x, y] = (255, 255, 255, a)
    # the word: drawn 8x larger with a 1-pixel stroke (a little bolder than the font), then shrunk
    S, size, top, spacing = 8, 11, 4, 0.4  # top 4: the word and its edge end by row 15, so no BC7
    # block row holds both the word and the pill (one block = one colour line)
    f = ImageFont.truetype(FONT, size * S)
    big = Image.new("L", (CELL * S, TOP * S), 0)
    d = ImageDraw.Draw(big)
    widths = [d.textlength(ch, font=f) for ch in text]
    x = (CELL * S - (sum(widths) + spacing * S * (len(text) - 1))) / 2
    for ch, cw in zip(text, widths):
        d.text((x, top * S), ch, font=f, fill=255, stroke_width=1, stroke_fill=255)
        x += cw + spacing * S
    m = big.resize((CELL, TOP), Image.LANCZOS)
    halo = m.filter(ImageFilter.MaxFilter(3)).point(lambda v: int(v * 0.9))
    g = Image.new("RGBA", (CELL, TOP), GLOW + (255,))
    g.putalpha(halo)
    im.alpha_composite(g)
    fill = Image.new("RGBA", (CELL, TOP))
    fd = ImageDraw.Draw(fill)
    for y in range(TOP):
        t = min(max((y - top - 2) / 9, 0), 1)
        fd.line([(0, y), (CELL - 1, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(FILL_TOP, FILL_BOTTOM)) + (255,))
    fill.putalpha(m.point(lambda v: min(255, int(v * 1.15))))
    im.alpha_composite(fill)
    return im


# ---- BC7 mode 6 (one subset, RGBA endpoints of 7 bits + a p-bit each, 4-bit indices)

WEIGHTS = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]


def _interp(e0, e1, w):
    return [((64 - w) * a + w * b + 32) >> 6 for a, b in zip(e0, e1)]


def _quant(e, p):
    """A float RGBA endpoint to 7-bit values with p-bit p; returns (7-bit values, the 8-bit colour)."""
    q = [min(127, max(0, int(round((v - p) / 2)))) for v in e]
    return q, [(v << 1) | p for v in q]


def _wt(c):
    """how much a pixel's colour counts: by its alpha (a clear pixel's colour is never seen), alpha itself 4x"""
    a = max(c[3] / 255, 0.02)
    return (a, a, a, 4.0)


def _fit(px, e0, e1):
    """Best index per pixel for these endpoints, and the total weighted squared error."""
    pal = [_interp(e0, e1, w) for w in WEIGHTS]
    idx, err = [], 0.0
    for c in px:
        wt = _wt(c)
        errs = [sum(k * (a - b) ** 2 for k, a, b in zip(wt, c, pal[i])) for i in range(16)]
        best = min(range(16), key=errs.__getitem__)
        idx.append(best)
        err += errs[best]
    return idx, err


def _ls(px, idx):
    """Weighted least-squares endpoints for fixed indices, channel by channel."""
    e0, e1 = [0.0] * 4, [0.0] * 4
    for k in range(4):
        a2 = b2 = ab = ax = bx = 0.0
        for c, i in zip(px, idx):
            q = _wt(c)[k]
            b = WEIGHTS[i] / 64
            a = 1 - b
            a2 += q * a * a; b2 += q * b * b; ab += q * a * b
            ax += q * a * c[k]; bx += q * b * c[k]
        det = a2 * b2 - ab * ab
        if abs(det) < 1e-9:
            return None
        e0[k] = min(255, max(0, (ax * b2 - bx * ab) / det))
        e1[k] = min(255, max(0, (bx * a2 - ax * ab) / det))
    return e0, e1


def encode_block(px: list[tuple[int, int, int, int]]) -> bytes:
    """16 RGBA pixels (rows) to one BC7 mode 6 block."""
    # start: the two pixels farthest apart, then refine by least squares
    far = max(((i, j) for i in range(16) for j in range(i + 1, 16)),
              key=lambda ij: sum((a - b) ** 2 for a, b in zip(px[ij[0]], px[ij[1]])))
    e0, e1 = [float(v) for v in px[far[0]]], [float(v) for v in px[far[1]]]
    best = None
    for _ in range(4):
        for p0 in (0, 1):
            for p1 in (0, 1):
                q0, c0 = _quant(e0, p0)
                q1, c1 = _quant(e1, p1)
                idx, err = _fit(px, c0, c1)
                if best is None or err < best[0]:
                    best = (err, q0, q1, p0, p1, idx)
        r = _ls(px, best[5])
        if not r:
            break
        e0, e1 = r
    _, q0, q1, p0, p1, idx = best
    if idx[0] >= 8:  # the first index's top bit is implied 0: swap the endpoints
        q0, q1, p0, p1 = q1, q0, p1, p0
        idx = [15 - i for i in idx]
    bits, n = 1 << 6, 7  # mode 6
    for k in range(4):
        for q in (q0[k], q1[k]):
            bits |= q << n; n += 7
    bits |= p0 << n; n += 1
    bits |= p1 << n; n += 1
    for i, v in enumerate(idx):
        bits |= v << n; n += 3 if i == 0 else 4
    assert n == 128
    return bits.to_bytes(16, "little")


def encode(im: Image.Image) -> bytes:
    """An RGBA image (sides a multiple of 4) to linear rows of BC7 blocks."""
    px = im.load()
    out = bytearray()
    for by in range(0, im.height, 4):
        for bx in range(0, im.width, 4):
            out += encode_block([px[bx + x, by + y] for y in range(4) for x in range(4)])
    return bytes(out)


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def cell_blocks(linear: bytes, sheet_w: int, at: tuple[int, int], rows: int = CELL) -> bytes:
    """A cell's BC7 blocks, as linear rows (what tegra_read_region gives the runtime)."""
    pitch = sheet_w // 4 * 16
    out = bytearray()
    for by in range(at[1] // 4, (at[1] + rows) // 4):
        s = by * pitch + at[0] // 4 * 16
        out += linear[s:s + CELL // 4 * 16]
    return bytes(out)


XBOX_VIEW_AT = (104, 104)     # gaiji_bt09 in the Xbox sheet (View), same cells as the PlayStation sheet's
XBOX_MENU_AT = (156, 0)       # gaiji_bt10 (Menu)
XBOX_PATCH = (12, 16, 24, 16)  # x, y, w, h in the cell: the middle 6 x 4 blocks, round the symbol
# the circle's inside there (near black, darker at the top), by row; 8 at the sides from row 28; and its four
# corner pixels on the circle's soft edge (grey, alpha). Measured from the game's Menu icon (owner OK'd the look).
XBOX_ROW = {16: 0, 17: 0, 18: 0, 19: 0, 20: 2, 21: 2, 22: 3, 23: 4, 24: 4, 25: 5, 26: 6, 27: 7, 28: 8, 29: 8,
            30: 10, 31: 11}
XBOX_CORNER = {(12, 16): (36, 211), (35, 16): (36, 211), (12, 31): (8, 211), (35, 31): (8, 215)}


def draw_xbox360(direction: str) -> Image.Image:
    """The middle of an Xbox circle icon (XBOX_PATCH) with a white triangle: Start points right, Back left (an
    Xbox 360 pad's symbols). Rows 18-29 like the Menu icon's lines; Start at x 20-29, Back its mirror round the
    circle's centre (x = 24), each 0.4 px from the exact middle (whole pixels)."""
    x0, y0, w, h = XBOX_PATCH
    im = Image.new("RGBA", (w, h))
    p = im.load()
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            v = 8 if (x < 16 or x > 31) and y >= 28 else XBOX_ROW[y]
            g, a = XBOX_CORNER.get((x, y), (v, 255))
            p[x - x0, y - y0] = (g, g, g, a)
    for y in range(18, 30):
        n = max(1, int(round(11 * (5.5 - abs(y - 23.5)) / 5.5)))
        for i in range(n):
            x = 20 + i if direction == "right" else 27 - i
            p[x - x0, y - y0] = (255, 255, 255, 255)
    return im


def region_blocks(linear: bytes, sheet_w: int, x: int, y: int, w: int, h: int) -> bytes:
    pitch = sheet_w // 4 * 16
    return b"".join(linear[by * pitch + x // 4 * 16: by * pitch + (x + w) // 4 * 16] for by in range(y // 4, (y + h) // 4))


def load_sheet(name: str) -> bytes:
    """A sheet from the owner's font.cpk, as linear rows of BC7 blocks (256 x 256)."""
    font_cpk = config.load().romfs_dir / "data" / "nx" / "font" / "font.cpk"
    entry = next(f for f in cpk.list_files(font_cpk).files if f.path.endswith("/" + name))
    data = cpk.read(font_cpk, entry)
    u16 = lambda off: int.from_bytes(data[off:off + 2], "little")
    nxtch = 4 * (u16(0x0A) + u16(0x3E))
    w = int.from_bytes(data[nxtch + 0x14:nxtch + 0x18], "little")
    h = int.from_bytes(data[nxtch + 0x18:nxtch + 0x1C], "little")
    if (w, h) != (256, 256):
        raise SystemExit(f"error: {name} is {w}x{h}, expected 256x256")
    return bytes(deswizzle_bc7(data[nxtch + 0x100:], w, h))


def c_array(fp, name: str, data: bytes, indent: str = "") -> None:
    fp.write(f"{indent}static const unsigned char {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        fp.write(indent + "    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
    fp.write(indent + "};\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="runtime/src/gaiji_create.h")
    ap.add_argument("--preview", help="also save the new icons (with the sheets' own parts) as a PNG")
    args = ap.parse_args(argv)

    ps = load_sheet("gaiji_ps4.g4tx")
    xb = load_sheet("gaiji_xinput.g4tx")
    print_ = fnv1a64(cell_blocks(ps, 256, TOUCHPAD_AT))
    view_print = fnv1a64(cell_blocks(xb, 256, XBOX_VIEW_AT))

    tops = [encode(draw_top(word)) for word in WORDS]
    back, start = encode(draw_xbox360("left")), encode(draw_xbox360("right"))
    n = len(tops[0])
    with open(args.out, "w", newline="\n") as fp:
        fp.write("/* Generated by python -m tools.yw4.gaiji_create (our drawings; no game pixels).\n"
                 " * PlayStation sheet: the middle-button words, each as rows 0-23 of an icon (the word and the pill's\n"
                 " * top) in 12 x 6 BC7 blocks, linear rows; the fingerprint (FNV-1a 64) of the touchpad icon's 12 x 12\n"
                 " * blocks. Xbox sheet: an Xbox 360 pad's Back and Start, the middle 6 x 4 blocks of the circle icon\n"
                 " * (x 12-35, y 16-31 of the cell); the fingerprint of the View icon's 12 x 12 blocks. */\n")
        fp.write(f"#define GAIJI_TOUCHPAD_PRINT 0x{print_:016x}ull\n")
        fp.write(f"#define GAIJI_XBOX_VIEW_PRINT 0x{view_print:016x}ull\n")
        fp.write("enum { " + ", ".join(f"GAIJI_WORD_{w}" for w in WORDS) + ", GAIJI_WORDS };\n")
        fp.write(f"static const unsigned char gaiji_word_top[GAIJI_WORDS][{n}] = {{\n")
        for word, blocks in zip(WORDS, tops):
            fp.write(f"    /* {word} */ {{\n")
            for i in range(0, n, 16):
                fp.write("        " + ", ".join(f"0x{b:02x}" for b in blocks[i:i + 16]) + ",\n")
            fp.write("    },\n")
        fp.write("};\n")
        c_array(fp, "gaiji_xbox_back", back)
        c_array(fp, "gaiji_xbox_start", start)
    print(f"wrote {args.out}: {len(WORDS)} words of {n // 16} blocks, Back and Start of {len(back) // 16}; "
          f"prints {print_:016x} {view_print:016x}")

    if args.preview:
        # each icon as the runtime builds it, decoded: the PlayStation words over the Options icon's pill; the
        # Xbox middles in the Menu icon's circle
        body = cell_blocks(ps, 256, (OPTIONS_AT[0], OPTIONS_AT[1] + TOP), CELL - TOP)
        icons = [decode_bc7_to_image(t + body, CELL, CELL).convert("RGBA") for t in tops]
        x0, y0, pw, ph = XBOX_PATCH
        for mid in (back, start):
            cell = bytearray(cell_blocks(xb, 256, XBOX_MENU_AT))
            for by in range(ph // 4):
                row = (y0 // 4 + by) * (CELL // 4 * 16) + x0 // 4 * 16
                cell[row:row + pw // 4 * 16] = mid[by * pw // 4 * 16:(by + 1) * pw // 4 * 16]
            icons.append(decode_bc7_to_image(bytes(cell), CELL, CELL).convert("RGBA"))
        out = Image.new("RGBA", (len(icons) * CELL * 5, CELL * 5), (55, 55, 66, 255))
        for i, icon in enumerate(icons):
            out.alpha_composite(icon.resize((CELL * 5, CELL * 5), Image.NEAREST), (i * CELL * 5, 0))
        out.save(args.preview)
        print(f"preview {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
