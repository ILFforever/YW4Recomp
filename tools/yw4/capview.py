"""Contact sheet of the render-target captures from YW4_CAPTURE_FS (work/dump/cap_*.raw).

    python -m tools.yw4.capview [dump folder] [out.png] [--alpha]

Each capture is u32 width, height, VkFormat, then the texels. RGB is scaled to its 99.5th
percentile (printed in the tile title), so dark HDR buffers stay readable; --alpha adds a tile for
alpha where it varies. Prints the per-channel mean and maximum of every capture.
"""

from __future__ import annotations

import glob
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

VK_R8G8B8A8_UNORM, VK_R16G16B16A16_SFLOAT, VK_B10G11R11_UFLOAT = 37, 97, 122


def small_float(x: np.ndarray, mbits: int) -> np.ndarray:
    """Unsigned float with a 5-bit exponent and mbits of mantissa (R11G11B10)."""
    e = (x >> mbits).astype(np.int32)
    m = (x & ((1 << mbits) - 1)).astype(np.float32)
    return np.where(e == 0, np.ldexp(m, -14 - mbits), np.ldexp(m + (1 << mbits), e - 15 - mbits)).astype(np.float32)


def decode(path: str) -> np.ndarray:
    """The capture as height x width x RGBA float32."""
    raw = open(path, "rb").read()
    w, h, fmt = struct.unpack_from("<3I", raw)
    data = raw[12:]
    if fmt == VK_R8G8B8A8_UNORM:
        return np.frombuffer(data, np.uint8).reshape(h, w, 4).astype(np.float32) / 255
    if fmt == VK_R16G16B16A16_SFLOAT:
        return np.frombuffer(data, "<f2").reshape(h, w, 4).astype(np.float32)
    if fmt == VK_B10G11R11_UFLOAT:
        v = np.frombuffer(data, "<u4").reshape(h, w)
        return np.stack([small_float(v & 0x7FF, 6), small_float((v >> 11) & 0x7FF, 6), small_float(v >> 22, 5),
                         np.ones((h, w), np.float32)], -1)
    raise SystemExit(f"{path}: VkFormat {fmt} not handled")


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    folder = args[0] if args else "work/dump"
    out = args[1] if len(args) > 1 else os.path.join(folder, "captures.png")
    tiles = []
    for p in sorted(glob.glob(os.path.join(folder, "cap_*.raw"))):
        a = decode(p)
        name = os.path.basename(p)[4:-4]
        rgb = a[..., :3]
        top = float(np.percentile(rgb, 99.5)) or 1.0
        print(f"{name}: rgb mean {rgb.reshape(-1, 3).mean(0).round(4)} max {rgb.reshape(-1, 3).max(0).round(3)}"
              f"  alpha mean {a[..., 3].mean():.3f}")
        tiles.append((f"{name} rgb/{top:.2f}", Image.fromarray((np.clip(rgb / top, 0, 1) * 255).astype(np.uint8))))
        if "--alpha" in sys.argv and a[..., 3].std() > 0:
            alpha = (np.clip(a[..., 3], 0, 1) * 255).astype(np.uint8)
            tiles.append((f"{name} alpha", Image.fromarray(alpha).convert("RGB")))
    if not tiles:
        raise SystemExit(f"no cap_*.raw in {folder}")
    w, h, cols = 400, 225, 4
    sheet = Image.new("RGB", (w * cols, (h + 16) * ((len(tiles) + cols - 1) // cols)), "white")
    for i, (title, im) in enumerate(tiles):
        x, y = (i % cols) * w, (i // cols) * (h + 16)
        sheet.paste(im.resize((w, h)), (x, y + 16))
        ImageDraw.Draw(sheet).text((x + 2, y + 2), title, fill="black")
    sheet.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
