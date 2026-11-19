"""The settings panel's texts (runtime/src/settings.c): every setting has a description, every option a note,
and every character of them exists in the panel's font (runtime/src/hud.c), so nothing shows up blank."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def c_strings(text: str) -> list[str]:
    """The string literals in a piece of C, adjacent literals joined (as the compiler does)."""
    out: list[str] = []
    joined = False
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|(\s+)|(.)', text, re.S):
        if m.group(1) is not None:
            s = m.group(1).replace("\\'", "'").replace('\\"', '"')
            if joined and out:
                out[-1] += s
            else:
                out.append(s)
            joined = True
        elif m.group(2) is None:
            joined = False
    return out


def settings() -> list[dict]:
    src = (ROOT / "runtime/src/settings.c").read_text(encoding="utf-8")
    entries = []
    for var, end in (("player", "/* The developer page"), ("developer", "enum { NPLAYER")):
        body = src[src.index(f"static const Setting {var}[] = {{"):src.index(end)]
        body = body[body.index("{") + 1:]
        depth, start = 0, None
        for i, ch in enumerate(body):  # the top-level { ... } of each setting
            if ch == "{":
                if depth == 0:
                    start = i
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0 and start is not None:
                    entries.append(body[start + 1:i])
    out = []
    for e in entries:
        lists = re.findall(r"\{([^{}]*)\}", e)
        head = e[:e.index("{")]
        category, name, *help_parts = c_strings(head)
        count = int(re.search(r"\}\s*,\s*(\d+)\s*,", e).group(1))  # the number after the notes
        out.append({"category": category, "name": name, "help": "".join(help_parts),
                    "options": c_strings(lists[0]), "notes": c_strings(lists[1]), "count": count,
                    "values": c_strings(lists[2]) if len(lists) > 2 else [],
                    # a typed text, not choices (Profile > Player name, Online play's codes: a box and an Edit button, no note under it)
                    "typed": "player_name_edit" in e or "_code_edit" in e})
    return out


def font() -> set[str]:
    src = (ROOT / "runtime/src/hud.c").read_text(encoding="utf-8")
    chars = set()
    for m in re.finditer(r"\{'(\\.|[^'\\])', \{", src):
        c = m.group(1)
        chars.add(c[1] if c.startswith("\\") else c)
    return chars | {" "}


class SettingsTexts(unittest.TestCase):
    def test_every_setting_explained(self):
        rows = settings()
        self.assertGreaterEqual(len(rows), 9)
        for r in rows:
            with self.subTest(setting=r["name"]):
                self.assertGreater(len(r["help"]), 20, "no description")
                self.assertEqual(len(r["options"]), r["count"], "option count")
                self.assertEqual(len(r["notes"]), len(r["options"]), "a note per option")
                self.assertEqual(len(r["values"]), len(r["options"]), "a settings-file value per option")
                if r["typed"]:
                    continue  # its one "option" is the name itself (owner, 2026-11-19: no note, it shifted the row)
                for opt, note in zip(r["options"], r["notes"]):
                    self.assertGreater(len(note.strip()), 2, f"no note for {opt}")

    def test_texts_in_the_font(self):
        glyphs = font()
        for r in settings():
            for text in [r["category"], r["name"], r["help"], *r["options"], *r["notes"]]:
                with self.subTest(text=text):
                    missing = {c for c in text.upper() if c not in glyphs}
                    self.assertFalse(missing, f"characters without a glyph: {sorted(missing)}")

    def test_sharpening_row(self):
        """SHARPENING (decision 0076, a slider since issue #11): 0..1 stopping at its five levels (steps of 0.25),
        shown by name, never greyed out (it works with every anti-aliasing), the default MEDIUM (0.5), and settings
        files from before the slider (0..4) read as those levels."""
        src = (ROOT / "runtime/src/settings.c").read_text(encoding="utf-8")
        row = next(r for r in settings() if r["name"] == "SHARPENING")
        self.assertEqual(row["count"], 0, "a slider has no options")
        entry = src[src.index('"SHARPENING",'):]
        entry = entry[:entry.index("}},") + 3]
        self.assertIn('"YW4_SHARPEN", {NULL}, NULL, gpu_sharpen_get, gpu_sharpen_set, 0.0f, 1.0f, 0.25f,', entry,
                      "no greyed-out reason, 0..1 stopping at the five levels")
        self.assertRegex(entry, r'1, sharpen_parse, \{"OFF", "LOW", "MEDIUM", "HIGH", "MAX"\}\}')
        aa = (ROOT / "runtime/src/gpu_aa.c").read_text(encoding="utf-8")
        self.assertIn('if (!e || !*e) return 50;', aa)
        self.assertIn("else if ((p = atoi(e)) >= 1 && p <= 4) p *= 25;", aa)

    def test_sharpen_text_row(self):
        """SHARPEN TEXT AND MENUS: right under SHARPENING, OFF / ON saved as 0 / 1, off unless YW4_SHARPEN_UI=1."""
        rows = settings()
        names = [r["name"] for r in rows]
        i = names.index("SHARPEN TEXT AND MENUS")
        self.assertEqual(names[i - 1], "SHARPENING")
        self.assertEqual(rows[i]["options"], ["OFF", "ON"])
        self.assertEqual(rows[i]["values"], ["0", "1"])
        aa = (ROOT / "runtime/src/gpu_aa.c").read_text(encoding="utf-8")
        self.assertRegex(aa, r'getenv\("YW4_SHARPEN_UI"\);\s*on = e && atoi\(e\) == 1;')


if __name__ == "__main__":
    unittest.main()
