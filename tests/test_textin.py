"""runtime/src/textin.c: what typing and pasting put in a field. A name (bmp_only) takes no emoji: the game's font
has none, and its UTF-16 text turned one into some other character in the save (review 2026-11-19)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from recomp import check

RUNTIME = check.REPO / "runtime"

# textin.c uses SDL only for the clipboard (copy, cut): a stand-in header, no SDL needed
SDL_STUB = "#pragma once\nstatic inline int SDL_SetClipboardText(const char *t) { (void)t; return 1; }\n"

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "textin.h"

static int typed(TextField *f, const char *in, const char *want) {
    textin_clear(f);
    textin_focus(f);
    textin_type(in);
    char got[TEXTIN_MAX];
    textin_read(f, got, sizeof got, NULL, NULL);
    if (strcmp(got, want)) { printf("typed [%s]: got [%s], want [%s]\n", in, got, want); return 1; }
    return 0;
}

int main(void) {
    static TextField name, report;
    name.max_chars = 8, name.bmp_only = 1;
    int bad = 0;
    bad += typed(&name, "Na\xF0\x9F\x98\x80te", "Nate");                        /* an emoji inside */
    bad += typed(&name, "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", "");     /* a joined emoji, joiner and all */
    bad += typed(&name, "\xE2\x9D\xA4\xEF\xB8\x8F", "\xE2\x9D\xA4");             /* heart + variation selector */
    bad += typed(&name, "\xE3\x82\xB1\xE3\x83\xBC\xE3\x82\xBF", "\xE3\x82\xB1\xE3\x83\xBC\xE3\x82\xBF");  /* katakana */
    bad += typed(&name, "Jos\xC3\xA9", "Jos\xC3\xA9");                           /* accented letters stay */
    bad += typed(&name, "A\x80" "B\xE3\x82", "AB");                              /* broken sequences go */
    bad += typed(&name, "\xF0\x9F\x98\x80" "ABCDEFGHIJ", "ABCDEFGH");            /* the 8 letters, emoji not counted */
    bad += typed(&report, "ok \xF0\x9F\x98\x80", "ok \xF0\x9F\x98\x80");         /* other fields keep emoji */
    puts(bad ? "fail" : "ok");
    return bad;
}
"""


class TextinTest(unittest.TestCase):
    def test_names_take_no_emoji(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "SDL3").mkdir()
            (t / "SDL3" / "SDL.h").write_text(SDL_STUB)
            (t / "drv.c").write_text(DRIVER)
            exe = t / "textin.exe"
            rt = RUNTIME / "src"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", "-Werror",
                            *([] if sys.platform == "win32" else ["-D_GNU_SOURCE"]), f"-I{t}",
                            f"-I{RUNTIME / 'include'}", f"-I{rt}", "-o", str(exe), str(t / "drv.c"),
                            str(rt / "textin.c"), str(rt / "host.c"), "-pthread",
                            *(["-lwinmm"] if sys.platform == "win32" else [])], check=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(got.stdout.strip().splitlines()[-1:], ["ok"], got.stdout + got.stderr)


if __name__ == "__main__":
    unittest.main()
