"""Linux's own build tools (decision 0083): FSR's patches applied in Python, and the Zig compiler scripts."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.yw4 import deps, fsr_linux

PATCH = """\
diff --git a/src/a.c b/src/a.c
index 1111111..2222222 100644
--- a/src/a.c
+++ b/src/a.c
@@ -2,4 +2,4 @@
 two
--- dashes
+++ pluses
 four

@@ -9,2 +9,3 @@ int f(void)
 nine
+nine and a half
 ten
"""


class ApplyPatch(unittest.TestCase):
    def run_patch(self, original: bytes, patch: str = PATCH) -> bytes:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src").mkdir()
            (root / "src" / "a.c").write_bytes(original)
            (root / "p.patch").write_text(patch)
            fsr_linux.apply_patch(root / "p.patch", root)
            return (root / "src" / "a.c").read_bytes()

    def test_hunks_lf(self):
        lines = ["one", "two", "-- dashes", "four", "", "six", "seven", "eight", "nine", "ten"]
        out = self.run_patch(("\n".join(lines) + "\n").encode())
        want = ["one", "two", "++ pluses", "four", "", "six", "seven", "eight", "nine", "nine and a half", "ten"]
        self.assertEqual(out.decode(), "\n".join(want) + "\n")

    def test_crlf_kept(self):
        lines = ["one", "two", "-- dashes", "four", "", "six", "seven", "eight", "nine", "ten"]
        out = self.run_patch(("\r\n".join(lines) + "\r\n").encode())
        self.assertIn(b"++ pluses\r\n", out)
        self.assertIn(b"nine and a half\r\n", out)
        self.assertNotIn(b"\n", out.replace(b"\r\n", b""))

    def test_moved_hunk_found(self):
        # two extra lines at the top: the hunks are found two lines further down, as patch and git apply do
        lines = ["extra", "extra", "one", "two", "-- dashes", "four", "", "six", "seven", "eight", "nine", "ten"]
        out = self.run_patch(("\n".join(lines) + "\n").encode()).decode()
        self.assertIn("++ pluses\nfour", out)
        self.assertIn("nine\nnine and a half\nten", out)

    def test_misfit_writes_nothing(self):
        original = b"one\ntwo\nsomething else\nfour\n\nsix\nseven\neight\nnine\nten\n"
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src").mkdir()
            (root / "src" / "a.c").write_bytes(original)
            (root / "p.patch").write_text(PATCH)
            with self.assertRaises(RuntimeError):
                fsr_linux.apply_patch(root / "p.patch", root)
            self.assertEqual((root / "src" / "a.c").read_bytes(), original)

    def test_shipped_patches_parse(self):
        # every hunk of the patches we ship is read with as many lines as its header counts
        for p in sorted((fsr_linux.PORT / "patches").glob("*.patch")):
            text = p.read_text(encoding="utf-8")
            self.assertGreater(text.count("\n@@ "), 0, p.name)


@unittest.skipIf(sys.platform == "win32" or not shutil.which("sh"), "the compiler scripts are POSIX shell")
class ZigWrapper(unittest.TestCase):
    def test_march_names_and_g0(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / "bin").mkdir()
            fake = out / "zig"  # prints what it was given
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            fake.chmod(0o755)
            script = out / "bin" / "zig-clang"
            script.write_text(deps.ZIG_WRAPPER.format(mode="cc"))
            script.chmod(0o755)
            got = subprocess.run([str(script), "-O2", "-march=x86-64-v3", "-c", "a b.c"], capture_output=True,
                                 text=True, check=True).stdout.splitlines()
            self.assertEqual(got, ["cc", "-g0", "-O2", "-march=x86_64_v3", "-c", "a b.c"])


if __name__ == "__main__":
    unittest.main()
