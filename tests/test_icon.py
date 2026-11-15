"""The game's icon (tools/yw4/icon.py): the launcher's picture, icon.jpg, is written on every system, and the exe's
yw4.ico only on Windows. On Linux the .ico's "needs Windows" error once came before the JPEG was copied, so the
launcher showed no icon and no banner. The game's own files are faked: no game data here (rule 1)."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.yw4 import icon

JPEG = b"\xff\xd8\xff\xe0fake icon\xff\xd9"


def fake_jpeg(hactool, home, ncas, tmp: Path) -> Path:
    p = tmp / "icon_AmericanEnglish.dat"
    p.write_bytes(JPEG)
    return p


def fake_ico(jpeg: Path, ico: Path, tmp: Path) -> None:
    ico.write_bytes(b"ico")


class IconTest(unittest.TestCase):
    def make(self, platform: str) -> tuple[Path, Path]:
        work = Path(self.enterContext(tempfile.TemporaryDirectory()))
        cfg = SimpleNamespace(work_dir=work, hactool_exe=None)
        with mock.patch.object(icon, "icon_jpeg", fake_jpeg), mock.patch.object(icon, "jpeg_to_ico", fake_ico), \
                mock.patch.object(icon.sys, "platform", platform):
            return icon.make(cfg, work, []), work / "icon"

    def test_linux_writes_the_launchers_picture(self):
        made, out = self.make("linux")
        self.assertEqual(made, out / "icon.jpg")
        self.assertEqual((out / "icon.jpg").read_bytes(), JPEG)
        self.assertFalse((out / "yw4.ico").exists())

    def test_windows_writes_both(self):
        made, out = self.make("win32")
        self.assertEqual(made, out / "yw4.ico")
        self.assertEqual((out / "icon.jpg").read_bytes(), JPEG)
        self.assertTrue((out / "yw4.ico").is_file())


if __name__ == "__main__":
    unittest.main()
