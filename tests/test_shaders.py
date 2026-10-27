"""The HBH1 shader container parser (tools/yw4/shaders.py).

Built from a synthetic container with the layout measured on the game's files, so no
game data is needed. The shader key (FNV-1a 64 of the assembly) must match the runtime's.
"""

from __future__ import annotations

import struct
import unittest

from tools.yw4 import shaders

ASM = (b"!!NVfp5.0\nOPTION NV_bindless_texture;\nTEMP R0;\nMOV.U.CC R0.x, {1, 0, 0, 0};\n"
       b"IF NE.x;\nMOV.F result.color, {1, 0, 0, 1};\nENDIF;\nREP.S ;\nBRK (NE.x);\nENDREP;\nEND")


def container() -> bytes:
    strings = b"in_POSITION0\0t_color\0u_rate\0"
    inputs = struct.pack("<IIII", 0, 0x1234, 2, 0)
    resources = struct.pack("<IIHH", 13, 0x5678, 3, 0).ljust(0x20, b"\0")
    members = struct.pack("<IIHHHH", 21, 0x9ABC, 2, 1, 4, 8)
    unknown = b"\0" * 0x10
    code = b"\x11" * 0x40
    control = b"\0" * 0x20 + ASM + b"\n\0\0\0"
    nxgl = b"NXGL" + struct.pack("<5I", 1, 0x20, len(code), 0x20 + len(code), len(control)).ljust(0x1C, b"\0")
    nxgl += code + control
    body = [inputs, resources, members, unknown, nxgl, strings]
    off, table, blob = 4 + 8 * 6, b"", b""
    for part in body:
        table += struct.pack("<II", off + len(blob), len(part))
        blob += part
    return b"HBH1" + table + blob


class ShaderTest(unittest.TestCase):
    def test_fnv1a64(self):
        # Published FNV-1a 64 test vectors.
        self.assertEqual(shaders.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(shaders.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(shaders.fnv1a64(b"foobar"), 0x85944171F73967E8)

    def test_parse(self):
        s = shaders.parse(container())
        self.assertEqual(s["stage"], "frag")
        self.assertEqual(s["text"], ASM)
        self.assertEqual(s["code_size"], 0x40)
        self.assertEqual(s["inputs"], [{"name": "in_POSITION0", "location": 2}])
        self.assertEqual(s["resources"], [{"name": "t_color", "kind": "texture", "slot": 0}])
        self.assertEqual(s["members"], [{"name": "u_rate", "type": 2, "count": 1, "size": 4, "offset": 8}])

    def test_rejects(self):
        with self.assertRaises(shaders.ShaderError):
            shaders.parse(b"XXXX" + container()[4:])
        with self.assertRaises(shaders.ShaderError):
            shaders.assembly(b"!!NVfp5.0\nMOV.F result.color, 0;\n")


FRAG = """!!NVfp5.0
OPTION NV_internal;
OPTION NV_bindless_texture;
CBUFFER buf2[] = { program.buffer[2] };
CBUFFER buf14[] = { program.buffer[14] };
ATTRIB fragment_attrib[] = { fragment.attrib[0..3] };
TEMP R0, R1;
LONG TEMP D0;
OUTPUT result_color0 = result.color;
LDC.U64 D0.x, buf14[16];
TEX.F R0, fragment.attrib[3], handle(D0.x), 2D;
LDC.F32X4 R1, buf2[R0.x + 32];
MAD.F32.SAT R1, R0, -R1.x, |R1|;
TRUNC.U.CC R0.x, R1.y;
IF NE.x;
MOV.F R1.y (EQ.x), {0.5, 0, 0, 0}.x;
ELSE;
KIL NE.x;
ENDIF;
SGE.S R0.z, R0.x, {0xffffffff, 0, 0, 0}.x;
MOV.F result_color0, R1;
END"""


class TranslatorTest(unittest.TestCase):
    def test_fragment(self):
        from recomp import shader
        glsl, iface = shader.translate("k", "frag", FRAG)
        for line in (
            "layout(set = 1, binding = 2, std140) uniform CB2 { uvec4 d[4096]; } cb2;",
            "layout(set = 1, binding = 18) uniform sampler2D tex2;",       # buf14[16] -> slot 2
            "layout(location = 3) in vec4 i_attr3;",
            "R0 = floatBitsToUint(texture(tex2, i_attr3.xy));",
            "R1 = cb2.d[(R0.x + 32u) >> 4];",
            "R1 = floatBitsToUint(clamp(fma(uintBitsToFloat(R0), (-uintBitsToFloat(R1.xxxx)), "
            "abs(uintBitsToFloat(R1))), 0.0, 1.0));",
            "tu = uvec4(max(trunc(uintBitsToFloat(R1.yyyy)), 0.0));",
            "cc.x = notEqual(tu, uvec4(0u)).x;",
            "if (cc.x) {",
            "R1.y = mix(R1, floatBitsToUint(vec4(0.5, 0.0, 0.0, 0.0).xxxx), not(cc.xxxx)).y;",
            "if (cc.x) discard;",
            "R0.z = mix(uvec4(0u), uvec4(0xFFFFFFFFu), greaterThanEqual(ivec4(R0.xxxx), ivec4(-1, 0, 0, 0).xxxx)).z;",
            "o_color0 = uintBitsToFloat(R1);",
        ):
            self.assertIn(line, glsl)
        self.assertEqual(iface["textures"], {"2": "2D"})
        self.assertEqual(iface["cbuffers"], [2])
        self.assertEqual(iface["colors"], [0])

    def test_position_source(self):
        from recomp import shader
        head = ("!!NVvp5.0\nOPTION NV_internal;\nCBUFFER buf2[] = { program.buffer[2] };\n"
                "ATTRIB vertex_attrib[] = { vertex.attrib[0..1] };\nTEMP R0, R1, R2;\n")
        # straight into the position, and through a register with a moved component
        direct = ("LDC.F32X4 R0, buf2[48];\nDP4.F32 result.position.w, R0, vertex.attrib[0];\n"
                  "LDC.F32X4 R0, buf2[0];\nDP4.F32 result.position.x, vertex.attrib[0], R0;\n"
                  "LDC.F32X4 R0, buf2[16];\nDP4.F32 result.position.y, vertex.attrib[0], R0;\n"
                  "LDC.F32X4 R0, buf2[32];\nDP4.F32 result.position.z, vertex.attrib[0], R0;\n")
        moved = ("LDC.F32X4 R0, buf2[128];\nDP4.F32 R2.x, vertex.attrib[0], R0;\n"
                 "LDC.F32X4 R0, buf2[176];\nDP4.F32 R1.x, R0, vertex.attrib[0];\n"
                 "LDC.F32X4 R0, buf2[144];\nDP4.F32 R2.y, vertex.attrib[0], R0;\n"
                 "LDC.F32X4 R0, buf2[160];\nDP4.F32 R2.z, vertex.attrib[0], R0;\n"
                 "MOV.F R2.w, R1.x;\nMOV.F result.position, R2;\n")
        self.assertEqual(shader.position_source(head + direct + "END"), "2 0 1 2 3")
        self.assertEqual(shader.position_source(head + moved + "END"), "2 8 9 10 11")
        # anything else on the way: not a plain matrix
        for bad in ("ADD.F32 R2.x, R2, R1;\n", "LDC.F32X4 R0, buf2[64];\nMOV.F R2.w, R0.x;\n"):
            self.assertIsNone(shader.position_source(head + moved.replace("MOV.F result", bad + "MOV.F result") + "END"))
        self.assertIsNone(shader.position_source(head + direct.replace("vertex.attrib[0]", "vertex.attrib[1]") + "END"))
        # a full-screen pass: the input as it is (a moved component is not)
        self.assertEqual(shader.position_source(head + "MOV.F result.position, vertex.attrib[0];\nEND"), "in")
        self.assertIsNone(shader.position_source(head + "MOV.F result.position, vertex.attrib[0].yxzw;\nEND"))
        self.assertIn("m 2 0 1 2 3", shader.iface_text(shader.translate("k", "vert", head + direct + "END")[1]))

    def test_rejects(self):
        from recomp import shader
        for bad in ("LDC.U64 D0.x, buf14[R0.x];", "LDC.F32 R0.x, buf14[0];", "FOO.F R0, R1;",
                    "IF GT.x;", "TEX.F R0, R1, handle(D9.x), 2D;"):
            with self.assertRaises(shader.ShaderError, msg=bad):
                shader.translate("k", "frag", f"!!NVfp5.0\nCBUFFER buf14[] = {{ program.buffer[14] }};\n"
                                              f"TEMP R0, R1;\n{bad}\nEND")


if __name__ == "__main__":
    unittest.main()
