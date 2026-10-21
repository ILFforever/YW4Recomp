"""The HBH1 shader container parser (tools/yw4/shaders.py).

Built from a synthetic container with the layout measured on the game's files, so no
game data is needed. The shader key (FNV-1a 64 of the assembly) must match the runtime's.
"""

from __future__ import annotations

import struct
import unittest

from tools.yw4 import shaders

ASM = b"!!NVfp5.0\nOPTION NV_bindless_texture;\nMOV.F result.color, {1, 0, 0, 1};\nEND"


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


if __name__ == "__main__":
    unittest.main()
