"""Extract the game's shaders: parse Level-5's HBH1 container, keep the assembly.

    python -m tools.yw4.shaders

Every shader file (.vsb/.fsb/.gsb/.esb in data/nx/shader/shader.cpk, loose .csb
compute shaders next to it) is an HBH1 container, little-endian:

    0x00 "HBH1"
    0x04 six (u32 offset, u32 size) pairs, sections in this order:
         inputs     vertex inputs, 0x10 each: u32 name, u32 hash, u32 location, u32 0
         resources  0x20 each: u32 name, u32 hash, u16 kind, u16 slot, ...
                    kind 0 constant buffer, 3 texture, 1/2 compute image/buffer,
                    4/6 storage buffer (output/input)
         members    0x10 each: u32 name, u32 hash, u16 type, u16 count, u16 size, u16 offset
                    (constant-buffer members, e.g. u_rate at offset 0)
         (unknown)  0x10 each, not needed so far
         NXGL       u32 "NXGL", u32 1, then (u32 offset, u32 size) of the GPU code and of
                    the control section, relative to NXGL. The control section is what the
                    game passes to nvnProgramSetShaders as `control`; it contains the
                    NV_gpu_program5 assembly ("!!NVvp5.0" ... "END") the code was built from.
         strings    NUL-terminated names; the u32 name fields above are offsets into it

Shaders are identified by the FNV-1a 64-bit hash of the assembly text, from "!!NV"
through "END" inclusive. The runtime computes the same hash from the control
pointer, so the translated shader is found without knowing which file it came from.
1,891 programs have 1,349 distinct texts.

Writes <work_dir>/shaders/asm/<key>.<stage>.asm and <work_dir>/shaders/index.json.
"""

from __future__ import annotations

import collections
import json
import re
import struct
import sys
from pathlib import Path

from . import config, cpk

STAGES = {b"!!NVvp5.0": "vert", b"!!NVfp5.0": "frag", b"!!NVgp5.0": "geom", b"!!NVcp5.0": "comp"}
KINDS = {0: "cbuffer", 1: "image", 2: "buffer", 3: "texture", 4: "storage_out", 6: "storage_in"}


class ShaderError(Exception):
    pass


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def assembly(control: bytes) -> bytes:
    """The assembly text inside a control section (or a whole file)."""
    a = control.find(b"!!NV")
    if a < 0:
        raise ShaderError("no NV_gpu_program5 assembly")
    m = re.compile(rb"\nEND(?![A-Z])").search(control, a)   # not ENDIF / ENDREP
    if not m:
        raise ShaderError("assembly has no END")
    return control[a:m.end()]


def parse(data: bytes) -> dict:
    if data[:4] != b"HBH1":
        raise ShaderError("not an HBH1 container")
    secs = [struct.unpack_from("<II", data, 4 + 8 * i) for i in range(6)]
    so, sn = secs[5]
    strings = data[so:so + sn]

    def name(off: int) -> str:
        return strings[off:strings.index(b"\0", off)].decode("ascii")

    def entries(i: int, size: int):
        o, n = secs[i]
        return [data[e:e + size] for e in range(o, o + n, size)]

    inputs = [{"name": name(e[0]), "location": e[2]}
              for e in (struct.unpack_from("<III", x) for x in entries(0, 0x10))]
    resources = [{"name": name(e[0]), "kind": KINDS.get(e[2], e[2]), "slot": e[3]}
                 for e in (struct.unpack_from("<IIHH", x) for x in entries(1, 0x20))]
    members = [{"name": name(e[0]), "type": e[2], "count": e[3], "size": e[4], "offset": e[5]}
               for e in (struct.unpack_from("<IIHHHH", x) for x in entries(2, 0x10))]
    no, nn = secs[4]
    if data[no:no + 4] != b"NXGL":
        raise ShaderError("no NXGL section")
    code_off, code_size, ctrl_off, ctrl_size = struct.unpack_from("<4I", data, no + 8)
    control = data[no + ctrl_off:no + ctrl_off + ctrl_size]
    text = assembly(control)
    stage = STAGES.get(text.split(b"\n", 1)[0].strip())
    if stage is None:
        raise ShaderError(f"unknown program header {text[:9]!r}")
    return {"stage": stage, "text": text, "code_size": code_size,
            "inputs": inputs, "resources": resources, "members": members}


def shader_files(romfs: Path):
    """(name, bytes) for every shader program the game ships."""
    sdir = romfs / "data/nx/shader"
    c = sdir / "shader.cpk"
    for e in cpk.list_files(c).files:
        if e.path.endswith((".vsb", ".fsb", ".gsb", ".esb")):
            yield e.path.lstrip("/"), cpk.read(c, e)
    for p in sorted(sdir.glob("*.csb")):
        yield p.name, p.read_bytes()


def main() -> int:
    try:
        cfg = config.load(require_exefs=False)
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1
    out = cfg.work_dir / "shaders"
    (out / "asm").mkdir(parents=True, exist_ok=True)
    index: dict[str, dict] = {}
    stages = collections.Counter()
    for fname, data in shader_files(cfg.romfs_dir):
        try:
            s = parse(data)
        except ShaderError as e:
            print(f"{fname}: {e}", file=sys.stderr)
            return 1
        key = f"{fnv1a64(s['text']):016x}"
        if key not in index:
            (out / "asm" / f"{key}.{s['stage']}.asm").write_bytes(s["text"] + b"\n")
            index[key] = {"stage": s["stage"], "files": [], "inputs": s["inputs"],
                          "resources": s["resources"], "members": s["members"]}
            stages[s["stage"]] += 1
        index[key]["files"].append(fname)
    (out / "index.json").write_text(json.dumps(index, indent=1))
    print(f"{sum(len(v['files']) for v in index.values())} programs, {len(index)} distinct: "
          + ", ".join(f"{k} {v}" for k, v in stages.most_common()))
    print(f"wrote {out / 'index.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
