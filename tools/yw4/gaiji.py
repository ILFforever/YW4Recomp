"""Decode Level-5 G4TX icon sheets from font.cpk into PNGs.

Reads a .g4tx sheet (default gaiji_key.g4tx), de-swizzles its Switch block-linear
BC7 texture, decodes it with Pillow, prints every icon's (x, y, w, h), and saves
the full sheet plus each icon as PNG in work/gaiji/.
"""

from __future__ import annotations

import argparse
import io
import struct
import sys
from pathlib import Path
from PIL import Image

from tools.yw4 import config, cpk


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
    block_rows = (hb + (8 << bh) - 1) // (8 << bh)
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


def decode_bc7_to_image(linear_bc7: bytes, w: int, h: int) -> Image.Image:
    """Wrap linear BC7 blocks into a DDS stream and decode using Pillow."""
    bw, bh_texel = 4, 4
    wb = (w + bw - 1) // bw
    hb = (h + bh_texel - 1) // bh_texel
    padded_w = wb * bw
    padded_h = hb * bh_texel

    header = bytearray(128)
    header[0:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)  # header_size
    struct.pack_into("<3I", header, 8, 0x1007, padded_h, padded_w)  # flags, height, width
    struct.pack_into("<3I", header, 20, 0, 0, 1)  # pitch, depth, mipmaps
    struct.pack_into("<4I", header, 76, 32, 4, 0x30315844, 0)  # ddspf: DDPF_FOURCC, 'DX10'
    struct.pack_into("<I", header, 108, 0x1000)  # caps: DDSCAPS_TEXTURE

    # DXGI_FORMAT_BC7_UNORM = 98, D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3
    dx10 = struct.pack("<5I", 98, 3, 0, 1, 0)

    stream = io.BytesIO(bytes(header) + dx10 + linear_bc7)
    im = Image.open(stream)
    im.load()
    if padded_w != w or padded_h != h:
        im = im.crop((0, 0, w, h))
    return im


def extract_g4tx(data: bytes, out_dir: Path | None = None) -> tuple[Image.Image, list[dict]]:
    """Parse G4TX container, decode the sheet image, and extract icons."""
    if data[:4] != b"G4TX":
        raise ValueError("Not a G4TX container")

    u16 = lambda off: struct.unpack_from("<H", data, off)[0]
    base = u16(0x0A)
    total_names = u16(0x22)

    # String table
    str_tab = 4 * (base + u16(0x46))
    names = []
    for i in range(total_names):
        off = str_tab + u16(str_tab + 2 * i)
        end = data.index(b"\0", off)
        names.append(data[off:end].decode("ascii"))

    sheet_name = names[0] if names else "sheet"

    # Texture info from NXTCH
    nxtch_off = 4 * (base + u16(0x3E))
    if data[nxtch_off : nxtch_off + 8] != b"NXTCH000":
        raise ValueError(f"Expected NXTCH000 at offset {nxtch_off:#x}")

    w = struct.unpack_from("<I", data, nxtch_off + 0x14)[0]
    h = struct.unpack_from("<I", data, nxtch_off + 0x18)[0]
    raw_tex = data[nxtch_off + 0x100 :]

    # De-swizzle and decode
    linear_bc7 = deswizzle_bc7(raw_tex, w, h)
    sheet_img = decode_bc7_to_image(linear_bc7, w, h)

    # Sub-image atlas records (table 0x42)
    rec_tab = 4 * (base + u16(0x42))
    icons = []
    for i in range(1, total_names):
        off = rec_tab + (i - 1) * 24
        x = u16(off + 4)
        y = u16(off + 6)
        icon_w = u16(off + 8)
        icon_h = u16(off + 10)
        name = names[i]
        icons.append({"name": name, "x": x, "y": y, "w": icon_w, "h": icon_h})

    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)
        sheet_path = out_dir / f"{sheet_name}.png"
        sheet_img.save(sheet_path)

        for icon in icons:
            x, y, iw, ih = icon["x"], icon["y"], icon["w"], icon["h"]
            icon_img = sheet_img.crop((x, y, x + iw, y + ih))
            icon_img.save(out_dir / f"{icon['name']}.png")

    return sheet_img, icons


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Extract and decode gaiji icon sheets from font.cpk")
    parser.add_argument("sheet", nargs="?", default="gaiji_key.g4tx", help="G4TX file name (default: gaiji_key.g4tx)")
    parser.add_argument("--out-dir", default="work/gaiji", help="Output directory (default: work/gaiji)")
    args = parser.parse_args(argv)

    cfg = config.load()
    font_cpk = cfg.romfs_dir / "data" / "nx" / "font" / "font.cpk"
    if not font_cpk.is_file():
        print(f"Error: {font_cpk} not found", file=sys.stderr)
        return 1

    cpk_files = {f.path: f for f in cpk.list_files(font_cpk).files}
    target_entry = None
    target_name = args.sheet if args.sheet.endswith(".g4tx") else f"{args.sheet}.g4tx"

    for path, entry in cpk_files.items():
        if path.endswith(f"/{target_name}") or path == target_name:
            target_entry = entry
            break

    if not target_entry:
        print(f"Error: {target_name} not found in {font_cpk}", file=sys.stderr)
        return 1

    data = cpk.read(font_cpk, target_entry)
    out_dir = Path(args.out_dir)
    sheet_img, icons = extract_g4tx(data, out_dir=out_dir)

    print(f"Sheet: {target_name} ({sheet_img.width}x{sheet_img.height}), {len(icons)} icons")
    for icon in icons:
        print(f"  {icon['name']:20s} x={icon['x']:3d} y={icon['y']:3d} w={icon['w']:2d} h={icon['h']:2d}")

    print(f"Saved sheet and icons to {out_dir}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
