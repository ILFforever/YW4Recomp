"""runtime/src/savejournal.c: a save the game didn't commit is undone at the next start (review 2026-11-19).

Each step runs as its own process, as a crash would: the driver changes files and exits without committing, and the
next run's sj_recover must put the last committed content back."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from recomp import check

RUNTIME = check.REPO / "runtime"

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "savejournal.h"

static void put(const char *p, const char *text) { FILE *f = fopen(p, "wb"); fputs(text, f); fclose(f); }

int main(int argc, char **argv) {
    const char *root = argv[2], *step = argv[1];
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s/USERDATA00/data.bin", root);
    snprintf(b, sizeof b, "%s/AUTOSAVE/data.bin", root);
    if (!strcmp(step, "recover")) {  /* a new run mounting the save folder: twice, the second must do nothing */
        int n = sj_recover(root), again = sj_recover(root);
        printf("%d %d\n", n, again);
        return 0;
    }
    sj_recover(root);
    if (!strcmp(step, "half")) {  /* a save that stops before its Commit: one file changed, one new */
        sj_before_change(a); put(a, "HALF");
        sj_before_change(a); put(a, "HALF2");  /* a second change in the same save keeps the first backup */
        sj_before_change(b); put(b, "NEW");
        return 0;
    }
    if (!strcmp(step, "whole")) {  /* the same save, committed */
        sj_before_change(a); put(a, "SAVED");
        sj_before_change(b); put(b, "AUTO");
        sj_commit(NULL, NULL);
        return 0;
    }
    return 1;
}
"""


class SaveJournalTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        (t / "drv.c").write_text(DRIVER)
        cls.exe = t / "journal.exe"
        rt = RUNTIME / "src"
        subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", "-Werror", *([] if sys.platform == "win32" else
                        ["-D_GNU_SOURCE"]), f"-I{RUNTIME / 'include'}", f"-I{rt}", "-o", str(cls.exe), str(t / "drv.c"),
                        str(rt / "savejournal.c"), str(rt / "host.c"), "-pthread",
                        *(["-lwinmm"] if sys.platform == "win32" else [])], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(dir=self.tmp.name))
        (self.root / "USERDATA00").mkdir()
        (self.root / "AUTOSAVE").mkdir()
        (self.root / "USERDATA00" / "data.bin").write_bytes(b"OLD")

    def run_step(self, step: str) -> str:
        got = subprocess.run([str(self.exe), step, str(self.root)], capture_output=True, text=True, timeout=60)
        self.assertEqual(got.returncode, 0, got.stderr)
        return got.stdout.strip()

    def leftovers(self) -> list[str]:
        return sorted(p.name for p in self.root.rglob("*") if ".yw4" in p.name)

    def test_unfinished_save_is_undone(self):
        self.run_step("half")
        self.assertEqual(self.leftovers(), ["data.bin.yw4bak", "data.bin.yw4new"])
        self.assertEqual(self.run_step("recover"), "2 0")  # both files, and the second mount does nothing
        self.assertEqual((self.root / "USERDATA00" / "data.bin").read_bytes(), b"OLD")  # the first backup, not HALF
        self.assertFalse((self.root / "AUTOSAVE" / "data.bin").exists())  # new in the unfinished save
        self.assertEqual(self.leftovers(), [])

    def test_committed_save_stays(self):
        self.run_step("whole")
        self.assertEqual(self.leftovers(), [])
        self.assertEqual(self.run_step("recover"), "0 0")
        self.assertEqual((self.root / "USERDATA00" / "data.bin").read_bytes(), b"SAVED")
        self.assertEqual((self.root / "AUTOSAVE" / "data.bin").read_bytes(), b"AUTO")

    def test_backup_cut_short_is_never_used(self):
        (self.root / "USERDATA00" / "data.bin.yw4bak.part").write_bytes(b"OL")  # the copy was still being written
        self.assertEqual(self.run_step("recover"), "0 0")
        self.assertEqual((self.root / "USERDATA00" / "data.bin").read_bytes(), b"OLD")
        self.assertEqual(self.leftovers(), [])


if __name__ == "__main__":
    unittest.main()
