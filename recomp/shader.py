"""Translate the game's NV_gpu_program5 assembly to GLSL for Vulkan (decision 0020).

    python -m recomp.shader [--check] [key ...]

Reads <work_dir>/shaders/asm/<key>.<stage>.asm (from `python -m tools.yw4.shaders`),
writes <work_dir>/shaders/glsl/<key>.<stage> and <key>.json (what the shader binds).
--check compiles every output with glslangValidator (Vulkan SDK) to SPIR-V.

The model follows the assembly closely, so a line of output can be traced to a line
of input (each instruction is emitted as a comment above its GLSL):

- Registers have no type in the assembly: an instruction's suffix (.F/.F32/.F16, .S,
  .U) says how it reads them. Every temporary (R, H, and the H/R condition temps) is a
  `uvec4` of raw bits; operands are reinterpreted per instruction (`uintBitsToFloat`).
  H registers are fp16 on the Switch; here they are fp32 (precision may differ).
- D registers (64-bit) only ever hold texture handles loaded with
  `LDC.U64 Dn.c, buf14[k]`. buf14 is where NVN puts bound textures' handles, so the
  handle is texture binding k/8; D registers never appear in the output.
- Constant buffers are `uvec4 d[4096]` uniform blocks addressed by byte offset (the
  assembly's `bufN[R0.x + 16]`): offsets are multiples of the load size (spec), so an
  X4 load is one array element.
- The condition code (written by `.CC` instructions) is only ever tested with NE, so
  it is kept as `bvec4 cc` = "written value != 0" for the components written.
- Control flow is structured (IF/ELSE/ENDIF, REP/ENDREP, BRK) and maps 1:1.

Vulkan interface (the runtime builds its descriptor layouts from <key>.json):
- descriptor set = stage (vertex 0, fragment 1, geometry 2; compute 0)
- constant buffer slot n -> binding n; texture slot k -> binding 16 + k;
  storage buffer n -> binding 64 + n (texture slots reach 32)
- vertex input / varying / colour output n -> location n. Varyings are vec4; integer
  data is carried as float bits (fragment inputs read as integers are `flat`).
- Push constant `yw4.pos_fix` (vertex and geometry): bit 0 (vertex) or bit 1 (geometry)
  maps the stage's clip-space z from OpenGL's [-w, w] to Vulkan's [0, w] (NVN's default
  depth mode). The runtime sets the bit of the last stage before rasterisation only. Y
  orientation is left to the viewport.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools.yw4 import config

SET = {"vert": 0, "frag": 1, "geom": 2, "comp": 0}
TEX_BINDING, STORAGE_BINDING = 16, 64
# Soft shadows (yw4_shadow below): the shadow map's raw depth at DEPTH_BINDING + slot, the
# runtime's settings at PCSS_BINDING.
DEPTH_BINDING, PCSS_BINDING = 96, 95
# The shaders that get yw4_shadow: only the game's screen-space shadow mask (a full-screen pass
# at half resolution that shadows everything in the G-buffer). The helper is copied into every
# lookup by the driver, and in all ~200 shadow shaders that made pipeline builds take up to 25 s
# each; the other shaders keep the game's own lookups. Keys are hashes of the game's shaders.
PCSS_SHADERS = {"06d03ad703b4bee5"}
SWZ = "xyzw"
GLSL_TYPE = {"f": "vec4", "u": "uvec4", "s": "ivec4"}


class ShaderError(Exception):
    pass


@dataclass
class Operand:
    text: str
    neg: bool = False
    abs: bool = False
    swizzle: str = "xyzw"   # always 4 components (NV rule: a short swizzle repeats its last)


@dataclass
class Program:
    key: str
    stage: str
    regs: list[str] = field(default_factory=list)
    arrays: dict[str, int] = field(default_factory=dict)              # lmemN -> element count
    images: dict[int, str] = field(default_factory=dict)              # slot -> dim
    written_storage: set[int] = field(default_factory=set)
    input_locs: set[int] = field(default_factory=set)                 # every input read
    alias: dict[str, tuple[str, int]] = field(default_factory=dict)   # name -> (base, first index)
    cbuffers: dict[str, int] = field(default_factory=dict)            # name -> slot
    storage: dict[str, int] = field(default_factory=dict)
    shared: str | None = None
    shared_size: int = 0
    group_size: list[int] = field(default_factory=lambda: [1, 1, 1])
    gs: dict[str, str | int] = field(default_factory=dict)
    handles: dict[str, int] = field(default_factory=dict)             # "D0.x" -> texture slot
    textures: dict[int, str] = field(default_factory=dict)            # slot -> dim
    depth_reads: set[int] = field(default_factory=set)                # shadow map slots read via yw4_shadow
    inputs: dict[int, str] = field(default_factory=dict)              # location -> "f" / "u"
    flat_inputs: set[int] = field(default_factory=set)
    outputs: set[int] = field(default_factory=set)
    colors: set[int] = field(default_factory=set)
    used_cbuffers: set[int] = field(default_factory=set)
    cbuffer_rows: dict[int, set[int]] = field(default_factory=dict)   # slot -> 16-byte rows read by a constant address
    cbuffer_computed: set[int] = field(default_factory=set)           # slots also read at a computed address
    uses: set[str] = field(default_factory=set)
    body: list[str] = field(default_factory=list)
    depth: int = 1


def split_args(s: str) -> list[str]:
    """Split on commas outside {}, [] and ()."""
    out, cur, nest = [], "", 0
    for ch in s:
        if ch in "{[(":
            nest += 1
        elif ch in "}])":
            nest -= 1
        if ch == "," and nest == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def balanced(s: str) -> bool:
    depth = 0
    for ch in s:
        depth += {"(": 1, ")": -1}.get(ch, 0)
        if depth < 0:
            return False
    return depth == 0


def paren(e: str) -> str:
    """`e` safe to follow with a swizzle: names and single calls as they are, else (e)."""
    if re.fullmatch(r"[\w.\[\]]+", e):
        return e
    m = re.fullmatch(r"\w+\((.*)\)(\.[xyzw]+)?", e)
    if m and balanced(m.group(1)):
        return e
    return f"({e})"


def parse_operand(text: str) -> Operand:
    op = Operand(text)
    t = text.strip()
    if t.startswith("-"):
        op.neg, t = True, t[1:].strip()
    if t.startswith("|"):
        end = t.rindex("|")
        op.abs = True
        t = t[1:end] + t[end + 1:]
    m = re.fullmatch(r"(.*?)\.([xyzw]{1,4})", t)
    if m and not m.group(1).endswith(("vertex", "fragment", "result")):
        t, s = m.group(1), m.group(2)
        op.swizzle = (s + s[-1] * 4)[:4]
    op.text = t
    return op


def literal(values: list[str], typ: str) -> str:
    vals = []
    for v in values:
        v = v.strip()
        if typ == "f":
            if v.lower().startswith(("0x", "-0x")):
                raise ShaderError(f"hex literal {v} in a float operation")
            x = float(v)
            vals.append(repr(x) if "e" not in repr(x) else f"{x:.9e}")
        else:
            x = int(v, 0) if not re.search(r"[.eE]", v) or v.lower().startswith("0x") else None
            if x is None:
                raise ShaderError(f"float literal {v} in an integer operation")
            if typ == "u":
                vals.append(f"{x & 0xFFFFFFFF}u")
            else:
                x = x & 0xFFFFFFFF
                vals.append(str(x - (1 << 32) if x >= 1 << 31 else x))
    while len(vals) < 4:
        vals.append(vals[-1])
    return f"{GLSL_TYPE[typ]}({', '.join(vals)})"


def convert(expr: str, frm: str, to: str) -> str:
    if frm == to:
        return expr
    # Undo a reinterpretation instead of stacking a second one.
    inverse = {("u", "f"): "floatBitsToUint(", ("f", "u"): "uintBitsToFloat("}.get((frm, to))
    if inverse and expr.startswith(inverse) and expr.endswith(")") and balanced(expr[len(inverse):-1]):
        return expr[len(inverse):-1]
    return {("u", "f"): "uintBitsToFloat({})", ("s", "f"): "intBitsToFloat({})",
            ("f", "u"): "floatBitsToUint({})", ("f", "s"): "floatBitsToInt({})",
            ("u", "s"): "ivec4({})", ("s", "u"): "uvec4({})"}[(frm, to)].format(expr)


class Translator:
    def __init__(self, key: str, stage: str, text: str):
        self.p = Program(key, stage)
        self.lines = [ln.strip() for ln in text.split(";")]
        self.text = text

    # ----------------------------------------------------------------- declarations
    def declare(self, ln: str) -> bool:
        p = self.p
        if ln.startswith(("!!NV", "OPTION ")) or ln == "":
            return True
        m = re.fullmatch(r"(?:SHORT |LONG )?TEMP (.+)", ln)
        if m:
            for r in (x.strip() for x in m.group(1).split(",")):
                a = re.fullmatch(r"(\w+)\[(\d+)\]", r)
                if a:
                    p.arrays[a.group(1)] = int(a.group(2))
                elif ln.startswith("LONG"):
                    if not r.startswith("D"):
                        raise ShaderError(f"LONG temp {r}")
                elif r != "T":
                    p.regs.append(r)
            return True
        m = re.fullmatch(r"CBUFFER (\w+)\[\] = \{ program\.buffer\[(\d+)\] \}", ln)
        if m:
            p.cbuffers[m.group(1)] = int(m.group(2))
            return True
        m = re.fullmatch(r"STORAGE (\w+)\[\] = \{ program\.storage\[(\d+)\] \}", ln)
        if m:
            p.storage[m.group(1)] = int(m.group(2))
            return True
        m = re.fullmatch(r"SHARED (\w+)\[\] = \{ program\.sharedmem \}", ln)
        if m:
            p.shared = m.group(1)
            return True
        m = re.fullmatch(r"(FLAT )?(ATTRIB|OUTPUT) (\w+)(\[\])? = \{? ?([\w.]+)(?:\[(\d+)(?:\.\.(\d+))?\])? ?\}?", ln)
        if m:
            first = int(m.group(6) or 0)
            p.alias[m.group(3)] = (m.group(5), first)
            if m.group(1):
                p.flat_inputs.update(range(first, int(m.group(7) or first) + 1))
            return True
        m = re.fullmatch(r"(FLAT )?ATTRIB (\w+) = (\w+\.attrib)\[(\d+)\]", ln)
        if m:
            p.alias[m.group(2)] = (m.group(3), int(m.group(4)))
            if m.group(1):
                p.flat_inputs.add(int(m.group(4)))
            return True
        if ln.startswith(("GROUP_SIZE ", "SHARED_MEMORY ", "PRIMITIVE_IN ", "PRIMITIVE_OUT ", "VERTICES_OUT ")):
            w = ln.split()
            if w[0] == "GROUP_SIZE":
                p.group_size[:len(w) - 1] = [int(x) for x in w[1:]]
            elif w[0] == "SHARED_MEMORY":
                p.shared_size = int(w[1])
            else:
                p.gs[w[0]] = w[1] if w[0] != "VERTICES_OUT" else int(w[1])
            return True
        return False

    # ----------------------------------------------------------------- operands
    def resolve(self, name: str) -> tuple[str, int | None]:
        """'vertex_attrib[3]' -> ('vertex.attrib', 3); 'R0' -> ('R0', None)."""
        m = re.fullmatch(r"([\w.]+?)(?:\[(\d+)\])?", name)
        if not m:
            return name, None
        base, idx = m.group(1), m.group(2)
        if base in self.p.alias:
            b, first = self.p.alias[base]
            return b, first + int(idx) if idx is not None else first
        return base, int(idx) if idx is not None else None

    def read(self, op: Operand, typ: str) -> str:
        """A 4-component operand as a GLSL expression of type `typ`."""
        p, t = self.p, op.text
        swz = "." + op.swizzle
        if t.startswith("{"):
            e = literal(split_args(t[1:-1]), typ)
            e = e + swz if op.swizzle != "xyzw" else e
        else:
            e, nat = self.source(t)
            e = convert(e + swz if op.swizzle != "xyzw" else e, nat, typ)
        if op.abs:
            e = f"abs({e})"
        if op.neg:
            e = f"(-{e})"
        return e

    def source(self, t: str) -> tuple[str, str]:
        p = self.p
        m = re.fullmatch(r"vertex\[(\d+)\]\.(position|attrib\[(\d+)\])", t)
        if m:
            if m.group(2) == "position":
                return "gl_in[%s].gl_Position" % m.group(1), "f"
            loc = int(m.group(3))
            p.input_locs.add(loc)
            return f"i_attr{loc}[{m.group(1)}]", "f"
        base, idx = self.resolve(t)
        if base in p.regs:
            return base, "u"
        if base in ("vertex.attrib", "fragment.attrib") or base == "vertex.position":
            loc = 0 if base == "vertex.position" else idx
            if p.stage == "geom":
                raise ShaderError(f"geometry input without vertex index: {t}")
            # Vertex inputs are declared by how they are read; varyings are always vec4 and
            # integer reads reinterpret their bits.
            p.input_locs.add(loc)
            if p.stage == "vert":
                return f"a{loc}", self.known_inputs.get(loc, "f")
            return f"i_attr{loc}", "f"
        if base == "fragment.position":
            return "gl_FragCoord", "f"
        if base == "fragment.facing":
            # +1.0 front, -1.0 back; integer reads see the float's bits (sign intact).
            return "vec4(gl_FrontFacing ? 1.0 : -1.0)", "f"
        m = re.fullmatch(r"(lmem\d+)\[(.+)\]", t)
        if m and m.group(1) in p.arrays:
            return f"{m.group(1)}[{self.array_index(m.group(2))}]", "u"
        if base == "vertex.instance":
            p.uses.add("instance")
            return "ivec4(gl_InstanceIndex - gl_BaseInstance)", "s"
        if base.startswith("invocation."):
            v = {"globalid": "gl_GlobalInvocationID", "groupid": "gl_WorkGroupID",
                 "localid": "gl_LocalInvocationID"}.get(base[11:])
            if base[11:] == "localindex":
                return "uvec4(gl_LocalInvocationIndex)", "u"
            if v:
                return f"uvec4({v}, 0u)", "u"
        raise ShaderError(f"unknown operand {t!r}")

    def attr_type(self, op: Operand, typ: str) -> None:
        """Record how a vertex input / varying is read (float or integer)."""
        if op.text.startswith("{"):
            return
        base, idx = self.resolve(op.text)
        if base in ("vertex.attrib", "fragment.attrib", "vertex.position"):
            loc = 0 if base == "vertex.position" else idx
            want = "f" if typ == "f" else "u"
            old = self.p.inputs.get(loc)
            if old and old != want and self.p.stage == "vert":
                raise ShaderError(f"vertex input {loc} read as both float and integer")
            self.p.inputs[loc] = "u" if "u" in (old, want) else "f"
            if want == "u" and self.p.stage == "frag":
                self.p.flat_inputs.add(loc)

    def operand(self, op: Operand, typ: str) -> str:
        self.attr_type(op, typ)
        return self.read(op, typ)

    # ----------------------------------------------------------------- destinations
    def dest(self, text: str) -> tuple[str, str, str]:
        """(GLSL lvalue, its type, write mask)."""
        p = self.p
        mask = "xyzw"
        m = re.fullmatch(r"(.*?)\.([xyzw]{1,4})", text)
        if m and not m.group(1).endswith(("result", "vertex")):
            text, mask = m.group(1), m.group(2)
        m = re.fullmatch(r"(lmem\d+)\[(.+)\]", text)
        if m and m.group(1) in p.arrays:
            return f"{m.group(1)}[{self.array_index(m.group(2))}]", "u", mask
        base, idx = self.resolve(text)
        if base in p.regs:
            return base, "u", mask
        if base == "result.attrib":
            p.outputs.add(idx)
            return f"o_attr{idx}", "f", mask
        if base == "result.color":
            idx = idx or 0
            p.colors.add(idx)
            return f"o_color{idx}", "f", mask
        if base == "result.position":
            return "gl_Position", "f", mask
        if base == "result.depth":
            if mask != "z":
                raise ShaderError("result.depth written other than .z")
            return "gl_FragDepth", "f1", "x"
        raise ShaderError(f"unknown destination {text!r}")

    def emit(self, s: str) -> None:
        self.p.body.append("    " * self.p.depth + s)

    def store(self, dst: str, expr: str, typ: str, cc: bool, sat: bool = False) -> None:
        # A conditional write (`R0.z (EQ.x)`) only changes components whose test passes.
        m = re.fullmatch(r"(.*?)\s*\(([A-Z]{2}(?:\.[xyzw]+)?)\)", dst)
        when = None
        if m:
            dst, when = m.group(1), self.cond(m.group(2), vector=True)
            if cc:
                raise ShaderError("conditional write that also sets the condition code")
        lv, dtyp, mask = self.dest(dst)
        if sat:
            if typ != "f":
                raise ShaderError(".SAT on an integer operation")
            expr = f"clamp({expr}, 0.0, 1.0)"
        if cc:
            self.emit(f"t{typ} = {expr};")
            expr = f"t{typ}"
            zero = {"f": "vec4(0.0)", "u": "uvec4(0u)", "s": "ivec4(0)"}[typ]
        if dtyp == "f1":
            if when:
                raise ShaderError("conditional depth write")
            self.emit(f"gl_FragDepth = {paren(convert(expr, typ, 'f'))}.z;")
        elif when:
            self.emit(f"{lv}.{mask} = mix({lv}, {convert(expr, typ, dtyp)}, {when}).{mask};")
        else:
            v = convert(expr, typ, dtyp)
            self.emit(f"{lv}.{mask} = {paren(v)}.{mask};" if mask != "xyzw" else f"{lv} = {v};")
        if cc:
            self.emit(f"cc.{mask} = notEqual({expr}, {zero}).{mask};")

    # ----------------------------------------------------------------- instructions
    def cbuffer_load(self, args: list[str], fmt: str, sat: bool, cc: bool) -> None:
        p = self.p
        m = re.fullmatch(r"(\w+)\[(.+)\]", args[1])
        if not m or m.group(1) not in p.cbuffers:
            raise ShaderError(f"LDC from {args[1]}")
        slot = p.cbuffers[m.group(1)]
        addr = m.group(2).replace(" ", "")
        if fmt == "U64":
            if slot != 14 or not addr.isdigit() or int(addr) % 8:
                raise ShaderError(f"64-bit load not a texture handle: {args[1]}")
            p.handles[args[0]] = int(addr) // 8
            return
        if slot == 14:
            raise ShaderError("non-handle load from buf14")
        p.used_cbuffers.add(slot)
        size = {"F32": 4, "U32": 4, "S32": 4, "F32X2": 8, "U32X2": 8, "S32X2": 8,
                "F32X4": 16, "U32X4": 16, "S32X4": 16}[fmt]
        cb = f"cb{slot}.d"
        if addr.isdigit():
            n = int(addr)
            if n % size:
                raise ShaderError(f"unaligned constant load {args[1]}")
            p.cbuffer_rows.setdefault(slot, set()).add(n >> 4)
            e = {16: f"{cb}[{n >> 4}]", 8: f"{cb}[{n >> 4}].{'zwzw' if n & 8 else 'xyxy'}",
                 4: f"uvec4({cb}[{n >> 4}].{SWZ[(n >> 2) & 3]})"}[size]
        else:
            p.cbuffer_computed.add(slot)
            a = self.address(addr)
            if size == 16:
                e = f"{cb}[({a}) >> 4]"
            elif size == 8:
                e = f"yw4_ld2({cb}[({a}) >> 4], ({a}) >> 3)"
                p.uses.add("ld2")
            else:
                e = f"uvec4({cb}[({a}) >> 4][(({a}) >> 2) & 3u])"
        if sat:
            self.store(args[0], f"uintBitsToFloat({e})", "f", cc, sat=True)
        else:
            self.store(args[0], e, "u", cc)

    def array_index(self, idx: str) -> str:
        """Index of a local array (`lmem0[3]`, `lmem0[R0.x]`, `lmem0[R0.x + 1]`)."""
        idx = idx.replace(" ", "")
        if idx.isdigit():
            return idx
        m = re.fullmatch(r"([A-Z]\w*)\.([xyzw])(?:\+(\d+))?", idx)
        if not m or m.group(1) not in self.p.regs:
            raise ShaderError(f"array index {idx}")
        e = f"int({m.group(1)}.{m.group(2)})"
        return f"{e} + {m.group(3)}" if m.group(3) else e

    def address(self, addr: str) -> str:
        """'R0.x+16' -> GLSL uint expression of a byte address."""
        if addr.isdigit():
            return addr + "u"
        m = re.fullmatch(r"([A-Z]\w*)\.([xyzw])(?:\+(\d+))?", addr)
        if not m or m.group(1) not in self.p.regs:
            raise ShaderError(f"address {addr}")
        e = f"{m.group(1)}.{m.group(2)}"
        return f"{e} + {m.group(3)}u" if m.group(3) else e

    def texture(self, op: str, args: list[str], typ_mod: str, sat: bool) -> None:
        p = self.p
        m = re.fullmatch(r"handle\((D\d+\.[xyzw])\)", args[2])
        if not m or m.group(1) not in p.handles:
            raise ShaderError(f"texture handle {args[2]}")
        slot, dim = p.handles[m.group(1)], args[3]
        if p.textures.get(slot, dim) != dim:
            raise ShaderError(f"texture slot {slot} used as {p.textures[slot]} and {dim}")
        p.textures[slot] = dim
        t = f"tex{slot}"
        c = parse_operand(args[1])
        if op == "TXQ":
            if dim == "CUBE" and args[0].endswith(".w"):
                self.store(args[0], f"ivec4(textureQueryLevels({t}))", "s", False)
            elif dim == "2D":
                lod = self.operand(c, "s")
                self.store(args[0], f"ivec4(textureSize({t}, {lod}.x), 0, textureQueryLevels({t}))", "s", False)
            else:
                raise ShaderError(f"TXQ {dim}")
            return
        if op == "TXF":
            ci = self.operand(c, "s")
            self.store(args[0], f"texelFetch({t}, {ci}.xy, {ci}.w)", "f", False, sat)
            return
        cf = self.operand(c, "f")
        lod_needed = op == "TXL" or p.stage != "frag"
        lod = f"{cf}.w" if op == "TXL" else "0.0"
        # Coordinates per target (NV_gpu_program4): array layer in z; shadow reference
        # in z (SHADOW2D) or w (SHADOWARRAY2D); TXL's level of detail in w.
        n = {"2D": "xy", "3D": "xyz", "CUBE": "xyz", "ARRAY2D": "xyz", "SHADOW2D": "xyz",
             "SHADOWARRAY2D": "xyzw"}.get(dim)
        if n is None:
            raise ShaderError(f"texture target {dim}")
        soft = not lod_needed and p.key in PCSS_SHADERS
        if dim == "SHADOWARRAY2D":
            if op == "TXL":
                raise ShaderError("TXL on SHADOWARRAY2D")
            if soft:
                p.depth_reads.add(slot)
                e = f"vec4(yw4_shadow({t}, {t}_depth, {cf}))"
            else:
                e = (f"vec4(textureGrad({t}, {cf}, vec2(0.0), vec2(0.0)))" if lod_needed
                     else f"vec4(texture({t}, {cf}))")
        elif dim == "SHADOW2D":
            if soft:
                p.depth_reads.add(slot)
                e = f"vec4(yw4_shadow({t}, {t}_depth, {cf}.xyz))"
            else:
                e = f"vec4(textureLod({t}, {cf}.xyz, {lod}))" if lod_needed else f"vec4(texture({t}, {cf}.xyz))"
        else:
            e = f"textureLod({t}, {cf}.{n}, {lod})" if lod_needed else f"texture({t}, {cf}.{n})"
        self.store(args[0], e, "f", False, sat)

    def image_store(self, args: list[str]) -> None:
        p = self.p
        m = re.fullmatch(r"handle\((D\d+\.[xyzw])\)", args[0])
        if not m or m.group(1) not in p.handles or args[3] != "2D":
            raise ShaderError(f"STOREIM {args}")
        slot = p.handles[m.group(1)]
        p.images[slot] = "2D"
        data = self.operand(parse_operand(args[1]), "f")
        coord = self.operand(parse_operand(args[2]), "s")
        self.emit(f"imageStore(img{slot}, {coord}.xy, {data});")

    def instruction(self, ln: str) -> None:
        p = self.p
        m = re.fullmatch(r"([A-Z0-9_]+)((?:\.[A-Z0-9]+)*)\s*(.*)", ln)
        if not m:
            raise ShaderError(f"cannot parse {ln!r}")
        op, mods, rest = m.group(1), m.group(2).split(".")[1:], m.group(3).strip()
        cc = "CC" in mods
        sat = "SAT" in mods
        tmod = next((x for x in mods if x in ("F", "F32", "F16", "S", "U", "U32", "S32", "U64",
                                               "F32X2", "F32X4", "U32X2", "U32X4", "S32X2", "S32X4")), None)
        typ = {"S": "s", "S32": "s", "U": "u", "U32": "u"}.get(tmod, "f")
        args = split_args(rest)
        self.emit(f"// {ln}")

        if op == "IF":
            self.emit(f"if ({self.cond(rest)}) {{")
            p.depth += 1
        elif op == "ELSE":
            p.depth -= 1
            self.emit("} else {")
            p.depth += 1
        elif op in ("ENDIF", "ENDREP"):
            p.depth -= 1
            self.emit("}")
        elif op == "REP":
            if not rest:
                self.emit("for (;;) {")
            else:
                n = re.fullmatch(r"\{(\d+), [^}]*\}(?:\.x)?", rest)
                if not n:
                    raise ShaderError(f"REP count {rest}")
                v = f"rep{p.depth}"
                self.emit(f"for (int {v} = 0; {v} < {n.group(1)}; {v}++) {{")
            p.depth += 1
        elif op == "BRK":
            self.emit(f"if ({self.cond(rest.strip('()'))}) break;" if rest else "break;")
        elif op == "KIL":
            self.emit(f"if ({self.cond(rest)}) discard;")
        elif op == "EMIT":
            if p.stage == "geom":
                self.emit("yw4_fix_position();")
            self.emit("EmitVertex();")
        elif op == "ENDPRIM":
            self.emit("EndPrimitive();")
        elif op == "BAR":
            self.emit("barrier();")
        elif op == "MEMBAR":
            self.emit("memoryBarrierShared();")
        elif op == "LDC":
            self.cbuffer_load(args, tmod, sat, cc)
        elif op in ("TEX", "TXL", "TXF", "TXQ"):
            self.texture(op, args, tmod, sat)
        elif op == "STOREIM":
            self.image_store(args)
        elif op in ("LDB", "STB", "LDS", "STS", "ATOMS", "ATOMB"):
            self.memory(op, mods, args)
        else:
            self.alu(op, typ, tmod, args, cc, sat)

    def cond(self, s: str, vector: bool = False) -> str:
        """A condition-code test. cc holds "value != 0", so NE is cc and EQ is !cc.
        Scalar for IF/BRK/KIL; a bvec4 for conditional writes (`R0.x (EQ.x)`)."""
        m = re.fullmatch(r"(NE|EQ)(?:\.([xyzw]{1,4}))?", s.strip())
        if not m or (not vector and (not m.group(2) or len(m.group(2)) != 1)):
            raise ShaderError(f"condition {s!r}")
        swz = m.group(2) or "xyzw"
        swz = (swz + swz[-1] * 4)[:4] if vector else swz
        return f"cc.{swz}" if m.group(1) == "NE" else f"not(cc.{swz})" if vector else f"!cc.{swz}"

    def memory(self, op: str, mods: list[str], args: list[str]) -> None:
        p = self.p
        mem = args[-1]
        m = re.fullmatch(r"(\w+)\[(.+)\]", mem)
        if not m:
            raise ShaderError(f"memory operand {mem}")
        a = self.address(m.group(2).replace(" ", ""))
        if m.group(1) in p.storage:
            arr = f"sb{p.storage[m.group(1)]}.d"
        elif m.group(1) == p.shared:
            arr = "shared_mem"
        else:
            raise ShaderError(f"memory {m.group(1)}")
        elem = f"{arr}[({a}) >> 2]".replace("(0u) >> 2", "0")
        if "U32" not in mods:
            raise ShaderError(f"{op} type {mods}")
        if op in ("LDB", "LDS"):
            self.store(args[0], f"uvec4({elem})", "u", False)
        elif op in ("STB", "STS"):
            self.emit(f"{elem} = {self.operand(parse_operand(args[0]), 'u')}.x;")
        else:
            fn = {"MIN": "atomicMin", "MAX": "atomicMax", "ADD": "atomicAdd"}[mods[0]]
            if op == "ATOMB" and arr.startswith("sb"):
                p.written_storage.add(int(arr[2:arr.index(".")]))
            self.store(args[0], f"uvec4({fn}({elem}, {self.operand(parse_operand(args[1]), 'u')}.x))", "u", False)

    def alu(self, op: str, typ: str, tmod: str | None, args: list[str], cc: bool, sat: bool) -> None:
        srcs = [parse_operand(a) for a in args[1:]]

        def s(i: int, t: str = typ) -> str:
            return self.operand(srcs[i], t)

        f = typ == "f"
        plain = not sat and not srcs[0].neg and not srcs[0].abs and not srcs[0].text.startswith("{")
        if op == "MOV" and plain and (not cc or typ == "u"):
            # A plain MOV copies bits whatever its suffix: it neither converts nor tells
            # us how an input is typed. (MOV.F.CC still tests as a float, below.)
            self.store(args[0], self.read(srcs[0], "u"), "u", cc)
            return
        if op == "BFI":
            # First operand: x = field width, y = offset (seen as {8, 0}, {8, 8}, {8, 16}).
            w = s(0, "s")
            self.store(args[0], f"bitfieldInsert({s(2)}, {s(1)}, {w}.y, {w}.x)", typ, cc)
            return
        if op == "NOT":
            self.store(args[0], f"~{s(0)}", typ, cc)
            return
        if op in ("MOV", "ADD", "MUL", "MAD", "MIN", "MAX", "DIV", "AND", "OR", "SHR", "SHL"):
            e = {"MOV": lambda: s(0), "ADD": lambda: f"{s(0)} + {s(1)}", "MUL": lambda: f"{s(0)} * {s(1)}",
                 "MAD": lambda: f"fma({s(0)}, {s(1)}, {s(2)})" if f else f"{s(0)} * {s(1)} + {s(2)}",
                 "MIN": lambda: f"min({s(0)}, {s(1)})", "MAX": lambda: f"max({s(0)}, {s(1)})",
                 "DIV": lambda: f"{s(0)} / {s(1)}", "AND": lambda: f"{s(0)} & {s(1)}",
                 "OR": lambda: f"{s(0)} | {s(1)}", "SHR": lambda: f"{s(0)} >> {s(1, 'u')}",
                 "SHL": lambda: f"{s(0)} << {s(1, 'u')}"}[op]()
            if f and op in ("AND", "OR", "SHR", "SHL"):
                raise ShaderError(f"{op} on floats")
            self.store(args[0], e, typ, cc, sat)
        elif op in ("DP2", "DP3", "DP4"):
            n = {"DP2": ".xy", "DP3": ".xyz", "DP4": ""}[op]
            self.store(args[0], f"vec4(dot({s(0)}{n}, {s(1)}{n}))", "f", cc, sat)
        elif op in ("RCP", "RSQ", "EX2", "LG2", "SIN", "COS"):
            x = f"{s(0)}.x"
            e = {"RCP": f"1.0 / {x}", "RSQ": f"inversesqrt(abs({x}))", "EX2": f"exp2({x})",
                 "LG2": f"log2({x})", "SIN": f"sin({x})", "COS": f"cos({x})"}[op]
            self.store(args[0], f"vec4({e})", "f", cc, sat)
        elif op in ("FRC", "FLR", "TRUNC", "CEIL", "ROUND"):
            # ROUND: Maxwell rounds to nearest even.
            fn = {"FRC": "fract", "FLR": "floor", "TRUNC": "trunc", "CEIL": "ceil", "ROUND": "roundEven"}[op]
            e = f"{fn}({s(0, 'f')})"
            if typ == "u":
                e = f"uvec4(max({e}, 0.0))"
            elif typ == "s":
                e = f"ivec4({e})"
            self.store(args[0], e, typ, cc, sat)
        elif op == "I2F":
            self.store(args[0], f"vec4({s(0)})", "f", cc, sat)
        elif op in ("SEQ", "SNE", "SLT", "SGE", "SGT"):
            fn = {"SEQ": "equal", "SNE": "notEqual", "SLT": "lessThan", "SGE": "greaterThanEqual",
                  "SGT": "greaterThan"}[op]
            b = f"{fn}({s(0)}, {s(1)})"
            if f:
                self.store(args[0], f"vec4({b})", "f", cc, sat)
            else:
                self.store(args[0], f"mix(uvec4(0u), uvec4(0xFFFFFFFFu), {b})", "u", cc)
        elif op in ("DDX", "DDY"):
            self.store(args[0], f"{'dFdx' if op == 'DDX' else 'dFdy'}({s(0)})", "f", cc, sat)
        else:
            raise ShaderError(f"unknown instruction {op}")

    # ----------------------------------------------------------------- whole program
    def translate(self) -> tuple[str, dict]:
        # Pass 1 learns how each vertex input is read (float or integer), which decides
        # its declared type; pass 2 emits code knowing it.
        for final in (False, True):
            known, flat = self.p.inputs, self.p.flat_inputs
            self.p = Program(self.p.key, self.p.stage)
            self.known_inputs = known if final else {}
            self.p.flat_inputs = set(flat)
            for ln in self.lines:
                if ln == "END":
                    break
                if not self.declare(ln):
                    self.instruction(ln)
            if self.p.depth != 1:
                raise ShaderError("unbalanced control flow")
        return self.glsl(), self.interface()

    def interface(self) -> dict:
        p = self.p
        return {"stage": p.stage, "set": SET[p.stage],
                "cbuffers": sorted(p.used_cbuffers),
                "cbuffer_rows": {str(k): sorted(v) for k, v in sorted(p.cbuffer_rows.items())},
                "cbuffer_computed": sorted(p.cbuffer_computed), "textures": {str(k): v for k, v in sorted(p.textures.items())},
                "images": {str(k): v for k, v in sorted(p.images.items())},
                "depth": {str(k): p.textures[k].removeprefix("SHADOW") for k in sorted(p.depth_reads)},
                "storage": sorted(p.storage.values()),
                "inputs": {str(k): p.inputs.get(k, "f") for k in sorted(p.input_locs)},
                "outputs": sorted(p.outputs), "colors": sorted(p.colors),
                "group_size": p.group_size if p.stage == "comp" else None}

    def glsl(self) -> str:
        p = self.p
        s = SET[p.stage]
        o = ["#version 460",
             f"// {p.key}.{p.stage}: translated from NV_gpu_program5 by recomp/shader.py (decision 0020)."
             " Derived from the game's code: never commit."]
        if p.stage == "comp":
            o.append("layout(local_size_x = %d, local_size_y = %d, local_size_z = %d) in;" % tuple(p.group_size))
        if p.stage == "geom":
            prim_in = {"TRIANGLES": "triangles", "POINTS": "points", "LINES": "lines"}[p.gs["PRIMITIVE_IN"]]
            prim_out = {"TRIANGLE_STRIP": "triangle_strip", "POINTS": "points", "LINE_STRIP": "line_strip"}[p.gs["PRIMITIVE_OUT"]]
            o += [f"layout({prim_in}) in;", f"layout({prim_out}, max_vertices = {p.gs['VERTICES_OUT']}) out;"]
        for slot in sorted(p.used_cbuffers):
            o.append(f"layout(set = {s}, binding = {slot}, std140) uniform CB{slot} {{ uvec4 d[4096]; }} cb{slot};")
        samp = {"2D": "sampler2D", "3D": "sampler3D", "CUBE": "samplerCube", "SHADOW2D": "sampler2DShadow",
                "ARRAY2D": "sampler2DArray", "SHADOWARRAY2D": "sampler2DArrayShadow"}
        for slot, dim in sorted(p.textures.items()):
            o.append(f"layout(set = {s}, binding = {TEX_BINDING + slot}) uniform {samp[dim]} tex{slot};")
        for slot in sorted(p.images):
            if slot in p.textures:
                raise ShaderError(f"slot {slot} used as texture and image")
            o.append(f"layout(set = {s}, binding = {TEX_BINDING + slot}) uniform writeonly image2D img{slot};")
        for name, n in sorted(p.storage.items(), key=lambda kv: kv[1]):
            o.append(f"layout(set = {s}, binding = {STORAGE_BINDING + n}, std430) buffer SB{n} {{ uint d[]; }} sb{n};")
        if p.shared:
            o.append(f"shared uint shared_mem[{max(p.shared_size, 4) // 4}];")
        for loc in sorted(p.input_locs):
            t = p.inputs.get(loc, "f")
            if p.stage == "vert":
                o.append(f"layout(location = {loc}) in {'vec4' if t == 'f' else 'uvec4'} a{loc};")
            elif p.stage == "geom":
                o.append(f"layout(location = {loc}) in vec4 i_attr{loc}[];")
            else:
                o.append(f"layout(location = {loc}) {'flat ' if loc in p.flat_inputs else ''}in vec4 i_attr{loc};")
        for loc in sorted(p.outputs):
            o.append(f"layout(location = {loc}) out vec4 o_attr{loc};")
        for loc in sorted(p.colors):
            o.append(f"layout(location = {loc}) out vec4 o_color{loc};")
        if p.stage in ("vert", "geom"):
            o += ["layout(push_constant) uniform YW4 { uint pos_fix; } yw4;",
                  "void yw4_fix_position() {",
                  f"    if ((yw4.pos_fix & {1 if p.stage == 'vert' else 2}u) != 0u) "
                  "gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;",
                  "}"]
        if "ld2" in p.uses:
            o += ["uvec4 yw4_ld2(uvec4 v, uint half_index) {",
                  "    return (half_index & 1u) != 0u ? v.zwzw : v.xyxy;", "}"]
        if p.depth_reads:
            depth_samp = {"SHADOW2D": "sampler2D", "SHADOWARRAY2D": "sampler2DArray"}
            for slot in sorted(p.depth_reads):
                o.append(f"layout(set = {s}, binding = {DEPTH_BINDING + slot}) uniform "
                         f"{depth_samp[p.textures[slot]]} tex{slot}_depth;")
            o.append(f"layout(set = {s}, binding = {PCSS_BINDING}, std140) uniform YW4PCSS "
                     "{ vec4 mode; vec4 scale; vec4 tune; } yw4_pcss;")
            o += PCSS_COMMON.splitlines()
            for dim in sorted({p.textures[slot] for slot in p.depth_reads}):
                o += pcss_helper(dim).splitlines()
        o.append("void main() {")
        decl = ", ".join(f"{r} = uvec4(0u)" for r in p.regs)
        if decl:
            o.append(f"    uvec4 {decl};")
        for name, n in sorted(p.arrays.items()):
            o.append(f"    uvec4 {name}[{n}];")
        o += ["    vec4 tf; uvec4 tu; ivec4 ts;", "    bvec4 cc = bvec4(false);"]
        o += p.body
        if p.stage == "vert":
            o.append("    yw4_fix_position();")
        o.append("}")
        return "\n".join(o) + "\n"


# Soft shadows with contact hardening (PCSS), in the shaders listed in PCSS_SHADERS: their
# shadow map lookups go through yw4_shadow. Off (yw4_pcss.mode.x == 0) it is the game's lookup. On, it reads the raw
# depth around the point to find the casters in front of it (blocker search); their average
# distance from the receiver sets the filter radius: sharp at the contact, soft far from it
# (tree canopies). The game's shaders average 5 lookups a little apart with the same layer and
# reference; the first one is filtered and the rest reuse its result (yw4_seen), so the cost is
# one search + one filter per pixel and cascade. The compare taps use texture(), not
# textureGrad(.., 0, 0): on NVIDIA the latter on an array shadow map made pipeline builds ~10x
# slower (the map has one level, so both read the same texels). Casters are texels with a
# smaller depth than the reference (the maps are cleared to 1.0). The game's compare (GREATER)
# only says the result means "in shadow", not which way depth runs.
#   mode:  x on, y unused, z largest radius and search radius (0..1 map coordinates), w smallest radius
#   scale: radius per unit of depth gap, per cascade layer 0..3
#   tune:  x depth gap below which a texel isn't counted as a caster, y debug view (1 casters
#          found / 16, 2 radius / largest)
PCSS_COMMON = """\
const vec2 YW4_DISK[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));
vec2 yw4_seen_key[2];
float yw4_seen_lit[2];
int yw4_seen_n = 0;
mat2 yw4_pcss_rotation() {
    float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return mat2(cos(a), sin(a), -sin(a), cos(a));
}
float yw4_pcss_remember(vec2 key, float lit) {
    if (yw4_seen_n < 2) {
        yw4_seen_key[yw4_seen_n] = key;
        yw4_seen_lit[yw4_seen_n] = lit;
        yw4_seen_n++;
    }
    return lit;
}"""


def pcss_helper(dim: str) -> str:
    """yw4_shadow for a SHADOWARRAY2D (c: x, y, layer, reference) or SHADOW2D (x, y, reference) map."""
    if dim == "SHADOWARRAY2D":
        sig = "sampler2DArrayShadow s, sampler2DArray d, vec4 c"
        game, key, layer, ref = "texture(s, c)", "c.zw", "c.z", "c.w"
        fetch = "textureLod(d, vec3({uv}, c.z), 0.0).r"
        test = "texture(s, vec4({uv}, c.z, c.w))"
    else:
        sig = "sampler2DShadow s, sampler2D d, vec3 c"
        game, key, layer, ref = "texture(s, c)", "vec2(-1.0, c.z)", "0.0", "c.z"
        fetch = "textureLod(d, {uv}, 0.0).r"
        test = "texture(s, vec3({uv}, c.z))"
    tap = "c.xy + rot * YW4_DISK[i] * {r}"
    return f"""\
float yw4_shadow({sig}) {{
    if (yw4_pcss.mode.x < 0.5) return {game};
    if (yw4_pcss.tune.y == 3.0) return {test.format(uv="c.xy")};
    if (yw4_pcss.tune.y == 4.0) return {fetch.format(uv="c.xy")};
    if (yw4_pcss.tune.y == 5.0) return {ref} > {fetch.format(uv="c.xy")} ? 1.0 : 0.0;
    if (yw4_pcss.tune.y == 6.0) return {ref};
    for (int i = 0; i < yw4_seen_n; i++)
        if (yw4_seen_key[i] == {key}) return yw4_seen_lit[i];
    mat2 rot = yw4_pcss_rotation();
    float gaps = 0.0, casters = 0.0;
    for (int i = 0; i < 16; i++) {{
        float z = {fetch.format(uv=tap.format(r="yw4_pcss.mode.z"))};
        float gap = {ref} - z;
        if (gap > yw4_pcss.tune.x) {{ gaps += gap; casters += 1.0; }}
    }}
    if (yw4_pcss.tune.y == 1.0) return casters * (1.0 / 16.0);
    if (casters == 0.0) return yw4_pcss_remember({key}, {test.format(uv="c.xy")});
    float r = clamp(gaps / casters * yw4_pcss.scale[min(int({layer} + 0.5), 3)], yw4_pcss.mode.w, yw4_pcss.mode.z);
    if (yw4_pcss.tune.y == 2.0) return r / yw4_pcss.mode.z;
    float lit = 0.0;
    for (int i = 0; i < 16; i++)
        lit += {test.format(uv=tap.format(r="r"))};
    return yw4_pcss_remember({key}, lit * (1.0 / 16.0));
}}"""


def iface_text(iface: dict, names: dict[int, str] | None = None) -> str:
    """The interface for the runtime (gpu.c), one item per line:
    c <slot> [name] (constant buffer, with the engine's name for it from the shader's header),
    r <slot> <hex mask> | * (its 16-byte rows 0-63 the program reads; *: also at computed addresses),
    t <slot> <target> (texture), i <slot> (image),
    s <n> (storage buffer), a <location> f|u (vertex input type),
    d <slot> <target> (a shadow map's raw depth, for yw4_shadow), p 0 (yw4_shadow's settings)."""
    names = names or {}
    lines = [f"c {n} {names[n]}" if n in names else f"c {n}" for n in iface["cbuffers"]]
    computed = set(iface.get("cbuffer_computed", []))
    for n in iface["cbuffers"]:
        rows = iface.get("cbuffer_rows", {}).get(str(n), [])
        mask = sum(1 << r for r in rows if r < 64)
        lines.append(f"r {n} *" if n in computed or any(r >= 64 for r in rows) else f"r {n} {mask:x}")
    lines += [f"t {k} {v}" for k, v in iface["textures"].items()]
    lines += [f"d {k} {v}" for k, v in iface.get("depth", {}).items()]
    if iface.get("depth"):
        lines.append("p 0")
    lines += [f"i {k}" for k in iface["images"]]
    lines += [f"s {n}" for n in iface["storage"]]
    if iface["stage"] == "vert":
        lines += [f"a {k} {v}" for k, v in iface["inputs"].items()]
        if iface.get("position"):
            lines.append(f"m {iface['position']}")
    return "\n".join(lines) + "\n"


def position_source(text: str) -> str | None:
    """Where a vertex program's position comes from, when it is simple enough for the runtime to work
    out a draw's corners without running the program (gpu_draw.c, widescreen): "<slot> <row x> <row y>
    <row z> <row w>" when it is one constant buffer's 16-byte rows each dotted with vertex input 0 (DP4),
    as the UI's programs do; "in" when it is input 0 as it is (full-screen passes); else None."""
    slots = {m.group(1): int(m.group(2))
             for m in re.finditer(r"CBUFFER\s+(\w+)\[\]\s*=\s*\{\s*program\.buffer\[(\d+)\]", text)}
    rows: dict[tuple[str, str], tuple[int, int]] = {}  # (register, component) -> (slot, row) loaded there
    # (register, component) -> the row it holds dotted with input 0, or ("in", component of input 0)
    dots: dict[tuple[str, str], tuple] = {}
    pos: dict[str, tuple] = {}

    def dest(t: str) -> tuple[str, str]:
        m = re.fullmatch(r"(.*?)(?:\.([xyzw]+))?", t.strip())
        return m.group(1), m.group(2) or SWZ

    for ln in text.splitlines():
        m = re.match(r"\s*([A-Z0-9]+)(?:\.[A-Z0-9]+)*\s+(.*);", ln)
        if not m or ln.lstrip().startswith(("CBUFFER", "ATTRIB", "OUTPUT", "TEMP", "OPTION")):
            continue
        op, args = m.group(1), split_args(m.group(2))
        if not args:
            continue
        reg, mask = dest(args[0])
        values: dict[str, tuple[int, int] | None] = {}
        if op == "LDC" and len(args) == 2 and (c := re.fullmatch(r"(\w+)\[(\d+)\]", args[1])) and c.group(1) in slots:
            values = {k: (slots[c.group(1)], int(c.group(2)) >> 4) for k in mask}
            for k in mask:
                rows[reg, k] = values[k]
            for k in mask:
                dots.pop((reg, k), None)
            continue
        if op == "DP4" and len(args) == 3:
            a, b = parse_operand(args[1]), parse_operand(args[2])
            if b.text == "vertex.attrib[0]":
                a, b = b, a
            src = {rows.get((b.text, k)) for k in b.swizzle}
            ok = a.text == "vertex.attrib[0]" and a.swizzle == SWZ and not (a.neg or b.neg or a.abs or b.abs)
            row = src.pop() if ok and len(src) == 1 and b.swizzle == SWZ else None
            values = {k: row for k in mask}
        elif op == "MOV" and len(args) == 2:
            s = parse_operand(args[1])
            values = {k: None if s.neg or s.abs else
                      ("in", s.swizzle[SWZ.index(k)]) if s.text == "vertex.attrib[0]" else
                      dots.get((s.text, s.swizzle[SWZ.index(k)])) for k in mask}
        else:
            values = {k: None for k in mask}
        for k in mask:
            rows.pop((reg, k), None)
            if reg == "result.position":
                if values[k] is None:
                    pos.pop(k, None)
                else:
                    pos[k] = values[k]
            elif values[k] is None:
                dots.pop((reg, k), None)
            else:
                dots[reg, k] = values[k]
    if len(pos) != 4:
        return None
    if all(pos[k] == ("in", k) for k in SWZ):
        return "in"
    if len({s for s, _ in pos.values()}) != 1 or pos["x"][0] == "in":
        return None
    return " ".join(str(v) for v in [pos["x"][0]] + [pos[k][1] for k in SWZ])


def translate(key: str, stage: str, text: str) -> tuple[str, dict]:
    glsl, iface = Translator(key, stage, text).translate()
    if stage == "vert":
        iface["position"] = position_source(text)
    return glsl, iface


def glslang(cfg: config.Config) -> str | None:
    """[tools] glslang, else glslangValidator from the Vulkan SDK or PATH."""
    import os
    if cfg.glslang:
        return str(cfg.glslang) if cfg.glslang.is_file() else None
    sdk = os.environ.get("VULKAN_SDK")
    if sdk and (Path(sdk) / "Bin" / "glslangValidator.exe").is_file():
        return str(Path(sdk) / "Bin" / "glslangValidator.exe")
    return shutil.which("glslangValidator") or shutil.which("glslang")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="compile the GLSL to SPIR-V with glslangValidator")
    ap.add_argument("keys", nargs="*", help="only these shader keys")
    args = ap.parse_args(argv)
    cfg = config.load(require_exefs=False)
    src = cfg.work_dir / "shaders" / "asm"
    out = cfg.work_dir / "shaders" / "glsl"
    out.mkdir(parents=True, exist_ok=True)
    files = sorted(src.glob("*.asm"))
    if not files:
        print("no shaders: run python -m tools.yw4.shaders first", file=sys.stderr)
        return 1
    if args.keys:
        files = [f for f in files if f.name.split(".")[0] in args.keys]
    exe = glslang(cfg) if args.check else None
    if args.check and not exe:
        print("glslang not found: install the Vulkan SDK or set [tools] glslang", file=sys.stderr)
        return 1
    # the engine's names for the constant buffers, from the shaders' headers (tools/yw4/shaders.py)
    index_path = cfg.work_dir / "shaders" / "index.json"
    index = json.loads(index_path.read_text()) if index_path.is_file() else {}
    errors: dict[str, list[str]] = {}
    bad_compile = []
    written = []
    for f in files:
        key, stage = f.name.split(".")[:2]
        try:
            glsl, iface = translate(key, stage, f.read_text())
        except (ShaderError, KeyError, ValueError) as e:
            msg = f"{type(e).__name__}: {e}" if not isinstance(e, ShaderError) else str(e)
            errors.setdefault(re.sub(r"\d+", "N", msg)[:70], []).append(key)
            continue
        g = out / f"{key}.{stage}"
        g.write_text(glsl)
        (out / f"{key}.json").write_text(json.dumps(iface))
        names = {r["slot"]: r["name"] for r in index.get(key, {}).get("resources", []) if r["kind"] == "cbuffer"}
        (out / f"{key}.iface").write_text(iface_text(iface, names))
        written.append((key, stage, g))

    def compile_one(item):
        key, stage, g = item
        r = subprocess.run([exe, "-V", "--target-env", "vulkan1.2", "-o", str(g) + ".spv", str(g)],
                           capture_output=True, text=True)
        msgs = [ln for ln in (r.stdout + r.stderr).splitlines() if ln.startswith("ERROR") and "compilation" not in ln]
        return (key, stage, msgs) if r.returncode else None

    if exe:
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor() as pool:
            bad_compile = [b for b in pool.map(compile_one, written) if b]
    done = len(files) - sum(len(v) for v in errors.values())
    print(f"translated {done}/{len(files)}")
    for msg, keys in sorted(errors.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(keys):4} {msg}  (e.g. {keys[0]})")
    if exe:
        print(f"glslang: {done - len(bad_compile)}/{done} compile")
        kinds: dict[str, list[str]] = {}
        for key, stage, msg in bad_compile:
            first = re.sub(r"^ERROR: \S+:\d+: ", "", msg[0]) if msg else "(no message)"
            kinds.setdefault(re.sub(r"\d+", "N", first)[:110], []).append(f"{key}.{stage}")
        for msg, keys in sorted(kinds.items(), key=lambda kv: -len(kv[1]))[:12]:
            print(f"  {len(keys):4} {msg}  (e.g. {keys[0]})")
    return 0 if not errors and not bad_compile else 1


if __name__ == "__main__":
    sys.exit(main())
