"""Which Program NCAs the build extracts from (tools/yw4/extract.py pick_programs): the base game's and the update's,
from what the two chosen files hold, merged dumps included (2026-11-20)."""

from __future__ import annotations

import unittest
from pathlib import Path

from tools.yw4.extract import ExtractError, pick_programs


def nca(name: str) -> Path:
    return Path("work/nca") / f"{name}.nca"


class PickPrograms(unittest.TestCase):
    def test_plain_base_and_update(self):
        b, u = nca("base-1111"), nca("upd-2222")
        self.assertEqual(pick_programs([("base", b), ("update", u)]), (b, u))

    def test_merged_base_with_an_older_update(self):
        # the base box's NSP holds the base game and an older update; 2.2.0 is in the update box
        b, old, new = nca("base-1111"), nca("base-3333"), nca("upd-2222")
        self.assertEqual(pick_programs([("base", b), ("update", old), ("update", new)]), (b, new))

    def test_update_box_holding_a_merged_file(self):
        # the update box's NSP holds the base game too: the base box's base game is used
        b, b2, u = nca("base-1111"), nca("upd-4444"), nca("upd-2222")
        self.assertEqual(pick_programs([("base", b), ("base", b2), ("update", u)]), (b, u))

    def test_an_update_in_the_base_box(self):
        with self.assertRaisesRegex(ExtractError, "base game is an update"):
            pick_programs([("update", nca("base-3333")), ("update", nca("upd-2222"))])

    def test_the_base_game_in_both_boxes(self):
        with self.assertRaisesRegex(ExtractError, "Both files are the base game"):
            pick_programs([("base", nca("base-1111")), ("base", nca("upd-1111"))])

    def test_no_update(self):
        b = nca("base-1111")
        self.assertEqual(pick_programs([("base", b)]), (b, None))  # the caller says the update is missing


if __name__ == "__main__":
    unittest.main()
