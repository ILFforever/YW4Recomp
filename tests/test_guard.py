"""tools/yw4/guard.py: what it refuses to commit (CLAUDE.md rule 1, and user names in paths)."""

import os
import unittest
from unittest import mock

from tools.yw4 import guard


class GuardTest(unittest.TestCase):
    def test_game_data_signature(self):
        self.assertIn("NSO", guard.problem("x.bin", b"NSO0" + bytes(60)))

    def test_windows_home_with_a_real_name(self):
        for text in (rb"see C:\Users\someone\Desktop\x", b"D:/Users/someone/x", rb"C:\\Users\\someone\\x"):
            self.assertIn("home folder", guard.problem("notes.md", text), text)

    def test_placeholders_and_ci_runners_pass(self):
        for text in (rb"C:\Users\<name>\AppData", b"C:/Users/runneradmin/work", rb"C:\Users\RUNNER~1\x",
                     rb"C:\Users\%USERNAME%\x", rb"C:\Users\Public\x"):
            self.assertIsNone(guard.problem("notes.md", text), text)

    def test_own_home_folder(self):
        with mock.patch.object(os.path, "expanduser", return_value="/home/someone"):
            self.assertIn("own home", guard.problem("run.sh", b"cd /home/someone/games"))
            self.assertIsNone(guard.problem("run.sh", b"cd /home/other/games"))

    def test_binary_files_skip_the_path_check(self):
        self.assertIsNone(guard.problem("shot.jpg", b"\xff\xd8\0\0" + rb"C:\Users\someone\x"))


if __name__ == "__main__":
    unittest.main()
