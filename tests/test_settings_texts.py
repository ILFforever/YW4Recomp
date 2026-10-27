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
    body = src[src.index("static const Setting settings[] = {"):src.index("enum { NSETTINGS")]
    body = body[body.index("{") + 1:]
    entries, depth, start = [], 0, None
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
                    "options": c_strings(lists[0]), "notes": c_strings(lists[1]), "count": count})
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
                for opt, note in zip(r["options"], r["notes"]):
                    self.assertGreater(len(note.strip()), 2, f"no note for {opt}")

    def test_texts_in_the_font(self):
        glyphs = font()
        for r in settings():
            for text in [r["category"], r["name"], r["help"], *r["options"], *r["notes"]]:
                with self.subTest(text=text):
                    missing = {c for c in text.upper() if c not in glyphs}
                    self.assertFalse(missing, f"characters without a glyph: {sorted(missing)}")


if __name__ == "__main__":
    unittest.main()
